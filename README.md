# WinKeyHook Daemon

![Platform: Windows](https://img.shields.io/badge/Platform-Windows-blue.svg)
![Language: C++](https://img.shields.io/badge/Language-C++-f34b7d.svg)
![License: GNU](https://img.shields.io/badge/License-GNU-blue.svg)

**WinKeyHook** is a lightweight, low-level global keyboard hooking daemon for Windows. It allows multiple applications or scripts to register and listen for complex global hotkeys (combinations of keys) simultaneously without conflicting with each other.

It provides two distinct modes of operation:
1. **CLI Mode (Stdout):** Run the executable with arguments to listen for specific hotkeys and print events to standard output.
2. **Daemon Mode (Named Pipe):** Run in the background as a server where multiple client applications can connect, register hotkeys dynamically, and receive trigger events via an IPC Named Pipe.

---

## Features

- **Global Hotkey Interception:** Captures key presses system-wide using `SetWindowsHookEx(WH_KEYBOARD_LL)`.
- **Mouse Button Awareness:** Installs a `WH_MOUSE_LL` hook so that mouse clicks properly cancel pending modifier-only combos (enables `Win+MButton` shortcuts in AutoHotkey and similar tools). A `GetAsyncKeyState` fallback also covers keyboard and mouse combos swallowed by tools that break the hook chain.
- **Complex Combos:** Supports multi-key combinations (e.g., `LCTRL+LSHIFT+A`).
- **Singleton Architecture:** Ensures only one instance of the daemon runs at a time (using a global mutex), centralizing all hotkey management to save system resources.
- **IPC Named Pipe:** Fast and reliable communication (`\\.\pipe\WinKeyHook`) for third-party apps to interact with the daemon.
- **Graceful Fallbacks:** Suppresses default Windows behaviors for registered hotkeys where necessary.
- **Silent Installation:** Ships with an Inno Setup installer for a clean, centralized system-wide deployment.

---

## Usage

### 1. CLI Mode (Standard Output)

You can launch `WinKeyHook.exe` directly from a terminal or a script, passing the hotkey combinations you want to listen to as arguments.

**Syntax:**
```
WinKeyHook.exe <parent_pid> [<combo> [--name <name>] ...]
```

- `parent_pid` — PID to watch; the daemon exits when that process dies. Use `0` to run forever.
- `combo` — One or more key combos (see [Key Naming Convention](#key-naming-convention)).
- `--name` — Optional label for the preceding combo.

**Example:**
```cmd
WinKeyHook.exe 0 "LCTRL+SPACE" "ALT+F1" --name "Snap"
```

**Output:**
```
HOOK_STARTED
TRIGGERED:0
TRIGGERED:1 Snap
```
*(The index corresponds to the position of the argument passed, starting at 0.)*

### 2. Version Check

```cmd
WinKeyHook.exe --version
```

**Output:**
```
v1.0.0
```

---

## Daemon Mode & Protocol (Named Pipe)

`WinKeyHook.exe` always starts a Named Pipe server. Other programs (Python, C#, Node.js, etc.) can connect to it to register hotkeys dynamically.

**Pipe Name:** `\\.\pipe\WinKeyHook`

### Message Format
All messages exchanged over the pipe are plain text strings, terminated by a newline character (`\n`).

### Client → Server Commands

| Command | Description | Example |
|---|---|---|
| `REGISTER <spec> [<name>]` | Registers a new hotkey. Optional name/identifier can be provided. | `REGISTER LCTRL+LSHIFT+A MyMacro` |
| `UNREGISTER <name_or_spec>` | Unregisters a previously registered hotkey by name or spec. | `UNREGISTER MyMacro` |
| `PING` | Sends a keep-alive ping to the server. | `PING` |

### Server → Client Responses

| Response | Description |
|---|---|
| `HELLO WinKeyHook` | Sent immediately upon a successful client connection. |
| `OK <name_or_spec>` | Confirms a successful `REGISTER` or `UNREGISTER` command. |
| `ERR <reason>` | Indicates a command failed (e.g., malformed combo, invalid command). |
| `TRIGGERED <name_or_spec> <spec>` | Fired asynchronously when the registered hotkey is pressed. |
| `PONG` | Response to a `PING` command. |

When a client disconnects, all combos it registered are removed automatically.

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

## Key Naming Convention

When defining combos (the `<spec>`), keys must be separated by `+`. The parser is **case-insensitive**.

**Modifiers:**
`LCTRL`, `RCTRL`, `CTRL`, `LSHIFT`, `RSHIFT`, `SHIFT`, `LALT`, `RALT`, `ALT`, `LWIN`, `RWIN`, `WIN`

**Standard Keys:**
`A`–`Z`, `0`–`9`, `F1`–`F24`, `SPACE`, `TAB`, `ENTER`, `ESC`, `BACKSPACE`, `DELETE`, `INSERT`, `HOME`, `END`, `PGUP`, `PGDN`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `PRINTSCREEN`, `PAUSE`, `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`, `NUM0`–`NUM9`

**Hexadecimal (Virtual-Key Codes):**
Use raw virtual key codes prefixed with `0x` (e.g., `0x41` for `A`).

---

## Trigger Timing: Modifier-Only Combos

When a registered combo contains **only modifier keys** (e.g., `LWIN` alone, or `LCTRL+LSHIFT`), it is triggered on **key-up**, not key-down.

This prevents the daemon from firing your shortcut when the user actually intends to press `Win+S`, `Win+Shift+S`, or any other system combo that begins with the same modifier. The sequence works as follows:

1. **Key-down:** The modifier passes through to Windows normally. Windows and AHK see the physical key press.
2. **Any other key pressed while modifier is held:** The pending trigger is cancelled — Windows and AHK already see the modifier held, so the system shortcut (e.g. `Win+S`) fires normally without any injection needed.
3. **Mouse button clicked while modifier is held:** Same cancellation — the mouse shortcut (e.g. an AutoHotkey `Win+MButton` binding) fires normally. A `WH_MOUSE_LL` hook handles this; a `GetAsyncKeyState` fallback covers cases where another tool breaks the hook chain.
4. **Key-up with no other key pressed:** The combo triggers (`TRIGGERED`). The physical keyup is suppressed, and a synthetic Ctrl tap + modifier keyup is injected to release the stuck modifier state and prevent the Start Menu.

**Examples:**

| Input | Result |
|---|---|
| Press and release `LWIN` alone | `TRIGGERED` fires on release |
| Press `LWIN`, then `S`, release both | `Win+S` (Search) fires normally; no trigger |
| Press `LWIN`, click mouse button, release | AutoHotkey `Win+MButton` fires normally; no trigger |
| Press `LWIN`, press `Shift`, press `S` | `Win+Shift+S` (Snip tool) fires normally; no trigger |

For combos that include a non-modifier key (e.g., `LWIN+SPACE`), the trigger fires immediately on key-down as usual.

---

## Building from Source

This project includes an automated build and release system compatible with Visual Studio MSVC and Inno Setup.

1. Open a terminal in the `build/` directory.
2. Run `build_app.bat`.
3. Enter the target version (e.g., `1.0.0`). The script will:
   - Inject the version into the C++ source.
   - Compile with `cl.exe` (MSVC x64).
   - Generate a silent installer (`WinKeyHook_setup.exe`) using Inno Setup.

**Manual compile:**
```cmd
cl.exe /EHsc WinKeyHook.cpp /link user32.lib advapi32.lib
```

---

## License

This project is open-source. Do whatever you want with it!
