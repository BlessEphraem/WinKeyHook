#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <atomic>
#include <tuple>
#include <cstdarg>
#include <conio.h>
#include <winhttp.h>
#include <shellapi.h>
#include <urlmon.h>

// Protocol (pipe, text, \n-terminated):
//   CLIENT->SERVER:  REGISTER <spec> [<name>]
//                   UNREGISTER <name_or_spec>
//                   PING
//   SERVER->CLIENT:  HELLO WinKeyHook
//                   OK <name_or_spec>
//                   ERR <reason>
//                   TRIGGERED <name_or_spec> <spec>
//                   PONG
//
// Stdout (CLI combos):  HOOK_STARTED | ALREADY_RUNNING
//                       TRIGGERED:<idx> [<name>]

static const char*    WINKEYHOOK_VERSION = "v1.1.0";
static const wchar_t* PIPE_NAME          = L"\\\\.\\pipe\\WinKeyHook";
static const wchar_t* MUTEX_NAME         = L"Global\\WinKeyHookSingleton";
static const DWORD    PIPE_BUF           = 4096;

static const DWORD MODIFIER_VKS[] = {
    VK_LSHIFT, VK_RSHIFT,
    VK_LCONTROL, VK_RCONTROL,
    VK_LMENU, VK_RMENU,
    VK_LWIN, VK_RWIN,
};

// ---------- Debug log ----------------------------------------------------------
static std::ofstream g_log;

static void LogOpen() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    g_log.open(std::string(tmp) + "keyhook_debug.log", std::ios::out | std::ios::trunc);
}
static void Log(const char* msg) {
    if (!g_log.is_open()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char buf[32];
    sprintf_s(buf, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    g_log << buf << msg << "\n";
    g_log.flush();
}
static void Logf(const char* fmt, ...) {
    if (!g_log.is_open()) return;
    char msg[512];
    va_list a; va_start(a, fmt); vsprintf_s(msg, fmt, a); va_end(a);
    Log(msg);
}

// ---------- Combo structures ---------------------------------------------------
struct ComboSlot {
    std::vector<DWORD> vks;

    bool satisfiedBy(const std::set<DWORD>& pressed) const {
        for (DWORD vk : vks) if (pressed.count(vk)) return true;
        return false;
    }
    bool contains(DWORD vk) const {
        return std::find(vks.begin(), vks.end(), vk) != vks.end();
    }
};

struct Combo {
    std::string            spec;
    std::string            name;
    std::vector<ComboSlot> slots;
    bool  active           = false;
    bool  pending_trigger  = false;
    bool  had_other_key    = false;
    DWORD owner_client_id  = 0;   // 0 = stdout (CLI combo)
    int   cli_index        = -1;  // TRIGGERED:<cli_index> on stdout

    bool satisfied(const std::set<DWORD>& p) const {
        if (slots.empty()) return false;
        for (const auto& s : slots) if (!s.satisfiedBy(p)) return false;
        return true;
    }
    bool hasVk(DWORD vk) const {
        for (const auto& s : slots) if (s.contains(vk)) return true;
        return false;
    }
    // True if no slot contains a modifier key (solo key like F1, A, Space)
    bool isSolo() const {
        for (const auto& s : slots)
            for (DWORD vk : s.vks)
                for (DWORD mod : MODIFIER_VKS)
                    if (vk == mod) return false;
        return true;
    }
    // True if every key in every slot is a modifier (e.g. LWIN alone)
    bool isModifierOnly() const {
        if (slots.empty()) return false;
        for (const auto& s : slots)
            for (DWORD vk : s.vks) {
                bool is_mod = false;
                for (DWORD mod : MODIFIER_VKS)
                    if (vk == mod) { is_mod = true; break; }
                if (!is_mod) return false;
            }
        return true;
    }
    const std::string& identifier() const { return name.empty() ? spec : name; }
};

// ---------- Client -------------------------------------------------------------
struct Client {
    DWORD  id;
    HANDLE pipe;
    bool   alive = true;
};

// ---------- Global state -------------------------------------------------------
HHOOK          g_hook           = NULL;
HHOOK          g_mouse_hook     = NULL;
HWINEVENTHOOK  g_win_event_hook = NULL;
HWINEVENTHOOK  g_fg_event_hook  = NULL;   // EVENT_SYSTEM_FOREGROUND hook
HWND           g_msg_wnd        = NULL;   // message-only window for raw input
DWORD          g_main_thread_id = 0;

CRITICAL_SECTION     g_cs;         // guards g_combos + g_clients
std::vector<Combo>   g_combos;
std::vector<Client>  g_clients;

static HWND g_pending_foreground  = NULL;   // foreground at LWin keydown; NULL when no pending combo
static bool g_pending_fg_changed  = false;  // true if foreground changed at least once while pending

std::set<DWORD>   g_pressed;    // hook-thread only -> no lock needed
std::set<DWORD>   g_suppressed;

std::atomic<DWORD> g_next_client_id{1};

// ---------- Key name parser ----------------------------------------------------
static std::string ToUpper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

static std::vector<DWORD> ParseKeyName(const std::string& raw) {
    std::string n = ToUpper(raw);

    if (n == "LCTRL"  || n == "LCONTROL")   return {VK_LCONTROL};
    if (n == "RCTRL"  || n == "RCONTROL")   return {VK_RCONTROL};
    if (n == "CTRL"   || n == "CONTROL")    return {VK_LCONTROL, VK_RCONTROL};
    if (n == "LSHIFT")                      return {VK_LSHIFT};
    if (n == "RSHIFT")                      return {VK_RSHIFT};
    if (n == "SHIFT")                       return {VK_LSHIFT, VK_RSHIFT};
    if (n == "LALT"   || n == "LMENU")      return {VK_LMENU};
    if (n == "RALT"   || n == "RMENU")      return {VK_RMENU};
    if (n == "ALT"    || n == "MENU")       return {VK_LMENU, VK_RMENU};
    if (n == "LWIN")                        return {VK_LWIN};
    if (n == "RWIN")                        return {VK_RWIN};
    if (n == "WIN")                         return {VK_LWIN, VK_RWIN};

    if (n.size() >= 2 && n[0] == 'F') {
        try {
            int num = std::stoi(n.substr(1));
            if (num >= 1 && num <= 24) return {(DWORD)(VK_F1 + num - 1)};
        } catch (...) {}
    }

    if (n == "SPACE")                          return {VK_SPACE};
    if (n == "TAB")                            return {VK_TAB};
    if (n == "ENTER"    || n == "RETURN")      return {VK_RETURN};
    if (n == "ESC"      || n == "ESCAPE")      return {VK_ESCAPE};
    if (n == "BACK"     || n == "BACKSPACE")   return {VK_BACK};
    if (n == "DEL"      || n == "DELETE")      return {VK_DELETE};
    if (n == "INS"      || n == "INSERT")      return {VK_INSERT};
    if (n == "HOME")                           return {VK_HOME};
    if (n == "END")                            return {VK_END};
    if (n == "PGUP"     || n == "PAGEUP")      return {VK_PRIOR};
    if (n == "PGDN"     || n == "PAGEDOWN")    return {VK_NEXT};
    if (n == "UP")                             return {VK_UP};
    if (n == "DOWN")                           return {VK_DOWN};
    if (n == "LEFT")                           return {VK_LEFT};
    if (n == "RIGHT")                          return {VK_RIGHT};
    if (n == "APPS")                           return {VK_APPS};
    if (n == "PRINT"    || n == "PRINTSCREEN") return {VK_SNAPSHOT};
    if (n == "PAUSE")                          return {VK_PAUSE};
    if (n == "SCROLL"   || n == "SCROLLLOCK")  return {VK_SCROLL};
    if (n == "CAPS"     || n == "CAPSLOCK")    return {VK_CAPITAL};
    if (n == "NUMLOCK")                        return {VK_NUMLOCK};

    if (n.size() == 4 && n.substr(0, 3) == "NUM" && n[3] >= '0' && n[3] <= '9')
        return {(DWORD)(VK_NUMPAD0 + (n[3] - '0'))};

    if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return {(DWORD)n[0]};
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return {(DWORD)n[0]};

    if (n.size() >= 3 && n[0] == '0' && n[1] == 'X') {
        try { return {(DWORD)std::stoul(n, nullptr, 16)}; } catch (...) {}
    }

    // Unicode fallback: resolve via active keyboard layout (AZERTY, Dvorak, etc.)
    // Only reached when no hardcoded name matched — zero impact on existing combos.
    {
        wchar_t wch[4] = {};
        int count = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), wch, 3);
        if (count == 1 && wch[0] != 0) {
            HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
            SHORT scan = VkKeyScanExW(wch[0], hkl);
            if (scan != -1 && LOBYTE(scan) != 0) {
                Logf("ParseKeyName: '%s' -> VK=0x%02X via VkKeyScanEx", raw.c_str(), LOBYTE(scan));
                return {(DWORD)(BYTE)LOBYTE(scan)};
            }
        }
    }

    return {};
}

static Combo ParseCombo(const std::string& spec) {
    Combo combo;
    combo.spec = spec;
    std::string token;
    std::istringstream ss(spec);
    while (std::getline(ss, token, '+')) {
        if (token.empty()) continue;
        auto vks = ParseKeyName(token);
        if (vks.empty()) {
            Logf("WARNING: unknown key '%s' in combo '%s'", token.c_str(), spec.c_str());
            continue;
        }
        ComboSlot slot; slot.vks = vks;
        combo.slots.push_back(slot);
        if (combo.slots.size() >= 4) break;
    }
    return combo;
}

// ---------- Pipe helpers -------------------------------------------------------
static void PipeSend(HANDLE pipe, const char* msg) {
    DWORD w;
    WriteFile(pipe, msg, (DWORD)strlen(msg), &w, NULL);
}

// ---------- Deferred write helpers ---------------------------------------------
// Hook proc collects these inside g_cs, then flushes after releasing the lock.
// Keeps WriteFile out of the critical section and prevents hook-timeout risk.
struct PendingWrite {
    HANDLE pipe;
    char   msg[512];
};

static void FlushPendingWrites(std::vector<PendingWrite>& pw) {
    for (auto& w : pw) {
        DWORD written;
        WriteFile(w.pipe, w.msg, (DWORD)strlen(w.msg), &written, NULL);
    }
}

// Enqueue a TRIGGERED notification. Must be called with g_cs held.
// stdout combos are written immediately (no blocking risk); pipe combos are queued.
static void FireCombo(const Combo& combo, std::vector<PendingWrite>& pw) {
    if (combo.owner_client_id == 0) {
        char msg[256];
        if (combo.name.empty())
            sprintf_s(msg, "TRIGGERED:%d\n", combo.cli_index);
        else
            sprintf_s(msg, "TRIGGERED:%d %s\n", combo.cli_index, combo.name.c_str());
        fwrite(msg, 1, strlen(msg), stdout);
        fflush(stdout);
        Logf("TRIGGERED stdout combo[%d] '%s'", combo.cli_index, combo.spec.c_str());
    } else {
        PendingWrite entry = {};
        sprintf_s(entry.msg, "TRIGGERED %s %s\n",
                  combo.identifier().c_str(), combo.spec.c_str());
        for (const auto& c : g_clients) {
            if (c.id == combo.owner_client_id && c.alive) {
                entry.pipe = c.pipe;
                break;
            }
        }
        if (entry.pipe) {
            pw.push_back(entry);
            Logf("TRIGGERED pipe client=%u '%s'",
                 combo.owner_client_id, combo.identifier().c_str());
        }
    }
}

// ---------- Pending-combo cancellation helper ----------------------------------
// Cancels all pending modifier-only combos. Called under g_cs.
static void CancelPendingCombos(const char* reason) {
    bool any = false;
    for (auto& c : g_combos) if (c.pending_trigger) { any = true; break; }
    if (any) {
        for (auto& c : g_combos)
            if (c.pending_trigger) {
                c.had_other_key   = true;
                c.pending_trigger = false;
                c.active          = false;
            }
        Logf("CANCEL pending combos: %s", reason);
        // Reset FG tracking so stale events don't affect the next combo.
        // Without this, if the cancel came from MsgWndProc (hasPending=false at keyup),
        // the keyup FG-reset block is skipped and g_pending_foreground stays set,
        // causing spurious FG_CHANGED cancels on subsequent solo presses.
        g_pending_foreground = NULL;
        g_pending_fg_changed = false;
    }
}

// ---------- Driver-level key history (GetAsyncKeyState bit 0) -----------------
// GetAsyncKeyState() bit 0 is set if the key was pressed since the last call
// for that key.  This is recorded at the HID driver level — BELOW the LL hook
// chain, BELOW AHK, and BELOW Windows Shell (which processes Win+1–9 before
// any hook fires).  Reading the bit clears it for subsequent calls.
//
// ClearInputHistory: called when pending_trigger is set (modifier keydown) so
// that AnyNonModifierKeyPressedSince() only detects keys pressed AFTER that
// moment.
//
// AnyNonModifierKeyPressedSince: final fallback at modifier keyup, after
// DrainRawInput and the foreground check.  Catches Win+1 when the target app
// is already in the foreground (no foreground change) and the '1' key was
// consumed by Windows Shell before reaching raw input or the LL hook chain.

static const DWORD NON_MODIFIER_VKS[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '0','1','2','3','4','5','6','7','8','9',
    VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,
    VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    VK_F13,VK_F14,VK_F15,VK_F16,VK_F17,VK_F18,
    VK_F19,VK_F20,VK_F21,VK_F22,VK_F23,VK_F24,
    VK_NUMPAD0,VK_NUMPAD1,VK_NUMPAD2,VK_NUMPAD3,VK_NUMPAD4,
    VK_NUMPAD5,VK_NUMPAD6,VK_NUMPAD7,VK_NUMPAD8,VK_NUMPAD9,
    VK_MULTIPLY,VK_ADD,VK_SUBTRACT,VK_DECIMAL,VK_DIVIDE,
    VK_SPACE,VK_TAB,VK_RETURN,VK_ESCAPE,VK_BACK,
    VK_DELETE,VK_INSERT,VK_HOME,VK_END,VK_PRIOR,VK_NEXT,
    VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,
    VK_OEM_1,VK_OEM_PLUS,VK_OEM_COMMA,VK_OEM_MINUS,
    VK_OEM_PERIOD,VK_OEM_2,VK_OEM_3,VK_OEM_4,VK_OEM_5,
    VK_OEM_6,VK_OEM_7,VK_OEM_8,
    VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2,
};

static void ClearInputHistory() {
    for (DWORD vk : NON_MODIFIER_VKS)
        GetAsyncKeyState((int)vk);  // read-and-clear bit 0
}

static bool AnyNonModifierKeyPressedSince() {
    for (DWORD vk : NON_MODIFIER_VKS)
        if (GetAsyncKeyState((int)vk) & 0x0001) return true;
    return false;
}

// ---------- Raw input buffer drain --------------------------------------------
// Called synchronously from the LWin keyup LL hook callback, BEFORE deciding
// whether to fire a pending modifier-only combo.
//
// Problem: LL hook callbacks are delivered as sent messages (high priority).
// WM_INPUT is a posted message (lower priority). GetMessage processes sent
// messages before posted ones, so the LWin keyup hook fires BEFORE WM_INPUT
// for any AHK-swallowed key (e.g. Z, 1) is dispatched -- making had_other_key
// still false at decision time.
//
// Fix: GetRawInputBuffer reads the raw input buffer directly, bypassing the
// WM_INPUT dispatch queue. The buffer is populated at physical-key time (before
// any hook fires), so it always contains Z's data when LWin keyup hook runs.
static void DrainRawInput() {
    UINT sz = 0;
    UINT rc = GetRawInputBuffer(NULL, &sz, sizeof(RAWINPUTHEADER));
    Logf("DrainRawInput: query rc=%u sz=%u", rc, sz);
    if (rc != 0 || sz == 0) return;

    std::vector<BYTE> buf(sz + sizeof(RAWINPUT)); // slack for 8-byte alignment
    UINT bufsz = (UINT)buf.size();               // must pass actual capacity, not query-result sz
    UINT cnt = GetRawInputBuffer((RAWINPUT*)buf.data(), &bufsz, sizeof(RAWINPUTHEADER));
    Logf("DrainRawInput: read cnt=%u err=%lu", cnt, cnt == (UINT)-1 ? GetLastError() : 0UL);
    if (cnt == (UINT)-1) {
        // sz>0 means data is present but unreadable — cancel conservatively
        Logf("DrainRawInput: read failed → cancel conservatively");
        EnterCriticalSection(&g_cs);
        CancelPendingCombos("raw buffer drain (read failed, data present)");
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (cnt == 0) return;

    bool cancel = false;
    RAWINPUT* ri = (RAWINPUT*)buf.data();
    for (UINT i = 0; i < cnt; ++i) {
        bool physical = (ri->header.hDevice != NULL);
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            bool isDown = !(ri->data.keyboard.Flags & RI_KEY_BREAK);
            USHORT vk = ri->data.keyboard.VKey;
            bool isMod = false;
            for (DWORD mod : MODIFIER_VKS)
                if (vk == (USHORT)mod) { isMod = true; break; }
            Logf("DrainRawInput[%u]: kbd VK=0x%02X isDown=%d isMod=%d physical=%d",
                 i, vk, isDown, isMod, physical);
            // Cancel on both physical and injected non-modifier keydowns.
            // Injected non-modifier keys (e.g. VK_CONTROL=0x11 injected by AHK's
            // "Send ^v" action) signal that a hotkey fired while the modifier was held.
            // WinKeyHook's own cleanup sends VK_LCONTROL (0xA2) which IS in MODIFIER_VKS
            // (isMod=true), so it is never caught here.
            if (isDown && !isMod) cancel = true;
        } else if (ri->header.dwType == RIM_TYPEMOUSE) {
            USHORT bf = ri->data.mouse.usButtonFlags;
            if (bf != 0) Logf("DrainRawInput[%u]: mouse flags=0x%04X physical=%d", i, bf, physical);
            if (physical && (bf & (RI_MOUSE_LEFT_BUTTON_DOWN  |
                                   RI_MOUSE_RIGHT_BUTTON_DOWN  |
                                   RI_MOUSE_MIDDLE_BUTTON_DOWN |
                                   RI_MOUSE_BUTTON_4_DOWN      |
                                   RI_MOUSE_BUTTON_5_DOWN)))
                cancel = true;
        }
        // NEXTRAWINPUTBLOCK uses QWORD (unavailable with WIN32_LEAN_AND_MEAN);
        // equivalent: advance by dwSize then 8-byte align.
        ri = (RAWINPUT*)(((uintptr_t)((BYTE*)ri + ri->header.dwSize) + 7) & ~(uintptr_t)7);
    }

    Logf("DrainRawInput: cancel=%d", cancel);
    if (cancel) {
        EnterCriticalSection(&g_cs);
        CancelPendingCombos("raw buffer drain (keyup sync)");
        LeaveCriticalSection(&g_cs);
    }
}

// ---------- Raw input window proc ---------------------------------------------
// A message-only window receives WM_INPUT for every physical keyboard and mouse
// event, independently of the LL hook chain. This means AHK swallowing a key
// via its own hook (without calling CallNextHookEx) does NOT prevent us from
// seeing the physical event here.
//
// hDevice == NULL in RAWINPUTHEADER identifies injected (synthetic) events,
// e.g. from SendInput. We skip those so our own InjectModifierCleanup calls
// and AHK's injected keystrokes do not falsely cancel a pending combo.
//
// Timing guarantee: WM_INPUT is posted to the message queue from the HID driver
// before the keyboard event reaches the LL hook pipeline. Because both messages
// go through the same FIFO queue on the main thread, WM_INPUT for key N is
// always processed before the LL-hook callback for any key pressed after N.
// Therefore, when the LWin keyup LL hook fires, all WM_INPUT messages for keys
// pressed while LWin was held are already dispatched and had_other_key is set.
static const wchar_t* MSG_WND_CLASS = L"WinKeyHookMsgWnd";

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
        if (sz > 0 && sz <= 256) {
            BYTE buf[256];
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &sz,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                bool physical = (ri->header.hDevice != NULL);
                bool cancel = false;
                if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                    bool isDown = !(ri->data.keyboard.Flags & RI_KEY_BREAK);
                    USHORT vk = ri->data.keyboard.VKey;
                    bool isMod = false;
                    for (DWORD mod : MODIFIER_VKS)
                        if (vk == (USHORT)mod) { isMod = true; break; }
                    Logf("WM_INPUT: kbd VK=0x%02X isDown=%d isMod=%d physical=%d",
                         vk, isDown, isMod, physical);
                    // Same rationale as DrainRawInput: cancel on injected non-modifier
                    // keydowns too (AHK action indicator), not just physical ones.
                    if (isDown && !isMod) cancel = true;
                } else if (ri->header.dwType == RIM_TYPEMOUSE) {
                    USHORT bf = ri->data.mouse.usButtonFlags;
                    if (bf != 0) Logf("WM_INPUT: mouse flags=0x%04X physical=%d", bf, physical);
                    if (physical && (bf & (RI_MOUSE_LEFT_BUTTON_DOWN  |
                                          RI_MOUSE_RIGHT_BUTTON_DOWN  |
                                          RI_MOUSE_MIDDLE_BUTTON_DOWN |
                                          RI_MOUSE_BUTTON_4_DOWN      |
                                          RI_MOUSE_BUTTON_5_DOWN)))
                        cancel = true;
                }
                if (cancel) {
                    EnterCriticalSection(&g_cs);
                    CancelPendingCombos("WM_INPUT (physical key/mouse)");
                    LeaveCriticalSection(&g_cs);
                }
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- Modifier cleanup helper -------------------------------------------
// Called when a modifier-only combo fires on keyup (solo press).
// We suppressed the physical keyup -- inject a Ctrl tap to "poison" the modifier
// press (prevents Start Menu / Ctrl+Alt+Del prompt), then inject the modifier keyup
// to release Windows's stuck modifier state. Both events carry LLKHF_INJECTED so
// our hook skips them; AHK ignores injected events for hotkey triggering by default.
static void InjectModifierCleanup(DWORD modVk) {
    INPUT inputs[3] = {};
    // Ctrl down (poison -- tells Windows another key was pressed while mod held)
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = VK_LCONTROL;
    inputs[0].ki.dwFlags = 0;
    // Ctrl up
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = VK_LCONTROL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    // Modifier keyup
    inputs[2].type       = INPUT_KEYBOARD;
    inputs[2].ki.wVk     = (WORD)modVk;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP |
                           ((modVk == VK_LWIN || modVk == VK_RWIN) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(3, inputs, sizeof(INPUT));
}

// ---------- Start Menu taint helper -------------------------------------------
// Injects a VK_NONAME (0xFF) tap while a Win key is physically held. VK_NONAME
// is an unassigned virtual key -- no application responds to it, but the Windows
// shell counts it as "another key pressed while Win held", suppressing the Start
// Menu on Win release. Used when Win+mouse-click occurs: mouse clicks are invisible
// to the shell's combo logic, so without this the Start Menu opens on Win release.
// The injected events carry LLKHF_INJECTED -> LowLevelKeyboardProc ignores them.
static void InjectStartMenuTaint() {
    INPUT inputs[2] = {};
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = VK_NONAME;       // 0xFF, documented unassigned VK
    inputs[0].ki.dwFlags = 0;
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = VK_NONAME;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// ---------- Watchdog thread ----------------------------------------------------
DWORD WINAPI WatchdogThread(LPVOID param) {
    DWORD  pid     = (DWORD)(uintptr_t)param;
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hParent) { PostThreadMessage(g_main_thread_id, WM_QUIT, 0, 0); return 0; }
    WaitForSingleObject(hParent, INFINITE);
    CloseHandle(hParent);
    PostThreadMessage(g_main_thread_id, WM_QUIT, 0, 0);
    return 0;
}

// ---------- Pipe client thread -------------------------------------------------
// Uses PeekNamedPipe polling instead of blocking ReadFile so that the hook
// thread can call WriteFile(pipe, "TRIGGERED ...") concurrently without
// deadlocking on the synchronous pipe handle's I/O serialization.
struct ClientThreadParam { HANDLE pipe; DWORD id; };

DWORD WINAPI ClientThread(LPVOID raw) {
    auto* param = (ClientThreadParam*)raw;
    HANDLE pipe = param->pipe;
    DWORD  cid  = param->id;
    delete param;

    Logf("CLIENT %u connected", cid);
    PipeSend(pipe, "HELLO WinKeyHook\n");

    std::string inbuf;

    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) break;

        if (avail == 0) {
            Sleep(10);
            continue;
        }

        char c; DWORD rd;
        if (!ReadFile(pipe, &c, 1, &rd, NULL) || rd == 0) break;
        if (c == '\r') continue;
        if (c != '\n') { if (inbuf.size() < 4096) inbuf += c; continue; }

        if (inbuf.empty()) continue;
        std::string line = inbuf;
        inbuf.clear();

        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        for (char& ch : cmd) ch = (char)toupper((unsigned char)ch);

        if (cmd == "PING") { PipeSend(pipe, "PONG\n"); continue; }

        if (cmd == "REGISTER") {
            std::string spec, name;
            ss >> spec;
            ss >> name;

            if (spec.empty()) { PipeSend(pipe, "ERR missing_spec\n"); continue; }

            Combo combo = ParseCombo(spec);
            if (combo.slots.empty()) { PipeSend(pipe, "ERR invalid_combo\n"); continue; }
            combo.name            = name;
            combo.owner_client_id = cid;

            std::string ident = combo.identifier();

            EnterCriticalSection(&g_cs);
            bool dup = false;
            for (const auto& c : g_combos)
                if (c.owner_client_id == cid && c.identifier() == ident) { dup = true; break; }
            if (!dup) g_combos.push_back(std::move(combo));
            LeaveCriticalSection(&g_cs);

            if (dup) {
                PipeSend(pipe, "ERR already_registered\n");
            } else {
                char ok[256];
                sprintf_s(ok, "OK %s\n", ident.c_str());
                PipeSend(pipe, ok);
                Logf("CLIENT %u registered '%s'", cid, ident.c_str());
            }
            continue;
        }

        if (cmd == "UNREGISTER") {
            std::string id; ss >> id;
            if (id.empty()) { PipeSend(pipe, "ERR missing_id\n"); continue; }

            bool found = false;
            EnterCriticalSection(&g_cs);
            for (auto it = g_combos.begin(); it != g_combos.end(); ) {
                if (it->owner_client_id == cid && (it->name == id || it->spec == id))
                    { it = g_combos.erase(it); found = true; }
                else ++it;
            }
            LeaveCriticalSection(&g_cs);

            PipeSend(pipe, found ? "OK\n" : "ERR not_found\n");
            Logf("CLIENT %u unregistered '%s' found=%d", cid, id.c_str(), found);
            continue;
        }

        PipeSend(pipe, "ERR unknown_command\n");
    }

    Logf("CLIENT %u disconnected", cid);
    bool should_quit = false;
    EnterCriticalSection(&g_cs);
    for (auto it = g_combos.begin(); it != g_combos.end(); )
        it = (it->owner_client_id == cid) ? g_combos.erase(it) : ++it;
    for (auto& c : g_clients)
        if (c.id == cid) { c.alive = false; break; }

    // Auto-exit when last pipe client disconnects and no CLI combos are registered.
    // Avoids the daemon staying alive forever after its only user (e.g. InputBar) exits.
    bool has_live_client = false;
    for (const auto& c : g_clients) if (c.alive) { has_live_client = true; break; }
    bool has_cli_combo = false;
    for (const auto& co : g_combos) if (co.cli_index >= 0) { has_cli_combo = true; break; }
    if (!has_live_client && !has_cli_combo) should_quit = true;
    LeaveCriticalSection(&g_cs);

    if (should_quit) {
        Logf("CLIENT %u was last client, no CLI combos -- posting quit", cid);
        PostThreadMessage(g_main_thread_id, WM_QUIT, 0, 0);
    }

    CloseHandle(pipe);
    return 0;
}

// ---------- Pipe accept thread -------------------------------------------------
DWORD WINAPI PipeAcceptThread(LPVOID) {
    // NULL DACL lets non-admin processes connect to the pipe.
    SECURITY_DESCRIPTOR sd = {};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle       = FALSE;

    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUF, PIPE_BUF, 0, &sa
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            Logf("PipeAccept: CreateNamedPipe error %lu, retry in 500ms", GetLastError());
            Sleep(500);
            continue;
        }

        if (!ConnectNamedPipe(pipe, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                continue;
            }
        }

        DWORD cid = g_next_client_id++;

        EnterCriticalSection(&g_cs);
        for (auto it = g_clients.begin(); it != g_clients.end(); )
            it = (!it->alive) ? g_clients.erase(it) : ++it;
        Client c; c.id = cid; c.pipe = pipe; c.alive = true;
        g_clients.push_back(c);
        LeaveCriticalSection(&g_cs);

        Logf("PipeAccept: client %u connected", cid);
        auto* p = new ClientThreadParam{pipe, cid};
        HANDLE ht = CreateThread(NULL, 0, ClientThread, p, 0, NULL);
        if (ht) CloseHandle(ht);
    }
    return 0;
}

// ---------- Desktop-switch event hook -----------------------------------------
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event,
                            HWND, LONG, LONG, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
        Log("DESKTOP_SWITCH: clearing state");
        g_pressed.clear();
        g_suppressed.clear();
        // ClientThread runs on a worker thread and modifies g_combos under g_cs,
        // so we must hold g_cs here even though WinEventProc is on the main thread.
        EnterCriticalSection(&g_cs);
        for (auto& c : g_combos) { c.active = false; c.pending_trigger = false; c.had_other_key = false; }
        LeaveCriticalSection(&g_cs);
        g_pending_foreground = NULL;
        g_pending_fg_changed = false;
    } else if (event == EVENT_SYSTEM_FOREGROUND) {
        // Track any foreground switch while a modifier-only combo is pending.
        // Win+1 twice (A→B→A) would fool a simple HWND equality check at keyup,
        // but this flag catches both transitions.
        if (g_pending_foreground != NULL)
            g_pending_fg_changed = true;
    }
}

// ---------- Low-level mouse hook -----------------------------------------------
// Belt-and-suspenders: also cancels pending modifier-only combos on mouse button
// down via the LL hook, in addition to the raw input path. Covers the common case
// where our hook is first in the chain and fires before the WM_INPUT message is
// dispatched, ensuring zero-latency cancellation.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);

    bool isButtonDown = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                         wParam == WM_MBUTTONDOWN  || wParam == WM_XBUTTONDOWN);

    if (isButtonDown) {
        // g_pressed is hook-thread-local: LL keyboard and LL mouse hooks run on
        // the same thread, so reading it here requires no lock.
        bool win_held = g_pressed.count(VK_LWIN) || g_pressed.count(VK_RWIN);

        bool taint_start_menu = false;
        EnterCriticalSection(&g_cs);
        CancelPendingCombos("mouse hook (belt-and-suspenders)");
        if (win_held) {
            // Only taint when a Win combo is registered -- leaves Start Menu
            // untouched for users who have no Win-key hotkeys.
            for (const auto& c : g_combos)
                if (c.hasVk(VK_LWIN) || c.hasVk(VK_RWIN)) { taint_start_menu = true; break; }
        }
        LeaveCriticalSection(&g_cs);

        // SendInput outside g_cs -- keeps blocking calls out of the critical section.
        if (taint_start_menu) {
            Logf("MOUSE+WIN: injecting VK_NONAME taint to suppress Start Menu");
            InjectStartMenuTaint();
        }
    }

    return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);
}

// ---------- Low-level keyboard hook -------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

    if (p->flags & LLKHF_INJECTED)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    bool keyDown = (wParam == WM_KEYDOWN   || wParam == WM_SYSKEYDOWN);
    bool keyUp   = (wParam == WM_KEYUP     || wParam == WM_SYSKEYUP);

    // -- KEY DOWN --
    if (keyDown) {
        bool autoRepeat = g_pressed.count(p->vkCode) > 0;

        if (!autoRepeat) {
            g_pressed.insert(p->vkCode);

            bool suppress = false;

            // Phase 1: cancel any pending modifier-only combo interrupted by a non-combo key.
            // The modifier was passed through on keydown (not suppressed), so no injection
            // is needed -- AHK already sees the modifier held, and the new key passes normally.
            // Note: if AHK swallows the key without calling CallNextHookEx, this doesn't run --
            // the raw input path (MsgWndProc / WM_INPUT) handles that case instead.
            {
                EnterCriticalSection(&g_cs);
                for (auto& combo : g_combos) {
                    if (combo.pending_trigger && !combo.hasVk(p->vkCode)) {
                        Logf("PHASE1 CANCEL: '%s' interrupted by VK=0x%02X",
                             combo.identifier().c_str(), p->vkCode);
                        combo.had_other_key   = true;
                        combo.pending_trigger = false;
                        combo.active          = false;
                    }
                }
                LeaveCriticalSection(&g_cs);
            }

            // Phase 2: check for newly satisfied combos.
            // Only evaluate a combo when the pressed key belongs to it -- prevents
            // re-firing cancelled modifier-only combos when unrelated keys come in.
            std::vector<PendingWrite> pw;
            EnterCriticalSection(&g_cs);
            for (auto& combo : g_combos) {
                if (!combo.active && !combo.pending_trigger && combo.hasVk(p->vkCode) && combo.satisfied(g_pressed)) {
                    if (combo.isSolo()) {
                        bool mod_held = false;
                        for (DWORD mod : MODIFIER_VKS)
                            if (g_pressed.count(mod)) { mod_held = true; break; }
                        if (mod_held) continue;
                    }

                    if (combo.isModifierOnly()) {
                        // Pass modifier through so AHK sees a physical keydown.
                        // Fire on keyup instead (suppress keyup + inject cleanup there).
                        combo.pending_trigger = true;
                        combo.had_other_key   = false;
                        Logf("PENDING SET: '%s' VK=0x%02X", combo.identifier().c_str(), p->vkCode);
                        ClearInputHistory();  // zero driver-level bit-0 so keyup fallback only sees keys pressed after this moment
                        if (g_pending_foreground == NULL) {
                            g_pending_foreground = GetForegroundWindow();
                            g_pending_fg_changed  = false;
                        }
                        // suppress stays false -- key passes to Windows/AHK normally
                    } else {
                        combo.active = true;
                        suppress = true;
                        FireCombo(combo, pw);
                    }
                }
            }
            LeaveCriticalSection(&g_cs);

            FlushPendingWrites(pw);  // WriteFile outside CS

            if (suppress) {
                g_suppressed.insert(p->vkCode);
                return 1;
            }
        } else {
            if (g_suppressed.count(p->vkCode)) return 1;
        }
    }

    // -- KEY UP --
    if (keyUp) {
        g_pressed.erase(p->vkCode);

        // Drain pending raw input synchronously before deciding to fire.
        // Catches physical keys/clicks swallowed by AHK (not seen by our LL hook)
        // whose WM_INPUT hasn't been dispatched yet due to sent-vs-posted priority.
        {
            bool hasPending = false;
            EnterCriticalSection(&g_cs);
            for (const auto& combo : g_combos)
                if (combo.pending_trigger && combo.hasVk(p->vkCode)) { hasPending = true; break; }
            LeaveCriticalSection(&g_cs);
            Logf("KEYUP: VK=0x%02X hasPending=%d", p->vkCode, hasPending);
            if (hasPending) DrainRawInput();
            if (hasPending && g_pending_foreground != NULL) {
                HWND cur_fg = GetForegroundWindow();
                bool fg_changed = g_pending_fg_changed || (cur_fg != g_pending_foreground);
                if (fg_changed) {
                    Logf("FG CHANGED: event_flag=%d was=%p now=%p → cancel",
                         g_pending_fg_changed, (void*)g_pending_foreground, (void*)cur_fg);
                    EnterCriticalSection(&g_cs);
                    CancelPendingCombos("foreground window changed (Win+shortcut)");
                    LeaveCriticalSection(&g_cs);
                }
                g_pending_foreground = NULL;
                g_pending_fg_changed = false;
            }

            // Final fallback: driver-level key history.  Catches Win+1–9 when
            // the target app is already focused (no foreground change) and the
            // second key was consumed by Windows Shell before reaching raw input
            // or the LL hook chain.  GetAsyncKeyState bit 0 is set at the HID
            // driver level — below Shell, below AHK.
            if (hasPending) {
                bool input_fallback = AnyNonModifierKeyPressedSince();
                Logf("KEYUP FALLBACK: AnyNonModifierKeyPressedSince=%d", input_fallback);
                if (input_fallback) {
                    EnterCriticalSection(&g_cs);
                    CancelPendingCombos("driver-level key history (Win+shortcut, app already focused)");
                    LeaveCriticalSection(&g_cs);
                }
            }
        }

        DWORD cleanup_vk = 0;
        std::vector<PendingWrite> pw;
        EnterCriticalSection(&g_cs);
        for (auto& combo : g_combos) {
            if (combo.pending_trigger && combo.hasVk(p->vkCode)) {
                combo.pending_trigger = false;
                combo.active          = false;
                Logf("KEYUP DECISION: '%s' had_other_key=%d → %s",
                     combo.identifier().c_str(), combo.had_other_key,
                     combo.had_other_key ? "CANCEL" : "FIRE");
                if (!combo.had_other_key) {
                    // Solo modifier press: suppress keyup to prevent Start Menu,
                    // then inject cleanup sequence to release Windows modifier state.
                    cleanup_vk = p->vkCode;
                    FireCombo(combo, pw);
                }
            }
            if (combo.active && !combo.satisfied(g_pressed))
                combo.active = false;
        }
        LeaveCriticalSection(&g_cs);

        FlushPendingWrites(pw);  // WriteFile outside CS

        if (cleanup_vk != 0) {
            InjectModifierCleanup(cleanup_vk);
            return 1;  // suppress physical keyup
        }

        if (g_suppressed.count(p->vkCode)) {
            g_suppressed.erase(p->vkCode);
            return 1;
        }
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------- Tutorial -----------------------------------------------------------
static void PrintTutorial() {
    printf(
        "\n"
        "  +---------------------------------------------------------+\n"
        "  |              WinKeyHook.exe  --  keyboard daemon          |\n"
        "  +---------------------------------------------------------+\n"
        "\n"
        "  Singleton low-level keyboard hook.\n"
        "  Any number of programs connect via named pipe and register\n"
        "  their own combos. Events are routed only to the owner.\n"
        "  All state is in RAM -- no files written.\n"
        "\n"
        "  USAGE\n"
        "    WinKeyHook.exe <parent_pid> [<combo> [--name <name>] ...]\n"
        "\n"
        "  ARGUMENTS\n"
        "    parent_pid   PID to watch (quit when it dies). 0 = run forever.\n"
        "    combo        Keys joined by '+', up to 4 keys per combo.\n"
        "    --name       Optional label for the preceding combo.\n"
        "\n"
        "  STDOUT OUTPUT (for CLI combos)\n"
        "    HOOK_STARTED         hook is live\n"
        "    ALREADY_RUNNING      another instance is already up\n"
        "    TRIGGERED:0 Keybind1 combo index 0 was pressed\n"
        "\n"
        "  EXAMPLES\n"
        "    WinKeyHook.exe 0\n"
        "      Daemon only -- pipe clients register combos dynamically.\n"
        "\n"
        "    WinKeyHook.exe 0 \"LCtrl+LShift+Alt+F1\" --name \"Keybind1\"\n"
        "      One named CLI combo. Press it -> TRIGGERED:0 Keybind1\n"
        "\n"
        "    WinKeyHook.exe <pid> \"Win+F1\" \"RAlt+F2\" --name \"Snap\"\n"
        "      Die when <pid> exits. Two combos, second is named.\n"
        "\n"
        "  PIPE INTERFACE   \\\\.\\pipe\\WinKeyHook\n"
        "    Connect from any process. Only one instance runs.\n"
        "\n"
        "    CLIENT -> SERVER (lines ending with \\n):\n"
        "      REGISTER <spec> [<name>]    add combo, owned by your client\n"
        "      UNREGISTER <name_or_spec>   remove combo\n"
        "      PING                        keepalive\n"
        "\n"
        "    SERVER -> CLIENT:\n"
        "      HELLO WinKeyHook            sent on connect\n"
        "      OK <name_or_spec>           command succeeded\n"
        "      ERR <reason>                command failed\n"
        "      TRIGGERED <name> <spec>     your combo was pressed\n"
        "      PONG                        response to PING\n"
        "\n"
        "    Client disconnect -> all its combos removed automatically.\n"
        "\n"
        "  KEY NAMES\n"
        "    Sided   : LCtrl RCtrl  LShift RShift  LAlt RAlt  LWin RWin\n"
        "    Unsided : Ctrl  Shift  Alt    Win       (matches either side)\n"
        "    Fn keys : F1 - F24\n"
        "    Letters : A-Z    Digits: 0-9\n"
        "    Special : Space Tab Enter Esc Back Del Ins Home End\n"
        "              PgUp PgDn Up Down Left Right  Num0-Num9\n"
        "    Raw VK  : 0xBF  (any hex virtual-key code)\n"
        "\n"
        "  PYTHON QUICK-START\n"
        "    import subprocess, os, time, win32file, win32pipe, pywintypes\n"
        "    PIPE = r'\\\\\\\\.\\\\pipe\\\\WinKeyHook'\n"
        "    proc = subprocess.Popen(['WinKeyHook.exe', str(os.getpid())],\n"
        "                            stdout=subprocess.PIPE,\n"
        "                            creationflags=0x08000000)\n"
        "    if proc.stdout.readline().strip() == b'ALREADY_RUNNING':\n"
        "        proc.wait()\n"
        "    for _ in range(20):          # wait for pipe\n"
        "        try:\n"
        "            h = win32file.CreateFile(PIPE,\n"
        "                win32file.GENERIC_READ | win32file.GENERIC_WRITE,\n"
        "                0, None, win32file.OPEN_EXISTING, 0, None)\n"
        "            break\n"
        "        except pywintypes.error: time.sleep(0.1)\n"
        "    win32file.WriteFile(h, b'REGISTER LCtrl+F5 MyKey\\n')\n"
        "    while True:\n"
        "        _, data = win32file.ReadFile(h, 4096)\n"
        "        for line in data.decode().splitlines():\n"
        "            if line.startswith('TRIGGERED'):\n"
        "                print('Fired:', line)\n"
        "\n"
        "  COMPILE\n"
        "    cl.exe /EHsc WinKeyHook.cpp /link user32.lib advapi32.lib\n"
        "\n"
        "  +---------------------------------------------------------+\n"
        "\n"
    );
}

// ---------- Update (--update) --------------------------------------------------
static std::string GithubApiGet(const wchar_t* path) {
    HINTERNET hSes = WinHttpOpen(L"WinKeyHook-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return {};

    HINTERNET hCon = WinHttpConnect(hSes, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return {}; }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return {}; }

    WinHttpAddRequestHeaders(hReq,
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    std::string body;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
     && WinHttpReceiveResponse(hReq, NULL)) {
        DWORD sz = 0;
        while (WinHttpQueryDataAvailable(hReq, &sz) && sz > 0) {
            std::vector<char> buf(sz + 1, 0);
            DWORD rd = 0;
            if (!WinHttpReadData(hReq, buf.data(), sz, &rd)) break;
            body.append(buf.data(), rd);
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return body;
}

static std::string ExtractJsonField(const std::string& json, const std::string& key) {
    for (const char* sep : {"\":\"", "\": \""}) {
        std::string needle = "\"" + key + sep;
        auto pos = json.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end != std::string::npos) return json.substr(pos, end - pos);
    }
    return {};
}

static std::tuple<int,int,int> ParseSemVer(const std::string& v) {
    const char* s = v.c_str();
    if (*s == 'v' || *s == 'V') ++s;
    int maj = 0, min_v = 0, pat = 0;
    sscanf_s(s, "%d.%d.%d", &maj, &min_v, &pat);
    return {maj, min_v, pat};
}

static int DoUpdate() {
    std::cout << "Checking for updates..." << std::endl;

    std::string json = GithubApiGet(L"/repos/BlessEphraem/WinKeyHook/releases/latest");
    if (json.empty()) {
        std::cerr << "ERR: Could not reach GitHub API\n";
        return 1;
    }

    std::string tag = ExtractJsonField(json, "tag_name");
    if (tag.empty()) {
        std::cerr << "ERR: Could not parse release info\n";
        return 1;
    }

    std::cout << "Installed : " << WINKEYHOOK_VERSION << "\n";
    std::cout << "Latest    : " << tag << "\n";

    if (ParseSemVer(tag) <= ParseSemVer(WINKEYHOOK_VERSION)) {
        std::cout << "Already up to date.\n";
        return 0;
    }

    // Build download URL: tag already contains 'v' prefix (e.g. "v1.0.4")
    std::string url = "https://github.com/BlessEphraem/WinKeyHook/releases/download/"
        + tag + "/WinKeyHook_" + tag + "_Setup.exe";

    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring destW = std::wstring(tmpDir) + L"WinKeyHook_update.exe";

    std::cout << "Downloading " << tag << "..." << std::endl;

    std::wstring urlW(url.begin(), url.end());  // URL is pure ASCII
    HRESULT hr = URLDownloadToFileW(NULL, urlW.c_str(), destW.c_str(), 0, NULL);
    if (FAILED(hr)) {
        std::cerr << "ERR: Download failed (0x" << std::hex << hr << ")\n";
        return 1;
    }

    std::cout << "Launching installer..." << std::endl;
    HINSTANCE res = ShellExecuteW(NULL, L"runas", destW.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)res <= 32) {
        std::cerr << "ERR: Could not launch installer (code " << (INT_PTR)res << ")\n";
        return 1;
    }

    return 0;
}

// ---------- Entry point --------------------------------------------------------
int main(int argc, char* argv[]) {
    // Hide the console window when running as a daemon (argc >= 2, not a query
    // flag). Task Scheduler and ShellExecute don't pass CREATE_NO_WINDOW, so a
    // console window would appear briefly. ShowWindow suppresses it immediately
    // without affecting stdout (pipe handles used by parent processes are
    // unaffected by hiding the console window).
    if (argc >= 2) {
        std::string firstArg = argv[1];
        bool is_daemon = (firstArg != "--version" && firstArg != "--update");
        if (is_daemon) {
            HWND consoleWnd = GetConsoleWindow();
            if (consoleWnd) ShowWindow(consoleWnd, SW_HIDE);
        }
    }

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--version") {
            std::cout << WINKEYHOOK_VERSION << std::endl;
            return 0;
        }
        if (arg == "--update") {
            return DoUpdate();
        }
    }
    if (argc < 2) {
        CONSOLE_SCREEN_BUFFER_INFO csbi = {};
        bool direct_launch = false;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
            direct_launch = (csbi.dwCursorPosition.X == 0 && csbi.dwCursorPosition.Y == 0);

        PrintTutorial();

        if (direct_launch) {
            printf("  Press any key to close...");
            fflush(stdout);
            _getch();
        }
        return 0;
    }

    // -- Singleton --
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "ALREADY_RUNNING" << std::endl;
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    LogOpen();
    Log("=== WinKeyHook started ===");

    DWORD parent_pid = 0;
    try { parent_pid = (DWORD)std::stoul(argv[1]); }
    catch (...) { std::cerr << "ERREUR: invalid PID\n"; return 1; }

    InitializeCriticalSection(&g_cs);

    // -- Parse CLI combos: <spec> [--name <name>] ... --
    int cli_idx = 0;
    for (int i = 2; i < argc; ) {
        std::string arg(argv[i]);
        if (arg == "--name") { i += 2; continue; }

        std::string spec = arg;
        std::string name;
        if (i + 1 < argc && std::string(argv[i+1]) == "--name" && i + 2 < argc) {
            name = argv[i+2];
            i += 3;
        } else {
            i++;
        }

        Combo combo = ParseCombo(spec);
        if (combo.slots.empty()) {
            std::cerr << "WARNING: combo '" << spec << "' has no valid keys, skipped\n";
            continue;
        }
        combo.name            = name;
        combo.owner_client_id = 0;
        combo.cli_index       = cli_idx++;
        Logf("CLI combo[%d] spec='%s' name='%s'", combo.cli_index, spec.c_str(), name.c_str());
        g_combos.push_back(std::move(combo));
    }

    g_main_thread_id = GetCurrentThreadId();

    if (parent_pid != 0) {
        HANDLE h = CreateThread(NULL, 0, WatchdogThread, (LPVOID)(uintptr_t)parent_pid, 0, NULL);
        if (h) CloseHandle(h);
    }

    {
        HANDLE h = CreateThread(NULL, 0, PipeAcceptThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }

    g_win_event_hook = SetWinEventHook(
        EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_win_event_hook)
        Log("WARNING: SetWinEventHook(DESKTOPSWITCH) failed -- UAC/lock recovery disabled");

    g_fg_event_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (!g_fg_event_hook)
        Log("WARNING: SetWinEventHook(FOREGROUND) failed -- Win+number cancel may miss double-switch");

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!g_hook) { std::cerr << "ERREUR_HOOK\n"; return 1; }

    g_mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
    if (!g_mouse_hook)
        Log("WARNING: WH_MOUSE_LL failed -- mouse-button combo cancellation may be delayed");

    // -- Raw input: message-only window -----------------------------------------
    // Receives physical keyboard + mouse events independently of the LL hook chain.
    // Injected events (SendInput) have hDevice == NULL and are ignored in MsgWndProc.
    {
        WNDCLASSW wc    = {};
        wc.lpfnWndProc  = MsgWndProc;
        wc.hInstance    = GetModuleHandleW(NULL);
        wc.lpszClassName = MSG_WND_CLASS;
        if (RegisterClassW(&wc)) {
            g_msg_wnd = CreateWindowExW(0, MSG_WND_CLASS, NULL, 0,
                                         0, 0, 0, 0,
                                         HWND_MESSAGE, NULL,
                                         GetModuleHandleW(NULL), NULL);
        }
        if (g_msg_wnd) {
            RAWINPUTDEVICE rids[2] = {};
            rids[0].usUsagePage = 0x01; rids[0].usUsage = 0x06; // keyboard
            rids[0].dwFlags     = RIDEV_INPUTSINK;
            rids[0].hwndTarget  = g_msg_wnd;
            rids[1].usUsagePage = 0x01; rids[1].usUsage = 0x02; // mouse
            rids[1].dwFlags     = RIDEV_INPUTSINK;
            rids[1].hwndTarget  = g_msg_wnd;
            if (RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE)))
                Log("Raw input registered.");
            else
                Log("WARNING: RegisterRawInputDevices failed -- modifier-combo cancellation may miss swallowed keys");
        } else {
            Log("WARNING: message window creation failed -- raw input unavailable");
        }
    }

    Log("Hooks installed.");
    std::cout << "HOOK_STARTED" << std::endl;
    std::cout.flush();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_pressed.clear();
    g_suppressed.clear();
    UnhookWindowsHookEx(g_hook);
    if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
    if (g_win_event_hook) UnhookWinEvent(g_win_event_hook);
    if (g_fg_event_hook)  UnhookWinEvent(g_fg_event_hook);
    if (g_msg_wnd) {
        DestroyWindow(g_msg_wnd);
        UnregisterClassW(MSG_WND_CLASS, GetModuleHandleW(NULL));
    }
    DeleteCriticalSection(&g_cs);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    Log("=== WinKeyHook stopped ===");
    return 0;
}
