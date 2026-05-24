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
- **Mouse Button Awareness:** Installs a `WH_MOUSE_LL` hook so that mouse clicks properly cancel pending modifier-only combos (enables `Win+MButton` shortcuts in AutoHotkey and similar tools). A `RegisterRawInputDevices` / `WM_INPUT` listener on a message-only window also covers keyboard and mouse combos swallowed by tools that break the hook chain — physical events are received at the HID driver level, bypassing the hook chain entirely. Additionally, when a Win key is held and a mouse button is clicked while a Win-key combo is registered, a `VK_NONAME` keydown+keyup is injected to prevent the Windows shell from opening the Start Menu on Win release (the shell only tracks keyboard events, not mouse clicks, for its Start Menu activation logic).
- **Windows System Shortcut Detection:** A `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` callback detects foreground window changes caused by `Win+1`–`Win+9` taskbar shortcuts, which are processed by Windows at a level that bypasses both LL hooks and raw input entirely. Any foreground transition (including double-press round-trips A→B→A) cancels a pending modifier-only combo.
- **Complex Combos:** Supports multi-key combinations (e.g., `LCTRL+LSHIFT+A`).
- **Singleton Architecture:** Ensures only one instance of the daemon runs at a time (using a global mutex), centralizing all hotkey management to save system resources.
- **Auto-Exit:** The daemon exits automatically when its last pipe client disconnects and no CLI combos remain, avoiding stale background processes.
- **IPC Named Pipe:** Fast and reliable communication (`\\.\pipe\WinKeyHook`) for third-party apps to interact with the daemon.
- **Graceful Fallbacks:** Suppresses default Windows behaviors for registered hotkeys where necessary.
- **Silent Installation:** Ships with an Inno Setup installer for a clean, centralized system-wide deployment. The installer registers a Windows Task Scheduler entry (`/TN "WinKeyHook"`, `/RL HIGHEST`, `/SC ONLOGON`) so the daemon starts elevated automatically at logon — no UAC prompt, no manual step required.

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
v1.1.0
```

### 3. Auto-Update

```cmd
WinKeyHook.exe --update
```

Fetches the latest release from GitHub, compares it with the installed version, downloads the installer if a newer version is available, and launches it (UAC prompt will appear).

**Output:**
```
Checking for updates...
Installed : v1.1.0
Latest    : v1.1.1
Downloading v1.0.4...
Launching installer...
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

### Modifiers

| Name | Meaning |
|---|---|
| `LCTRL` / `RCTRL` / `CTRL` | Left, right, or either Ctrl |
| `LSHIFT` / `RSHIFT` / `SHIFT` | Left, right, or either Shift |
| `LALT` / `RALT` / `ALT` | Left, right, or either Alt |
| `LWIN` / `RWIN` / `WIN` | Left, right, or either Windows key |

### Standard Named Keys

`A`–`Z`, `0`–`9`, `F1`–`F24`

`SPACE`, `TAB`, `ENTER`, `ESC`, `BACKSPACE`, `DELETE`, `INSERT`

`HOME`, `END`, `PGUP`, `PGDN`, `UP`, `DOWN`, `LEFT`, `RIGHT`

`PRINTSCREEN`, `PAUSE`, `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`

`NUM0`–`NUM9` (numpad), `APPS` (Menu key)

### Hexadecimal Virtual-Key Code

Prefix any Windows VK code with `0x`:

```
LWIN+0x41    → LWin + A  (same as LWIN+A)
LCTRL+0xBC   → Ctrl + comma
```

Full VK list: [Microsoft docs — Virtual-Key Codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)

### Layout-Specific Characters (AZERTY, Dvorak, etc.)

Any single printable character not matched above is resolved automatically via `VkKeyScanExW()` against the **active keyboard layout**. You write the character your key physically produces — no need to know the VK code.

This was designed specifically for non-QWERTY layouts where the number row (and other keys) produce different characters:

| Layout | Key above `A` | You write | Resolves to |
|---|---|---|---|
| AZERTY | `&` | `LWIN+&` | VK `0x31` (same physical key as `1` on QWERTY) |
| AZERTY | `é` | `LWIN+é` | VK `0x32` |
| AZERTY | `"` | `LWIN+"` | VK `0x33` |
| AZERTY | `à` | `LWIN+à` | VK `0x30` |
| AZERTY | `(` | `LWIN+(` | VK `0x35` |
| QWERTY | `!` | `LCTRL+!` | VK `0x31` |

The virtual key is resolved **at registration time** — it stays stable even if the layout changes later. Multi-byte UTF-8 characters (`é`, `à`, `ç`, etc.) are fully supported when the spec is sent as UTF-8 text.

**CLI example (AZERTY):**
```cmd
WinKeyHook.exe 0 "LWIN+&" --name "App1" "LWIN+é" --name "App2"
```

**Pipe example (AZERTY, Python):**
```python
win32file.WriteFile(handle, "REGISTER LWIN+& App1\n".encode("utf-8"))
win32file.WriteFile(handle, "REGISTER LWIN+é App2\n".encode("utf-8"))
```

> **Note:** The `+` character cannot be used as a key token because it is the combo delimiter. Use `0x6B` (numpad `+`) or `0xBB` (OEM `+`) instead.

---

## Trigger Timing: Modifier-Only Combos

When a registered combo contains **only modifier keys** (e.g., `LWIN` alone, or `LCTRL+LSHIFT`), it is triggered on **key-up**, not key-down.

This prevents the daemon from firing your shortcut when the user actually intends to press `Win+S`, `Win+Shift+S`, or any other system combo that begins with the same modifier. The sequence works as follows:

1. **Key-down:** The modifier passes through to Windows normally. Windows and AHK see the physical key press. `GetAsyncKeyState` history is cleared at the driver level so only keys pressed after this moment are detected.
2. **Any other key pressed while modifier is held:** The pending trigger is cancelled — Windows and AHK already see the modifier held, so the system shortcut (e.g. `Win+S`) fires normally without any injection needed.
3. **Any key or mouse button pressed while modifier is held:** The pending trigger is cancelled — regardless of whether the event reaches WinKeyHook's hook chain. A `WH_MOUSE_LL` hook cancels immediately for mouse events; a `RegisterRawInputDevices` (`RIDEV_INPUTSINK`) listener on a message-only window cancels for both keyboard and mouse events that tools like AHK swallow before calling `CallNextHookEx`. Physical events are identified by `hDevice != NULL` in the raw input header; injected events (`SendInput`, `keybd_event`) are ignored.
4. **Windows system shortcuts (e.g. `Win+1`–`Win+9`):** These are processed by Windows at a level that may bypass the LL hook chain and raw input entirely. Three detection layers run at key-up:
   - **Raw input buffer** (`GetRawInputBuffer`): reads physical events that AHK swallowed before calling `CallNextHookEx`. Covers `LWin+Z` and similar AHK hotkeys.
   - **Foreground tracking** (`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`): detects any foreground transition while a combo is pending. Covers `Win+1`–`Win+9` when the target app is not already focused, including double-press round-trips (A→B→A).
   - **Driver-level key history** (`GetAsyncKeyState` bit 0): reads key-press records from the HID driver — below Windows Shell, below AHK, below raw input. Covers `Win+1`–`Win+9` when the target app is **already** in the foreground (no foreground change occurs) and the second key was consumed before reaching any hook or raw input.
5. **Key-up with no other key detected by any of the above layers:** The combo triggers (`TRIGGERED`). The physical keyup is suppressed, and a synthetic Ctrl tap + modifier keyup is injected to release the stuck modifier state and prevent the Start Menu.

**Examples:**

| Input | Result |
|---|---|
| Press and release `LWIN` alone | `TRIGGERED` fires on release |
| Press `LWIN`, then `S`, release both | `Win+S` (Search) fires normally; no trigger |
| Press `LWIN`, click mouse button, release (no Win combo registered) | AutoHotkey `Win+MButton` fires normally; no trigger; Start Menu may open |
| Press `LWIN`, click mouse button, release (Win combo registered) | AutoHotkey `Win+MButton` fires normally; no trigger; **Start Menu suppressed** via `VK_NONAME` injection |
| Press `LWIN`, press `Shift`, press `S` | `Win+Shift+S` (Snip tool) fires normally; no trigger |
| Press `LWIN`, then `Z` (AHK hotkey) | Raw input buffer detects `Z`; trigger cancelled |
| Press `LWIN`, then `1` (taskbar, different app focused) | Foreground window changes; trigger cancelled |
| Press `LWIN`, then `1` twice (round-trip focus) | Foreground event detected; trigger cancelled |
| Press `LWIN`, then `1` (taskbar, target already focused) | Driver-level key history detects `1`; trigger cancelled |

For combos that include a non-modifier key (e.g., `LWIN+SPACE`), the trigger fires immediately on key-down as usual.

---

## Elevated Startup for Client Applications

WinKeyHook installs with a `requireAdministrator` manifest — it must run elevated to reliably hook system-level key events. The installer creates a Task Scheduler entry that handles this automatically at logon.

If your client application needs to start the daemon on demand (e.g. InputBar), the recommended sequence avoids showing a UAC prompt to the user:

1. **Try connecting** to `\\.\pipe\WinKeyHook` first — the daemon may already be running.
2. **Trigger the scheduled task** — `schtasks /Run /TN WinKeyHook` starts the existing elevated task silently (no UAC).
3. **Fall back to direct launch** — works if the exe has no elevation manifest (dev/test builds).
4. **Last resort: ShellExecute runas** — triggers a UAC prompt once.

```python
import subprocess, ctypes

def start_via_task_scheduler() -> bool:
    r = subprocess.run(
        ["schtasks.exe", "/Run", "/TN", "WinKeyHook"],
        creationflags=subprocess.CREATE_NO_WINDOW,
        capture_output=True, timeout=5,
    )
    return r.returncode == 0

def start_elevated_fallback(exe_path: str) -> None:
    ctypes.windll.shell32.ShellExecuteW(None, "runas", exe_path, "0", None, 0)
```

The `schtasks /Run` approach requires only that the installer was previously run — no administrator privileges needed in the calling process.

---

## Building from Source

1. Open a terminal in the `build/` directory.
2. Run `build_app.bat`.
3. Enter the target version (e.g., `1.0.0`). The script will:
   - Inject the version into `WinKeyHook.cpp` and `setup_script.iss`.
   - Compile with `cl.exe` (MSVC x64).
   - Leave `WinKeyHook.exe` in `src/` for immediate local use.
   - Generate a silent installer (`WinKeyHook_vX.X.X_Setup.exe`) via Inno Setup in `releases/`.

**Manual compile:**
```cmd
cl.exe /nologo /EHsc WinKeyHook.cpp /link user32.lib advapi32.lib winhttp.lib urlmon.lib shell32.lib /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
```
