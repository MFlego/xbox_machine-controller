# xbox_reader notes #

Compilation command - g++ -std=c++17 -Wall -O2 xbox_reader.cpp -o xbox_reader.exe -lxinput9_1_0


## Notes & testing checklist ## 

- Start order: Start xbox_reader.exe first. It will create a transport (such as a named pipe) and begin writing frames which a client can subscribe to.

- Run Python: Run python xbox_pipe_reader.py. The script will retry until the transport exists and then it will subscribe and print each JSON object it receives.

- No UnicodeDecodeError: Because the C++ program sends textual UTF-8 JSON followed by \n, the Python reader decodes lines; there should be no decoding error.

- No flicker: The UI frame is built in memory and written to the console in one WriteConsoleA call, and the cursor is hidden while running — this should eliminate any flicker or cursor jumping.

- Normalized stick values: The UI prints normalized floats in [-1.000, +1.000], and the JSON uses high-precision floats for programmatic use.

## Current state of the project ##

You are assisting with the development of the Xbox Machine Controller project. The repository contains two working programs, which have been tested and verified to work together on Windows very reliably:

xbox_reader.cpp
- Reads Xbox controller input using decoupled API input modules for different platforms (e.g., SDL2, evdev, HID) which can be swapped in and out depending on the target system.
- Provides an htop-style live console UI with minimal flicker.
- Streams controller state as JSON through a transport mechanism.
- Runs efficiently, reliably, and portable across Windows, macOS, Linux, and even embedded systems.

xbox_pipe_reader.py
- Connects to the transport mechanism and parses the JSON stream.
- Intended as an extensible interface layer for controlling CNC machines, industrial robots, and other fabrication machinery.

# Goals for this phase 2 of development: #
- Refactor the C++ code to separate concerns: input handling, UI rendering, and pipe communication should each be modular (implemented in separate components or modules) and independent (with well-defined interfaces and minimal coupling between them). For example:
**A. Refactoring and Modularity**

*Controller Input Abstraction Specification:*
- Define a base class (e.g., `ControllerInputBackend`) with virtual methods for initialization, polling, and state retrieval.
- Implement each backend (XInput, SDL2, evdev, HID, etc.) as a derived class.
- Use a factory or plugin system to instantiate the appropriate backend at runtime based on platform or configuration.
- Ensure the main program interacts only with the abstraction, not with backend-specific details.
  - UI rendering: Should be separated from input and transport logic, receiving controller state via an interface and handling all display operations independently.
  - Pipe communication: Should be encapsulated in its own module/class, responsible only for transmitting data, and should not depend on UI or input logic.
**A. Refactoring and Modularity**
1. Refactor the C++ code to separate concerns: input handling, UI rendering, and pipe communication should be modular and independent.
2. Introduce an abstraction layer for controller input so that future backends (non-XInput) can be added cleanly.
3. Ensure the C++ program can be easily extended to support different input backends (e.g., SDL2, evdev, HID) in addition to XInput.

**B. Cross-Platform Compatibility**
4. Make the C++ program as self-contained and cross-platform as possible, minimizing Windows-specific code and dependencies, unless they are in the input or transport modules.
5. Make the Python program as self-contained and cross-platform as possible, minimizing Windows-specific code and dependencies, unless they are in the transport module.
6. Ensure the C++ program can be compiled and run on Windows, macOS, Linux, and embedded systems with minimal changes.
7. Ensure the Python program can be run on Windows, macOS, Linux, and embedded systems with minimal changes.

**C. Extensibility**
8. Ensure the Python program can be easily extended to support different transport mechanisms (e.g., serial, BLE, TCP/IP) in addition to named pipes.
9. Begin defining a transport abstraction layer for the Python interface, implemented as a class or module, prioritizing support for named pipes, TCP/IP, and serial protocols, so it can flexibly switch between these or other protocols without rewriting core logic.

**D. Data and Performance**
10. Ensure the JSON output is well-structured, consistent, and easy to parse in Python.
11. Maintain high performance and low latency in both the C++ and Python programs.

**E. Documentation**
12. Write clear documentation and comments to explain the architecture and design decisions, including:
    - Inline code comments for complex logic and module interfaces.
    - Dedicated sections in the README for architectural overview and usage.
    - Additional documentation files in a 'docs' folder for detailed module/component explanations.
