/*
  xbox_reader.cpp — Xbox controller console monitor + JSON named pipe stream (Windows)

  Features
  --------
  1) Polls a standard Xbox Wireless Controller (Series X|S compatible) via XInput (Windows stock API).
     - Works over USB or Bluetooth as long as the Microsoft XInput driver is active.
  2) Prints a single updating line in the console with all labeled inputs (10 Hz by default).
  3) Foreground app; exits cleanly with Ctrl+C.
  4) Exposes a simple interprocess interface via a Windows Named Pipe that streams JSON snapshots
     of the latest input state at the same rate, suitable for a Python client.
     Pipe name: \\?\pipe\XboxControllerState  (also accessible as \\.\pipe\XboxControllerState)
  5) No third‑party libraries required.

  Licensing / References
  ----------------------
  - This file is provided under the MIT License (see bottom of file).
  - Uses Microsoft XInput API (header <Xinput.h>, link Xinput9_1_0.lib) which is part of the Windows SDK
    and subject to Microsoft's terms/EULA. No third‑party open‑source code is included.
*/

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <Xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "Xinput9_1_0.lib")

// --- Config ---
static const int   kControllerIndex = 0;      // XInput supports up to 4 controllers (0..3)
static const int   kUpdateHz        = 10;     // refresh rate
static const char* kPipeName        = R"(\\?\pipe\XboxControllerState)"; // Preferred long-path form

// Globals for clean shutdown
static std::atomic<bool> g_running{true};

// Latest JSON snapshot shared with the pipe thread
static std::mutex              g_jsonMutex;
static std::condition_variable g_jsonCv;
static std::string             g_latestJson;

// Enable ANSI escape sequences for nicer single-line updates
static void EnableAnsiVirtualTerminal()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
}

// Ctrl+C handler
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        g_jsonCv.notify_all();
        return TRUE;
    }
    return FALSE;
}

// Normalize thumb values [-32768, 32767] -> [-1.0, 1.0]
static inline double normThumb(SHORT v)
{
    return (v >= 0) ? (double)v / 32767.0 : (double)v / 32768.0;
}

// Normalize trigger [0, 255] -> [0.0, 1.0]
static inline double normTrig(BYTE v)
{
    return (double)v / 255.0;
}

// Format the current state into a human-readable single line and JSON
static void FormatState(const XINPUT_STATE& st, bool connected, std::string& outLine, std::string& outJson)
{
    const XINPUT_GAMEPAD& g = st.Gamepad;
    auto bit = [&](WORD m){ return (g.wButtons & m) ? 1 : 0; };

    // Human-friendly line
    std::ostringstream line;
    line.setf(std::ios::fixed); line << std::setprecision(3);
    line << "Conn:" << (connected ? 1 : 0)
         << " A:"     << bit(XINPUT_GAMEPAD_A)
         << " B:"     << bit(XINPUT_GAMEPAD_B)
         << " X:"     << bit(XINPUT_GAMEPAD_X)
         << " Y:"     << bit(XINPUT_GAMEPAD_Y)
         << " LB:"    << bit(XINPUT_GAMEPAD_LEFT_SHOULDER)
         << " RB:"    << bit(XINPUT_GAMEPAD_RIGHT_SHOULDER)
         << " Back:"  << bit(XINPUT_GAMEPAD_BACK)
         << " Start:" << bit(XINPUT_GAMEPAD_START)
         << " LS:"    << bit(XINPUT_GAMEPAD_LEFT_THUMB)
         << " RS:"    << bit(XINPUT_GAMEPAD_RIGHT_THUMB)
         << " Du:"    << bit(XINPUT_GAMEPAD_DPAD_UP)
         << " Dd:"    << bit(XINPUT_GAMEPAD_DPAD_DOWN)
         << " Dl:"    << bit(XINPUT_GAMEPAD_DPAD_LEFT)
         << " Dr:"    << bit(XINPUT_GAMEPAD_DPAD_RIGHT)
         << " LT:"    << normTrig(g.bLeftTrigger)
         << " RT:"    << normTrig(g.bRightTrigger)
         << " LX:"    << normThumb(g.sThumbLX)
         << " LY:"    << normThumb(g.sThumbLY)
         << " RX:"    << normThumb(g.sThumbRX)
         << " RY:"    << normThumb(g.sThumbRY);
    outLine = line.str();

    // Minimal JSON snapshot for other processes
    std::ostringstream js;
    js.setf(std::ios::fixed); js << std::setprecision(6);
    js << "{\n";
    js << "  \"connected\": " << (connected?"true":"false") << ",\n";
    js << "  \"buttons\": {"
       << "\"A\":" << bit(XINPUT_GAMEPAD_A) << ", "
       << "\"B\":" << bit(XINPUT_GAMEPAD_B) << ", "
       << "\"X\":" << bit(XINPUT_GAMEPAD_X) << ", "
       << "\"Y\":" << bit(XINPUT_GAMEPAD_Y) << ", "
       << "\"LB\":" << bit(XINPUT_GAMEPAD_LEFT_SHOULDER) << ", "
       << "\"RB\":" << bit(XINPUT_GAMEPAD_RIGHT_SHOULDER) << ", "
       << "\"Back\":" << bit(XINPUT_GAMEPAD_BACK) << ", "
       << "\"Start\":" << bit(XINPUT_GAMEPAD_START) << ", "
       << "\"LS\":" << bit(XINPUT_GAMEPAD_LEFT_THUMB) << ", "
       << "\"RS\":" << bit(XINPUT_GAMEPAD_RIGHT_THUMB) << ", "
       << "\"DpadUp\":" << bit(XINPUT_GAMEPAD_DPAD_UP) << ", "
       << "\"DpadDown\":" << bit(XINPUT_GAMEPAD_DPAD_DOWN) << ", "
       << "\"DpadLeft\":" << bit(XINPUT_GAMEPAD_DPAD_LEFT) << ", "
       << "\"DpadRight\":" << bit(XINPUT_GAMEPAD_DPAD_RIGHT) << "},\n";
    js << "  \"triggers\": {\"LT\": " << normTrig(g.bLeftTrigger)
       << ", \"RT\": " << normTrig(g.bRightTrigger) << "},\n";
    js << "  \"sticks\": {\"LX\": " << normThumb(g.sThumbLX)
       << ", \"LY\": " << normThumb(g.sThumbLY)
       << ", \"RX\": " << normThumb(g.sThumbRX)
       << ", \"RY\": " << normThumb(g.sThumbRY) << "}\n";
    js << "}\n";
    outJson = js.str();
}

// Named pipe writer thread: sends latest JSON snapshot to any connected client
static void PipeThread()
{
    for (; g_running.load(); ) {
        HANDLE hPipe = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,      // max instances
            64*1024,// out buffer
            64*1024,// in buffer
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            // If pipe exists already (e.g., quick restart), wait a bit and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        // Connected: stream latest JSON on each update until client disconnects or we exit
        DWORD written = 0;
        std::unique_lock<std::mutex> lk(g_jsonMutex);
        while (g_running.load()) {
            // Wait for a state update
            g_jsonCv.wait(lk);
            if (!g_running.load()) break;
            std::string snapshot = g_latestJson; // copy under lock
            lk.unlock();

            // Write snapshot followed by a newline delimiter
            BOOL ok = WriteFile(hPipe, snapshot.data(), (DWORD)snapshot.size(), &written, nullptr);
            if (ok) {
                const char nl = '\n';
                ok = WriteFile(hPipe, &nl, 1, &written, nullptr);
            }
            if (!ok) {
                // Client likely disconnected
                break;
            }
            lk.lock();
        }
        lk.unlock();
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

int main()
{
    EnableAnsiVirtualTerminal();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    printf("Xbox Controller Monitor (XInput) — 10 Hz\n");
    printf("Named pipe: %s\n", kPipeName);
    printf("Press Ctrl+C to exit.\n\n");

    // Start pipe thread
    std::thread pipeThr(PipeThread);

    const auto interval = std::chrono::milliseconds(1000 / kUpdateHz);

    while (g_running.load()) {
        XINPUT_STATE state{};
        DWORD res = XInputGetState(kControllerIndex, &state);
        bool connected = (res == ERROR_SUCCESS);

        if (!connected) {
            // Zero the struct so formatting is stable when disconnected
            ZeroMemory(&state, sizeof(state));
        }

        std::string line, json;
        FormatState(state, connected, line, json);

        // Single-line update: erase line (ESC[2K) and carriage return
        printf("\x1b[2K\r%s", line.c_str());
        fflush(stdout);

        // Publish JSON to pipe thread
        {
            std::lock_guard<std::mutex> lk(g_jsonMutex);
            g_latestJson = json;
        }
        g_jsonCv.notify_all();

        std::this_thread::sleep_for(interval);
    }

    // Clean shutdown
    g_jsonCv.notify_all();
    if (pipeThr.joinable()) pipeThr.join();

    printf("\nExiting...\n");
    return 0;
}

/*
MIT License

Copyright (c) 2025

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
