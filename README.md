# WinKeyHook Daemon

![Platform: Windows](https://img.shields.io/badge/Platform-Windows-blue.svg)
![Language: C++](https://img.shields.io/badge/Language-C++-f34b7d.svg)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)

**WinKeyHook** is a lightweight, low-level global keyboard hooking daemon for Windows. It allows multiple applications or scripts to register and listen for complex global hotkeys (combinations of keys) simultaneously without conflicting with each other. 

It provides two distinct modes of operation:
1. **CLI Mode (Stdout):** Run the executable with arguments to listen for specific hotkeys and print events to standard output.
2. **Daemon Mode (Named Pipe):** Run in the background as a server where multiple client applications can connect, register hotkeys dynamically, and receive trigger events via an IPC Named Pipe.

---

## 🚀 Features

- **Global Hotkey Interception:** Captures key presses system-wide using `SetWindowsHookEx(WH_KEYBOARD_LL)`.
- **Complex Combos:** Supports multi-key combinations (e.g., `LCTRL+LSHIFT+A`).
- **Singleton Architecture:** Ensures only one instance of the daemon runs at a time (using a global mutex), centralizing all hotkey management to save system resources.
- **IPC Named Pipe:** Fast and reliable communication (`\\.\pipe\WinKeyHook`) for third-party apps to interact with the daemon.
- **Graceful Fallbacks:** Suppresses default Windows behaviors for registered hotkeys where necessary.
- **Silent Installation:** Ships with an Inno Setup installer for a clean, centralized system-wide deployment.

---

## 🛠️ Usage

### 1. CLI Mode (Standard Output)

You can launch `WinKeyHook.exe` directly from a terminal or a script, passing the hotkey combinations you want to listen to as arguments.

**Example:**
```cmd
WinKeyHook.exe "LCTRL+SPACE" "ALT+F1"
```

**Output:**
```
HOOK_STARTED
TRIGGERED:0
TRIGGERED:1
```
*(The index corresponds to the position of the argument passed, starting at 0).*

### 2. Version Check

You can query the current version of the daemon. This is highly useful for auto-updaters to verify the installed version before downloading an update.

**Command:**
```cmd
WinKeyHook.exe --version
```

**Output:**
```
v1.0.0
```

---

## 🔌 Daemon Mode & Protocol (Named Pipe)

If `WinKeyHook.exe` is launched without arguments (or is already running), it acts as a Named Pipe server. Other programs (Python, C#, Node.js, etc.) can connect to it to register hotkeys dynamically.

**Pipe Name:** `\\.\pipe\WinKeyHook`

### Message Format
All messages exchanged over the pipe are plain text strings, terminated by a newline character (`\n`).

### Client $\rightarrow$ Server Commands

| Command | Description | Example |
|---|---|---|
| `REGISTER <spec> [<name>]` | Registers a new hotkey combination. An optional name/identifier can be provided. | `REGISTER LCTRL+LSHIFT+A MyMacro` |
| `UNREGISTER <name_or_spec>` | Unregisters a previously registered hotkey using its specification or name. | `UNREGISTER MyMacro` |
| `PING` | Sends a keep-alive ping to the server. | `PING` |

### Server $\rightarrow$ Client Responses

| Response | Description |
|---|---|
| `HELLO WinKeyHook` | Sent immediately upon a successful client connection. |
| `OK <name_or_spec>` | Confirms a successful `REGISTER` or `UNREGISTER` command. |
| `ERR <reason>` | Indicates a command failed (e.g., malformed combo, invalid command). |
| `TRIGGERED <name_or_spec> <spec>` | Fired asynchronously when the registered hotkey is physically pressed by the user. |
| `PONG` | Response to a `PING` command. |

### Named Pipe Example (Python)

```python
import win32file
import win32pipe

# Connect to the daemon
handle = win32file.CreateFile(
    r'\\.\pipe\WinKeyHook',
    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
    0, None, win32file.OPEN_EXISTING, 0, None
)

# Register a hotkey
win32file.WriteFile(handle, b"REGISTER LWIN+SPACE SearchBar\n")

# Listen for triggers
while True:
    result, data = win32file.ReadFile(handle, 4096)
    message = data.decode().strip()
    if message.startswith("TRIGGERED"):
        print("Hotkey pressed!", message)
```

---

## ⌨️ Key Naming Convention

When defining combos (the `<spec>`), keys must be separated by a `+`.
The parser is case-insensitive.

**Modifiers:** 
`LCTRL`, `RCTRL`, `CTRL`, `LSHIFT`, `RSHIFT`, `SHIFT`, `LALT`, `RALT`, `ALT`, `LWIN`, `RWIN`, `WIN`

**Standard Keys:** 
`A`-`Z`, `0`-`9`, `F1`-`F24`, `SPACE`, `TAB`, `ENTER`, `ESC`, `BACKSPACE`, `DELETE`, `INSERT`, `HOME`, `END`, `PGUP`, `PGDN`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `PRINTSCREEN`, `PAUSE`, `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`, `NUM0`-`NUM9`

**Hexadecimal (Virtual-Key Codes):**
You can also use raw virtual key codes directly by prefixing them with `0x` (e.g., `0x41` for the 'A' key).

---

## 🏗️ Building from Source

This project includes an automated build and release system compatible with Visual Studio MSVC and Inno Setup.

1. Open a terminal in the `build/` directory.
2. Run the `build_app.bat` script.
3. Enter the target version (e.g., `1.0.0`). The script will:
   - Inject the version directly into the C++ source.
   - Compile the binary using `cl.exe`.
   - Generate a silent installer (`WinKeyHook_setup.exe`) using Inno Setup.

To publish directly to GitHub:
Run `python Publish_Release.py` in the `build/` directory.

---

## 📄 License

This project is open-source. Do whatever you want with it!