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

/*
  xbox_reader.cpp — Xbox controller console monitor + JSON named pipe stream (Windows)

  Features
  --------
  1) Polls a standard Xbox Wireless Controller (Series X|S compatible) via XInput (Windows stock API).
     - Works over USB or Bluetooth as long as the Microsoft XInput driver is active.
  2) Displays a fixed multi-line dashboard (htop style) in the console, updated at 10 Hz.
  3) Foreground app; exits cleanly with Ctrl+C.
  4) Exposes a simple interprocess interface via a Windows Named Pipe that streams JSON snapshots
     of the latest input state at the same rate, suitable for a Python client.
     Pipe name: \\.\pipe\XboxReaderPipe
  5) No third-party libraries required.

  Notes
  -----
  - The pipe sends *textual JSON* encoded in UTF-8, each snapshot followed by a newline.
  - The UI is double-buffered: the whole frame is built into a string then written in one call to avoid flicker.
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
static const char* kPipeName        = "\\\\.\\pipe\\XboxReaderPipe"; // final form

// Globals for clean shutdown
static std::atomic<bool> g_running{true};

// Latest JSON snapshot shared with the pipe thread
static std::mutex              g_jsonMutex;
static std::condition_variable g_jsonCv;
static std::string             g_latestJson;

// Save/restore cursor state
static CONSOLE_CURSOR_INFO g_savedCursorInfo{};
static bool g_savedCursorInfoValid = false;

// Enable ANSI VT processing (optional; not needed for our WriteConsole approach)
static void EnableAnsiVirtualTerminal()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
}

// Hide cursor
static void HideCursor()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(hOut, &ci)) {
        g_savedCursorInfo = ci;
        g_savedCursorInfoValid = true;
        ci.bVisible = FALSE;
        SetConsoleCursorInfo(hOut, &ci);
    }
}

// Restore cursor
static void RestoreCursor()
{
    if (!g_savedCursorInfoValid) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleCursorInfo(hOut, &g_savedCursorInfo);
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

// Build one textual UI frame (double-buffered)
static std::string BuildFrame(const XINPUT_STATE& st, bool connected)
{
    const XINPUT_GAMEPAD& g = st.Gamepad;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << std::setprecision(3);

    out << "Xbox Controller Monitor (XInput) — " << kUpdateHz << " Hz\n";
    out << "Named pipe: " << kPipeName << "    (Ctrl+C to exit)\n\n";

    out << "Connected: " << (connected ? "Yes" : "No") << "\n\n";

    out << "Buttons:   A:" << ((g.wButtons & XINPUT_GAMEPAD_A) ? 1 : 0)
        << "  B:" << ((g.wButtons & XINPUT_GAMEPAD_B) ? 1 : 0)
        << "  X:" << ((g.wButtons & XINPUT_GAMEPAD_X) ? 1 : 0)
        << "  Y:" << ((g.wButtons & XINPUT_GAMEPAD_Y) ? 1 : 0) << "\n";

    out << "           LB:" << ((g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0)
        << "  RB:" << ((g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0)
        << "  Back:" << ((g.wButtons & XINPUT_GAMEPAD_BACK) ? 1 : 0)
        << "  Start:" << ((g.wButtons & XINPUT_GAMEPAD_START) ? 1 : 0) << "\n";

    out << "           LS:" << ((g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) ? 1 : 0)
        << "  RS:" << ((g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) ? 1 : 0) << "\n";

    out << "DPad:      Up:" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0)
        << "  Down:" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0)
        << "  Left:" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0)
        << "  Right:" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) << "\n\n";

    out << "Triggers:  LT:" << std::setw(5) << std::setprecision(3) << normTrig(g.bLeftTrigger)
        << "   RT:" << std::setw(5) << std::setprecision(3) << normTrig(g.bRightTrigger) << "\n";

    out << "Sticks:    LX:" << std::setw(7) << normThumb(g.sThumbLX)
        << "  LY:" << std::setw(7) << normThumb(g.sThumbLY)
        << "   RX:" << std::setw(7) << normThumb(g.sThumbRX)
        << "  RY:" << std::setw(7) << normThumb(g.sThumbRY) << "\n";

    out << "\n"; // final newline
    return out.str();
}

// Format JSON snapshot (one line) — manual JSON (lightweight)
static std::string BuildJson(const XINPUT_STATE& st, bool connected)
{
    const XINPUT_GAMEPAD& g = st.Gamepad;
    std::ostringstream js;
    js.setf(std::ios::fixed);
    js << std::setprecision(6);

    js << "{";
    js << "\"connected\":" << (connected ? "true" : "false") << ",";
    js << "\"buttons\":{"
       << "\"A\":" << ((g.wButtons & XINPUT_GAMEPAD_A) ? 1 : 0) << ","
       << "\"B\":" << ((g.wButtons & XINPUT_GAMEPAD_B) ? 1 : 0) << ","
       << "\"X\":" << ((g.wButtons & XINPUT_GAMEPAD_X) ? 1 : 0) << ","
       << "\"Y\":" << ((g.wButtons & XINPUT_GAMEPAD_Y) ? 1 : 0) << ","
       << "\"LB\":" << ((g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0) << ","
       << "\"RB\":" << ((g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0) << ","
       << "\"Back\":" << ((g.wButtons & XINPUT_GAMEPAD_BACK) ? 1 : 0) << ","
       << "\"Start\":" << ((g.wButtons & XINPUT_GAMEPAD_START) ? 1 : 0) << ","
       << "\"LS\":" << ((g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) ? 1 : 0) << ","
       << "\"RS\":" << ((g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) ? 1 : 0) << ","
       << "\"DpadUp\":" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0) << ","
       << "\"DpadDown\":" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0) << ","
       << "\"DpadLeft\":" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0) << ","
       << "\"DpadRight\":" << ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0)
       << "},";
    js << "\"triggers\":{\"LT\":" << normTrig(g.bLeftTrigger)
       << ",\"RT\":" << normTrig(g.bRightTrigger) << "},";
    js << "\"sticks\":{\"LX\":" << normThumb(g.sThumbLX)
       << ",\"LY\":" << normThumb(g.sThumbLY)
       << ",\"RX\":" << normThumb(g.sThumbRX)
       << ",\"RY\":" << normThumb(g.sThumbRY) << "}";
    js << "}";
    return js.str();
}

// Named pipe writer thread: waits for client & streams snapshots
static void PipeThread()
{
    for (; g_running.load(); ) {
        // Create a fresh instance so new clients can connect after disconnect
        HANDLE hPipe = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,      // max instances
            16*1024,// out buffer
            16*1024,// in buffer
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            // If creation failed, wait and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        // Wait for a client to connect (blocks here)
        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        // Stream JSON snapshots while client is connected
        std::unique_lock<std::mutex> lk(g_jsonMutex);
        while (g_running.load()) {
            // Wait for an update (notified by main loop)
            g_jsonCv.wait(lk);
            if (!g_running.load()) break;
            std::string snapshot = g_latestJson; // copy snapshot under lock
            lk.unlock();

            // Write snapshot + newline (ensure utf-8 JSON text)
            DWORD written = 0;
            BOOL ok = WriteFile(hPipe, snapshot.data(), (DWORD)snapshot.size(), &written, nullptr);
            if (ok) {
                const char nl = '\n';
                ok = WriteFile(hPipe, &nl, 1, &written, nullptr);
            }
            if (!ok) {
                // client disconnected or error
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
    // Setup console + signal handler
    EnableAnsiVirtualTerminal(); // optional
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    HideCursor();

    // Print a small header (will be overwritten by double-buffered frames)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dummy = 0;
    const char *startup = "Xbox Controller Monitor (starting)...\n";
    WriteConsoleA(hOut, startup, (DWORD)strlen(startup), &dummy, nullptr);

    // Start pipe thread
    std::thread pipeThr(PipeThread);

    const auto interval = std::chrono::milliseconds(1000 / kUpdateHz);

    // Main polling loop
    while (g_running.load()) {
        XINPUT_STATE state{};
        DWORD res = XInputGetState(kControllerIndex, &state);
        bool connected = (res == ERROR_SUCCESS);

        // Build UI frame and JSON snapshot
        std::string frame = BuildFrame(state, connected);
        std::string js = BuildJson(state, connected);

        // Write frame in one go to console (double-buffered)
        // Move cursor to home (0,0) using SetConsoleCursorPosition for less jumpiness
        COORD home = {0, 0};
        SetConsoleCursorPosition(hOut, home);
        // Use WriteConsoleA to minimize intermediate cursor movement
        DWORD written = 0;
        WriteConsoleA(hOut, frame.c_str(), (DWORD)frame.size(), &written, nullptr);

        // Publish JSON snapshot for pipe thread
        {
            std::lock_guard<std::mutex> lk(g_jsonMutex);
            g_latestJson = js;
        }
        g_jsonCv.notify_all();

        std::this_thread::sleep_for(interval);
    }

    // Shutdown
    g_jsonCv.notify_all();
    if (pipeThr.joinable()) pipeThr.join();

    RestoreCursor();

    // Move cursor to next line then exit
    const char *bye = "\nExiting...\n";
    DWORD dw;
    WriteConsoleA(hOut, bye, (DWORD)strlen(bye), &dw, nullptr);

    return 0;
}
