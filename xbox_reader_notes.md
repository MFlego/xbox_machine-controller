# xbox_reader notes #

Compilation command - g++ -std=c++17 -Wall -O2 xbox_reader.cpp -o xbox_reader.exe -lxinput9_1_0


## Notes & testing checklist ## 

- Start order: Start xbox_reader.exe first. It will create the named pipe and begin writing frames (it spawns a background pipe thread that accepts clients).

- Run Python: Run python xbox_pipe_reader.py. The script will retry until the pipe exists and then attach and print each JSON object it receives.

- No UnicodeDecodeError: Because the C++ program now sends textual UTF-8 JSON followed by \n, the Python reader decodes lines; there should be no decoding error.

- No flicker: The UI frame is built in memory and written to the console in one WriteConsoleA call, and the cursor is hidden while running — this should eliminate the flicker and cursor jumping you observed.

- Normalized stick values: The UI prints normalized floats in [-1.000, +1.000], and the JSON uses high-precision floats for programmatic use.