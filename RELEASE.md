## v1.1.0

### Bug fix - `GetRawInputBuffer` passes wrong buffer size (false fires after AHK hotkeys)

Modifier-only combos (e.g. `LWIN` alone) no longer trigger incorrectly after using an AHK hotkey like `LWin+Z`.

**Root cause:** `DrainRawInput()` calls `GetRawInputBuffer(NULL, &sz, ...)` first to query the required buffer size (stores the result in `sz`), then allocates a larger buffer and calls `GetRawInputBuffer` again — but incorrectly passed `&sz` (the original query result = 40 bytes) as the capacity argument instead of the actual allocated buffer size. The API returned `(UINT)-1` (failure), the error was silently discarded, and the drain reported "nothing seen" — leaving `had_other_key = false` and causing the combo to fire.

**Fix:** A separate `bufsz` variable holds the actual allocated buffer capacity and is passed to the second call. On read failure, the error is now logged with `GetLastError()` and the pending combo is cancelled conservatively (data is present but unreadable → assume a second key was pressed).

### Bug fix - modifier-only combo fires after `Win+1`–`Win+9` when target app already focused

Modifier-only combos no longer trigger when `Win+1`–`Win+9` is pressed and the target taskbar app is **already** in the foreground.

**Root cause:** The existing foreground-change detection (`SetWinEventHook`) correctly handles `Win+1` when focus moves to a different app. But when the target app is already in the foreground, no foreground transition occurs — Windows Shell still consumes the `1` key before it reaches the LL hook chain or raw input, so `DrainRawInput` sees nothing and the foreground check passes. The combo fired incorrectly.

**Fix:** `GetAsyncKeyState` bit 0 is used as a final fallback. This bit is set at the HID driver level — below Windows Shell, below AHK, below raw input — so `1` (or any second key) is recorded there regardless of which layer consumed it. At modifier keydown, `ClearInputHistory()` zeroes bit 0 for all tracked keys so only keys pressed after the pending state is set are detected. At modifier keyup, `AnyNonModifierKeyPressedSince()` checks all tracked keys; if any bit 0 is set, the pending combo is cancelled.

This also reinforces detection for AHK hotkeys (`LWin+Z`) as a belt-and-suspenders layer on top of the `GetRawInputBuffer` fix above.

### Bug fix - modifier-only combo fires on Win+number (taskbar shortcuts)

Modifier-only combos (e.g. `LWIN` alone) no longer trigger when `Win+1`–`Win+9` switches the foreground window via the Windows taskbar.

**Root cause (single press):** Windows processes `Win+number` taskbar shortcuts at a level that is completely invisible to both the LL keyboard hook chain and raw input (`WM_INPUT`). WinKeyHook saw no second key between `LWin` keydown and `LWin` keyup, so `had_other_key` stayed false and the combo fired incorrectly. The only observable side effect of `Win+number` is that the foreground window changes.

**Root cause (double press):** Pressing `Win+1` twice (A→B, then B→A) would bring the foreground back to the original window before `LWin` keyup — a simple HWND equality check at keyup would see no change and still fire incorrectly.

**Fix:** `GetForegroundWindow()` is snapshotted when `pending_trigger` is set (at `LWin` keydown). A `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` callback sets a `g_pending_fg_changed` flag on any foreground transition while a combo is pending. At `LWin` keyup, if `g_pending_fg_changed` is true OR the current foreground differs from the snapshot, the pending combo is cancelled. This correctly handles both single and double `Win+number` presses without affecting a solo `LWin` press.

### Feature - `--update` flag (auto-update from GitHub)

```cmd
WinKeyHook.exe --update
```

Fetches the latest release from `github.com/BlessEphraem/WinKeyHook`, compares it with the installed version, downloads the setup installer if a newer version is available, and launches it (UAC prompt). Exits immediately if already up to date.

### Feature - universal keyboard layout support (AZERTY, Dvorak, etc.)

Key specs in combos now accept any printable character from the active keyboard layout, not just hardcoded ASCII names.

**Before:** `LWIN+&` failed silently on AZERTY — `&` was not in the hardcoded key list, so the combo was ignored.

**After:** Any single printable character that can be typed on the current layout is resolved to its virtual key code via `VkKeyScanExW()`. Write the character your key physically produces — no VK code lookup needed.

**AZERTY examples that now work:**

```cmd
WinKeyHook.exe 0 "LWIN+&" --name "App1"
WinKeyHook.exe 0 "LWIN+é" --name "App2"
WinKeyHook.exe 0 "LWIN+(" --name "App5"
WinKeyHook.exe 0 "LCTRL+LSHIFT+à" --name "MyMacro"
```

Via named pipe (UTF-8 encoded):
```
REGISTER LWIN+& App1
REGISTER LWIN+é App2
```

`LWIN+&` on AZERTY resolves to VK=`0x31` (same physical key as `1` on QWERTY). The virtual key is resolved at registration time — stable even if the layout changes later. Multi-byte UTF-8 characters (`é`, `à`, `ç`, etc.) are fully supported.

All existing combos (`LCTRL+SPACE`, `LWIN+F1`, `A`–`Z`, `0`–`9`, `0xHH`) are unaffected — the new code is a fallback reached only when no hardcoded name matches.

### Feature - daemon auto-exits when last client disconnects

The daemon now exits automatically when its last pipe client disconnects and no CLI combos are registered, instead of staying alive indefinitely.

**Why:** When launched by a host application (e.g. InputBar), the daemon previously kept running after the host exited, requiring a manual kill or reboot to clean up.

**How it works:** On each client disconnect, if no other pipe clients remain alive and no CLI combos are active, `PostThreadMessage(WM_QUIT)` is sent to the main thread. The daemon shuts down cleanly.
