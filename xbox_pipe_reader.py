# xbox_pipe_reader.py
# Requires pywin32: pip install pywin32

import time
import json
import win32file
import win32pipe
import pywintypes

PIPE_NAME = r"\\.\pipe\XboxReaderPipe"

def main():
    print("Waiting for Xbox Reader pipe... (will retry until pipe exists)")

    # Try to open the pipe until server has created it
    while True:
        try:
            handle = win32file.CreateFile(
                PIPE_NAME,
                win32file.GENERIC_READ,
                0,               # no sharing
                None,
                win32file.OPEN_EXISTING,
                0,
                None
            )
            break
        except pywintypes.error as e:
            # ERROR_FILE_NOT_FOUND (2) means pipe not yet created
            time.sleep(0.3)
        except Exception as e:
            print("CreateFile error:", e)
            time.sleep(0.5)

    print("Connected to pipe:", PIPE_NAME)

    # We'll read raw bytes and split on newline.
    buffer = b""
    try:
        while True:
            try:
                hr, data = win32file.ReadFile(handle, 4096, None)
            except pywintypes.error as e:
                # if client/server disconnect or error, break/exit
                print("ReadFile error:", e)
                break

            if not data:
                # no data, wait a bit
                time.sleep(0.05)
                continue

            buffer += data
            # split on newline(s)
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line:
                    continue
                try:
                    text = line.decode("utf-8")
                except UnicodeDecodeError as ude:
                    print("Unicode decode error for a frame, skipping:", ude)
                    continue
                try:
                    obj = json.loads(text)
                    print("JSON:", obj)
                except json.JSONDecodeError:
                    print("Invalid JSON frame:", text)
    finally:
        try:
            win32file.CloseHandle(handle)
        except Exception:
            pass

if __name__ == "__main__":
    main()
