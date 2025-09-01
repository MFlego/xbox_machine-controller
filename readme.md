# xbox_reader notes #

Compilation command - g++ -std=c++17 -Wall -O2 xbox_reader.cpp -o xbox_reader.exe -lxinput9_1_0


## Notes & testing checklist ## 

- Start order: Start xbox_reader.exe first. It will create the named pipe and begin writing frames (it spawns a background pipe thread that accepts clients).

- Run Python: Run python xbox_pipe_reader.py. The script will retry until the pipe exists and then attach and print each JSON object it receives.

- No UnicodeDecodeError: Because the C++ program now sends textual UTF-8 JSON followed by \n, the Python reader decodes lines; there should be no decoding error.

- No flicker: The UI frame is built in memory and written to the console in one WriteConsoleA call, and the cursor is hidden while running — this should eliminate the flicker and cursor jumping you observed.

- Normalized stick values: The UI prints normalized floats in [-1.000, +1.000], and the JSON uses high-precision floats for programmatic use.

## starting prompt for xbox_machine_controller (part 1) ##

You are assisting with the development of the Xbox Machine Controller project. The repository contains two working programs:

xbox_reader.cpp
- Reads Xbox controller input using XInput.
- Provides an htop-style live console UI with minimal flicker.
- Streams controller state as JSON through a Windows named pipe.
- Must remain efficient, reliable, and eventually portable across Windows, macOS, Linux, and even embedded systems.
- To achieve portability, the current XInput backend will need to be abstracted away so that other input APIs (e.g., SDL2, evdev, HID) can be swapped in.

xbox_pipe_reader.py
- Connects to the named pipe and parses the JSON stream.
- Intended as an extensible interface layer for controlling CNC machines, industrial robots, and other fabrication machinery.
- While currently Windows + pipes, the architecture should be designed to support alternative transports in the future: BLE, UART, TCP/IP, CAN bus, I²C, or other IPC/fieldbus systems.

Goals for this phase of development:
- Make the C++ program as self-contained and cross-platform as possible.
- Introduce an abstraction layer for controller input so that future backends (non-XInput) can be added cleanly.
- Begin defining a transport abstraction layer for the Python interface, so it can flexibly switch between pipes, serial, TCP/IP, or other protocols without rewriting core logic.
