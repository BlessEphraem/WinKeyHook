## What's new

### Bug fix - modifier-only combo fires through AHK hook chain

Modifier-only combos (e.g. `LWIN` alone) no longer trigger when the key is used as part of an AutoHotkey shortcut (e.g. `LWin+Z`, `LWin+MButton`).

**Root cause:** AutoHotkey and similar tools sometimes handle a hotkey without forwarding the event further down the Windows hook chain (`CallNextHookEx`). WinKeyHook never saw the second key, so its pending-trigger flag stayed set - causing the modifier-only combo to fire on key-up even though another key had been pressed.

**Fix:** Added a `GetAsyncKeyState` fallback. When a modifier-only combo's pending window opens (key-down), history bits are cleared for all tracked non-modifier keys (A–Z, 0–9, F1–F24, numpad, mouse buttons, navigation keys). On key-up, if any bit is set, the combo is cancelled - regardless of whether our hook saw the intermediate event.

---

## Installation

Download `WinKeyHook_vX.X.X_Setup.exe` and run it. The installer places `WinKeyHook.exe` in `%ProgramFiles%\WinKeyHook\` and registers it in PATH.

No restart required. Launch via terminal or integrate into your scripts.