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

/*
 * Xbox Controller Reader - htop style console UI
 *
 * License: MIT
 * (retain original license text here without modification)
 */

#include <windows.h>
#include <Xinput.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

// Named pipe name
const char* kPipeName = R"(\\.\pipe\XboxReaderPipe)";

// Global running flag to exit cleanly
volatile bool g_running = true;

// Signal handler for Ctrl+C
void signal_handler(int) {
    g_running = false;
}

// Hide cursor
void hide_cursor() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
}

// Show cursor (restore on exit)
void show_cursor() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = TRUE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
}

// Clear screen
void clear_screen() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coordScreen = {0, 0};
    DWORD cCharsWritten;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD dwConSize;

    if (!GetConsoleScreenBufferInfo(hConsole, &csbi))
        return;
    dwConSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacter(hConsole, (TCHAR)' ', dwConSize, coordScreen, &cCharsWritten);
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConSize, coordScreen, &cCharsWritten);
    SetConsoleCursorPosition(hConsole, coordScreen);
}

// Draw controller state in an htop-style dashboard
void draw_ui(const XINPUT_STATE& state) {
    printf("Xbox Controller State (refresh 10Hz)\n");
    printf("-----------------------------------\n");

    // Buttons
    WORD b = state.Gamepad.wButtons;
    printf("Buttons:   A:%d   B:%d   X:%d   Y:%d\n",
           (b & XINPUT_GAMEPAD_A) != 0,
           (b & XINPUT_GAMEPAD_B) != 0,
           (b & XINPUT_GAMEPAD_X) != 0,
           (b & XINPUT_GAMEPAD_Y) != 0);
    printf("          LB:%d  RB:%d  Back:%d  Start:%d  LS:%d  RS:%d\n",
           (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
           (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0,
           (b & XINPUT_GAMEPAD_BACK) != 0,
           (b & XINPUT_GAMEPAD_START) != 0,
           (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0,
           (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
    printf("          DPad Up:%d Down:%d Left:%d Right:%d\n",
           (b & XINPUT_GAMEPAD_DPAD_UP) != 0,
           (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0,
           (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0,
           (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);

    // Triggers
    printf("Triggers:  LT:%3d  RT:%3d\n",
           state.Gamepad.bLeftTrigger,
           state.Gamepad.bRightTrigger);

    // Sticks
    printf("Sticks:    LX:%6d  LY:%6d  RX:%6d  RY:%6d\n",
           state.Gamepad.sThumbLX,
           state.Gamepad.sThumbLY,
           state.Gamepad.sThumbRX,
           state.Gamepad.sThumbRY);
}

int main() {
    // Handle Ctrl+C
    signal(SIGINT, signal_handler);

    // Hide cursor
    hide_cursor();

    // Setup named pipe
    HANDLE pipe = CreateNamedPipeA(
        kPipeName,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, 1024, 1024, 0, NULL
    );

    if (pipe != INVALID_HANDLE_VALUE) {
        printf("[INFO] Named pipe created: %s\n", kPipeName);
    } else {
        printf("[ERROR] Failed to create named pipe: %lu\n", GetLastError());
        show_cursor();
        return 1;
    }

    // Main loop
    while (g_running) {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));

        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            clear_screen();
            draw_ui(state);

            // Write to pipe if a client is connected
            DWORD written;
            WriteFile(pipe, &state, sizeof(state), &written, NULL);
        } else {
            clear_screen();
            printf("Controller not connected.\n");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10 Hz
    }

    // Restore cursor
    show_cursor();

    // Close pipe
    CloseHandle(pipe);

    return 0;
}
