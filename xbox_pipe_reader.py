import time
import json
import win32file  # from pywin32
import win32pipe

# PIPE_NAME = r"\\.\pipe\XboxControllerState"
PIPE_NAME = r"\\.\pipe\XboxReaderPipe"

def main():
    print("Waiting for Xbox Reader pipe...")
    handle = win32file.CreateFile(
        PIPE_NAME,
        win32file.GENERIC_READ,
        0, None,
        win32file.OPEN_EXISTING,
        0, None
    )
    print("Connected to pipe.")

    buffer = b""
    while True:
        # Read chunks from pipe
        hr, data = win32file.ReadFile(handle, 4096)
        if not data:
            time.sleep(0.1)
            continue
        buffer += data
        # Split on newlines
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            try:
                obj = json.loads(line.decode("utf-8"))
                print("State:", obj)
            except json.JSONDecodeError:
                print("Bad JSON:", line)

if __name__ == "__main__":
    main()
