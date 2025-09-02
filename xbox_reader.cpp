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
#include <future>

#pragma comment(lib, "Xinput9_1_0.lib")

// Utility functions for normalizing input values
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

// Interface for controller input backends
class IControllerInput {
public:
    struct State {
        bool connected = false;
        struct {
            bool A, B, X, Y;
            bool LB, RB;
            bool Back, Start;
            bool LS, RS;
            bool DpadUp, DpadDown, DpadLeft, DpadRight;
        } buttons{};
        struct {
            double LT, RT;
        } triggers{};
        struct {
            double LX, LY;
            double RX, RY;
        } sticks{};
    };

    virtual ~IControllerInput() = default;
    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual bool poll(State& state) = 0;
};

// XInput-specific implementation
class XInputController : public IControllerInput {
private:
    const int m_controllerIndex;

public:
    explicit XInputController(int controllerIndex = 0) 
        : m_controllerIndex(controllerIndex) {}

    bool init() override { return true; }
    void shutdown() override {}
    
    bool poll(State& state) override {
        XINPUT_STATE xstate{};
        DWORD res = XInputGetState(m_controllerIndex, &xstate);
        state.connected = (res == ERROR_SUCCESS);
        
        if (state.connected) {
            const auto& pad = xstate.Gamepad;
            // Buttons
            state.buttons.A = (pad.wButtons & XINPUT_GAMEPAD_A) != 0;
            state.buttons.B = (pad.wButtons & XINPUT_GAMEPAD_B) != 0;
            state.buttons.X = (pad.wButtons & XINPUT_GAMEPAD_X) != 0;
            state.buttons.Y = (pad.wButtons & XINPUT_GAMEPAD_Y) != 0;
            state.buttons.LB = (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            state.buttons.RB = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            state.buttons.Back = (pad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            state.buttons.Start = (pad.wButtons & XINPUT_GAMEPAD_START) != 0;
            state.buttons.LS = (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            state.buttons.RS = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
            state.buttons.DpadUp = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            state.buttons.DpadDown = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            state.buttons.DpadLeft = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            state.buttons.DpadRight = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

            // Triggers and sticks
            state.triggers.LT = normTrig(pad.bLeftTrigger);
            state.triggers.RT = normTrig(pad.bRightTrigger);
            state.sticks.LX = normThumb(pad.sThumbLX);
            state.sticks.LY = normThumb(pad.sThumbLY);
            state.sticks.RX = normThumb(pad.sThumbRX);
            state.sticks.RY = normThumb(pad.sThumbRY);
        }
        return state.connected;
    }
};

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
static DWORD g_savedConsoleMode = 0;
static bool g_savedConsoleModeValid = false;

// Initialize console for our application
static void InitializeConsole()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    // Save current console mode
    if (GetConsoleMode(hOut, &g_savedConsoleMode)) {
        g_savedConsoleModeValid = true;
    }

    // Set up our preferred console mode
    DWORD mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    SetConsoleMode(hOut, mode);

    // Save and hide cursor
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(hOut, &ci)) {
        g_savedCursorInfo = ci;
        g_savedCursorInfoValid = true;
        ci.bVisible = FALSE;
        SetConsoleCursorInfo(hOut, &ci);
    }
}

// Restore console to its original state
static void RestoreConsole()
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    // Restore cursor
    if (g_savedCursorInfoValid) {
        SetConsoleCursorInfo(hOut, &g_savedCursorInfo);
        g_savedCursorInfoValid = false;
    }

    // Restore console mode
    if (g_savedConsoleModeValid) {
        SetConsoleMode(hOut, g_savedConsoleMode);
        g_savedConsoleModeValid = false;
    }
}

// Ctrl+C handler
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        // Get handle and clear screen
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        
        // Clear entire screen buffer
        DWORD written;
        DWORD length = csbi.dwSize.X * csbi.dwSize.Y;
        COORD topLeft = {0, 0};
        FillConsoleOutputCharacter(hOut, ' ', length, topLeft, &written);
        FillConsoleOutputAttribute(hOut, csbi.wAttributes, length, topLeft, &written);
        
        // Reset cursor and make it visible
        SetConsoleCursorPosition(hOut, topLeft);
        CONSOLE_CURSOR_INFO cursorInfo = {100, TRUE};
        SetConsoleCursorInfo(hOut, &cursorInfo);
        
        // Signal shutdown
        g_running.store(false);
        g_jsonCv.notify_all();

        // Print immediate feedback
        const char *msg = "Shutting down...\n";
        WriteConsoleA(hOut, msg, (DWORD)strlen(msg), &written, nullptr);
        
        // Force exit after a brief timeout
        std::thread([]{
            Sleep(500);  // Give main thread 500ms to clean up
            ExitProcess(0);  // Force exit if cleanup takes too long
        }).detach();
        
        return TRUE;
    }
    return FALSE;
}

// Build one textual UI frame (double-buffered)
static std::string BuildFrame(const IControllerInput::State& state)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out << std::setprecision(3);

    out << "Xbox Controller Monitor (XInput) — " << kUpdateHz << " Hz\n";
    out << "Named pipe: " << kPipeName << "    (Ctrl+C to exit)\n\n";

    out << "Connected: " << (state.connected ? "Yes" : "No") << "\n\n";

    out << "Buttons:   A:" << state.buttons.A
        << "  B:" << state.buttons.B
        << "  X:" << state.buttons.X
        << "  Y:" << state.buttons.Y << "\n";

    out << "           LB:" << state.buttons.LB
        << "  RB:" << state.buttons.RB
        << "  Back:" << state.buttons.Back
        << "  Start:" << state.buttons.Start << "\n";

    out << "           LS:" << state.buttons.LS
        << "  RS:" << state.buttons.RS << "\n";

    out << "DPad:      Up:" << state.buttons.DpadUp
        << "  Down:" << state.buttons.DpadDown
        << "  Left:" << state.buttons.DpadLeft
        << "  Right:" << state.buttons.DpadRight << "\n\n";

    out << "Triggers:  LT:" << std::setw(5) << std::setprecision(3) << state.triggers.LT
        << "   RT:" << std::setw(5) << std::setprecision(3) << state.triggers.RT << "\n";

    out << "Sticks:    LX:" << std::setw(7) << state.sticks.LX
        << "  LY:" << std::setw(7) << state.sticks.LY
        << "   RX:" << std::setw(7) << state.sticks.RX
        << "  RY:" << std::setw(7) << state.sticks.RY << "\n";

    out << "\n"; // final newline
    return out.str();
}

// Format JSON snapshot (one line) — manual JSON (lightweight)
static std::string BuildJson(const IControllerInput::State& state)
{
    std::ostringstream js;
    js.setf(std::ios::fixed);
    js << std::setprecision(6);

    js << "{";
    js << "\"connected\":" << (state.connected ? "true" : "false") << ",";
    js << "\"buttons\":{"
       << "\"A\":" << state.buttons.A << ","
       << "\"B\":" << state.buttons.B << ","
       << "\"X\":" << state.buttons.X << ","
       << "\"Y\":" << state.buttons.Y << ","
       << "\"LB\":" << state.buttons.LB << ","
       << "\"RB\":" << state.buttons.RB << ","
       << "\"Back\":" << state.buttons.Back << ","
       << "\"Start\":" << state.buttons.Start << ","
       << "\"LS\":" << state.buttons.LS << ","
       << "\"RS\":" << state.buttons.RS << ","
       << "\"DpadUp\":" << state.buttons.DpadUp << ","
       << "\"DpadDown\":" << state.buttons.DpadDown << ","
       << "\"DpadLeft\":" << state.buttons.DpadLeft << ","
       << "\"DpadRight\":" << state.buttons.DpadRight
       << "},";
    js << "\"triggers\":{\"LT\":" << state.triggers.LT
       << ",\"RT\":" << state.triggers.RT << "},";
    js << "\"sticks\":{\"LX\":" << state.sticks.LX
       << ",\"LY\":" << state.sticks.LY
       << ",\"RX\":" << state.sticks.RX
       << ",\"RY\":" << state.sticks.RY << "}";
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
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    InitializeConsole();

    // Print a small header (will be overwritten by double-buffered frames)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dummy = 0;
    const char *startup = "Xbox Controller Monitor (starting)...\n";
    WriteConsoleA(hOut, startup, (DWORD)strlen(startup), &dummy, nullptr);

    // Start pipe thread
    std::thread pipeThr(PipeThread);

    const auto interval = std::chrono::milliseconds(1000 / kUpdateHz);

    // Create controller input interface
    auto controller = std::make_unique<XInputController>(kControllerIndex);
    if (!controller->init()) {
        const char *err = "Failed to initialize controller input\n";
        WriteConsoleA(hOut, err, (DWORD)strlen(err), &dummy, nullptr);
        return 1;
    }

    // Main polling loop
    while (g_running.load()) {
        IControllerInput::State state{};
        controller->poll(state);

        // Build UI frame and JSON snapshot
        std::string frame = BuildFrame(state);
        std::string js = BuildJson(state);

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

    // Shutdown sequence with timeout
    auto shutdownStart = std::chrono::steady_clock::now();
    const auto shutdownTimeout = std::chrono::milliseconds(1000); // 1 second timeout
    
    g_running.store(false);
    g_jsonCv.notify_all();
    
    // Cleanup with timeout checks
    if (controller) {
        controller->shutdown();
        controller.reset();
    }
    
    // Wait for pipe thread with timeout
    if (pipeThr.joinable()) {
        std::thread([&pipeThr]{
            pipeThr.join();
        }).detach();
    }

    // Get console info for proper clearing
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    
    // Clear entire screen buffer
    DWORD written;
    DWORD length = csbi.dwSize.X * csbi.dwSize.Y;
    COORD topLeft = {0, 0};
    FillConsoleOutputCharacter(hOut, ' ', length, topLeft, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, length, topLeft, &written);
    
    // Reset cursor position to top
    SetConsoleCursorPosition(hOut, topLeft);
    
    // Restore cursor visibility
    CONSOLE_CURSOR_INFO cursorInfo = {100, TRUE};
    SetConsoleCursorInfo(hOut, &cursorInfo);
    
    // Print final message
    const char *bye = "Xbox Controller Monitor: Shutdown complete.\n";
    WriteConsoleA(hOut, bye, (DWORD)strlen(bye), &written, nullptr);
    
    // If we're taking too long, just exit
    if (std::chrono::steady_clock::now() - shutdownStart > shutdownTimeout) {
        ExitProcess(0);
    }

    return 0;
}
