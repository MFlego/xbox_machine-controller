// xbox_reader.cpp
//
// License: MIT License
//
// Copyright (c) 2025
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// ---
// References:
// - Microsoft XInput API documentation (public domain):
//   https://learn.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput
//
// This program demonstrates how to:
//   1. Poll input from an Xbox Series S/X controller using XInput.
//   2. Display all button/axis values as a live console dashboard (htop-style).
//   3. Refresh output at ~10Hz without flicker.
//   4. Provide JSON-formatted state via a Windows named pipe for use by external apps (e.g. Python).
//   5. Run until terminated by Ctrl+C.

#include <windows.h>
#include <Xinput.h>
#include <cstdio>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>
#include <sstream>

// Link against XInput library
#pragma comment(lib, "Xinput9_1_0.lib")

// Global flag for clean exit
static bool g_running = true;

// Signal handler for Ctrl+C
void signal_handler(int) {
    g_running = false;
}

// Normalize thumbstick value to [-1.0, 1.0]
float normThumb(SHORT v) {
    return (v >= 0) ? (float)v / 32767.0f : (float)v / 32768.0f;
}

// Normalize trigger value to [0.0, 1.0]
float normTrig(BYTE v) {
    return (float)v / 255.0f;
}

// Named pipe path
const char* kPipeName = R"(\\\\.\\pipe\\XboxControllerPipe)";

// Thread to handle pipe communication
void pipeThread() {
    HANDLE hPipe = CreateNamedPipeA(
        kPipeName,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to create named pipe. Error: %lu\n", GetLastError());
        return;
    }

    while (g_running) {
        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            while (g_running) {
                XINPUT_STATE state;
                ZeroMemory(&state, sizeof(state));
                DWORD res = XInputGetState(0, &state);
                if (res == ERROR_SUCCESS) {
                    // Build JSON output
                    std::ostringstream oss;
                    oss << "{";
                    oss << "\"A\":" << ((state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 1 : 0) << ",";
                    oss << "\"B\":" << ((state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 1 : 0) << ",";
                    oss << "\"X\":" << ((state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 1 : 0) << ",";
                    oss << "\"Y\":" << ((state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 1 : 0) << ",";
                    oss << "\"LX\":" << normThumb(state.Gamepad.sThumbLX) << ",";
                    oss << "\"LY\":" << normThumb(state.Gamepad.sThumbLY) << ",";
                    oss << "\"RX\":" << normThumb(state.Gamepad.sThumbRX) << ",";
                    oss << "\"RY\":" << normThumb(state.Gamepad.sThumbRY) << ",";
                    oss << "\"LT\":" << normTrig(state.Gamepad.bLeftTrigger) << ",";
                    oss << "\"RT\":" << normTrig(state.Gamepad.bRightTrigger);
                    oss << "}";
                    std::string msg = oss.str();
                    DWORD written;
                    WriteFile(hPipe, msg.c_str(), (DWORD)msg.size(), &written, NULL);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    CloseHandle(hPipe);
}

int main() {
    signal(SIGINT, signal_handler);

    // Start pipe thread for JSON interface
    std::thread t(pipeThread);

    const int kUpdateHz = 60;
    const auto interval = std::chrono::milliseconds(1000 / kUpdateHz);
    // Before entering main loop
    printf("\x1b[?25l"); // hide cursor

    // Print dashboard header once and clear screen
    printf("\x1b[2J\x1b[H");
    printf("Xbox Controller Monitor (XInput)\n\n");
    printf("Connected: \n\n");
    printf("Buttons:   A:   B:   X:   Y:    LB:   RB:   Back:   Start:   LS:   RS:\n");
    printf("DPad:      Up:   Down:   Left:   Right:\n\n");
    printf("Triggers:  LT:        RT:\n");
    printf("Sticks:    LX:        LY:        RX:        RY:\n");
    fflush(stdout);

    while (g_running) {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        DWORD res = XInputGetState(0, &state);
        bool connected = (res == ERROR_SUCCESS);

        auto& g = state.Gamepad;
        auto bit = [&](WORD mask) { return (g.wButtons & mask) ? 1 : 0; };

        // Cursor back to home and redraw values
        printf("\x1b[H");
        printf("Xbox Controller Monitor (XInput)\n\n");
        printf("Connected: %d\n\n", connected ? 1 : 0);

        printf("Buttons:   A:%d  B:%d  X:%d  Y:%d   LB:%d RB:%d  Back:%d Start:%d  LS:%d RS:%d\n",
               bit(XINPUT_GAMEPAD_A), bit(XINPUT_GAMEPAD_B), bit(XINPUT_GAMEPAD_X),
               bit(XINPUT_GAMEPAD_Y), bit(XINPUT_GAMEPAD_LEFT_SHOULDER),
               bit(XINPUT_GAMEPAD_RIGHT_SHOULDER), bit(XINPUT_GAMEPAD_BACK),
               bit(XINPUT_GAMEPAD_START), bit(XINPUT_GAMEPAD_LEFT_THUMB),
               bit(XINPUT_GAMEPAD_RIGHT_THUMB));

        printf("DPad:      Up:%d Down:%d Left:%d Right:%d\n\n",
               bit(XINPUT_GAMEPAD_DPAD_UP), bit(XINPUT_GAMEPAD_DPAD_DOWN),
               bit(XINPUT_GAMEPAD_DPAD_LEFT), bit(XINPUT_GAMEPAD_DPAD_RIGHT));

        printf("Triggers:  LT:%6.3f   RT:%6.3f\n",
               normTrig(g.bLeftTrigger), normTrig(g.bRightTrigger));

        printf("Sticks:    LX:%6.3f  LY:%6.3f   RX:%6.3f  RY:%6.3f\n",
               normThumb(g.sThumbLX), normThumb(g.sThumbLY),
               normThumb(g.sThumbRX), normThumb(g.sThumbRY));

        fflush(stdout);
        std::this_thread::sleep_for(interval);

    }
    // In cleanup (after loop, or in signal handler)
    printf("\x1b[?25h"); // show cursor again
    
    t.join();
    return 0;
}
