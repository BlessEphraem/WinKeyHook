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
#include <cstdarg>
#include <conio.h>

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

static const wchar_t* PIPE_NAME  = L"\\\\.\\pipe\\WinKeyHook";
static const wchar_t* MUTEX_NAME = L"Global\\WinKeyHookSingleton";
static const DWORD    PIPE_BUF   = 4096;

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
DWORD          g_main_thread_id = 0;

CRITICAL_SECTION     g_cs;         // guards g_combos + g_clients
std::vector<Combo>   g_combos;
std::vector<Client>  g_clients;

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

// ---------- Modifier cleanup helper -------------------------------------------
// ---------- GetAsyncKeyState helpers for modifier-only combos -----------------
// When another tool (e.g. AHK) handles a hotkey without calling CallNextHookEx,
// our hook never sees the second key. These helpers use the "since-last-call"
// history bit (bit 0) of GetAsyncKeyState to detect missed presses.
// Call ClearInputHistory() when pending_trigger becomes true, then call
// AnyNonModifierKeyPressedSince() at keyup to decide whether to cancel.
static void ClearInputHistory() {
    GetAsyncKeyState(VK_LBUTTON); GetAsyncKeyState(VK_RBUTTON);
    GetAsyncKeyState(VK_MBUTTON); GetAsyncKeyState(VK_XBUTTON1);
    GetAsyncKeyState(VK_XBUTTON2);
    for (int vk = 'A'; vk <= 'Z';        vk++) GetAsyncKeyState(vk);
    for (int vk = '0'; vk <= '9';        vk++) GetAsyncKeyState(vk);
    for (int vk = VK_F1; vk <= VK_F24;   vk++) GetAsyncKeyState(vk);
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; vk++) GetAsyncKeyState(vk);
    GetAsyncKeyState(VK_SPACE);    GetAsyncKeyState(VK_TAB);
    GetAsyncKeyState(VK_RETURN);   GetAsyncKeyState(VK_ESCAPE);
    GetAsyncKeyState(VK_BACK);     GetAsyncKeyState(VK_DELETE);
    GetAsyncKeyState(VK_INSERT);   GetAsyncKeyState(VK_HOME);
    GetAsyncKeyState(VK_END);      GetAsyncKeyState(VK_PRIOR);
    GetAsyncKeyState(VK_NEXT);     GetAsyncKeyState(VK_UP);
    GetAsyncKeyState(VK_DOWN);     GetAsyncKeyState(VK_LEFT);
    GetAsyncKeyState(VK_RIGHT);    GetAsyncKeyState(VK_APPS);
    GetAsyncKeyState(VK_SNAPSHOT); GetAsyncKeyState(VK_SCROLL);
    GetAsyncKeyState(VK_PAUSE);    GetAsyncKeyState(VK_CAPITAL);
    GetAsyncKeyState(VK_NUMLOCK);
}
static bool AnyNonModifierKeyPressedSince() {
    bool r = false;
    r |= (GetAsyncKeyState(VK_LBUTTON)  & 1) != 0;
    r |= (GetAsyncKeyState(VK_RBUTTON)  & 1) != 0;
    r |= (GetAsyncKeyState(VK_MBUTTON)  & 1) != 0;
    r |= (GetAsyncKeyState(VK_XBUTTON1) & 1) != 0;
    r |= (GetAsyncKeyState(VK_XBUTTON2) & 1) != 0;
    for (int vk = 'A'; vk <= 'Z';        vk++) r |= (GetAsyncKeyState(vk) & 1) != 0;
    for (int vk = '0'; vk <= '9';        vk++) r |= (GetAsyncKeyState(vk) & 1) != 0;
    for (int vk = VK_F1; vk <= VK_F24;   vk++) r |= (GetAsyncKeyState(vk) & 1) != 0;
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; vk++) r |= (GetAsyncKeyState(vk) & 1) != 0;
    r |= (GetAsyncKeyState(VK_SPACE)    & 1) != 0;
    r |= (GetAsyncKeyState(VK_TAB)      & 1) != 0;
    r |= (GetAsyncKeyState(VK_RETURN)   & 1) != 0;
    r |= (GetAsyncKeyState(VK_ESCAPE)   & 1) != 0;
    r |= (GetAsyncKeyState(VK_BACK)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_DELETE)   & 1) != 0;
    r |= (GetAsyncKeyState(VK_INSERT)   & 1) != 0;
    r |= (GetAsyncKeyState(VK_HOME)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_END)      & 1) != 0;
    r |= (GetAsyncKeyState(VK_PRIOR)    & 1) != 0;
    r |= (GetAsyncKeyState(VK_NEXT)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_UP)       & 1) != 0;
    r |= (GetAsyncKeyState(VK_DOWN)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_LEFT)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_RIGHT)    & 1) != 0;
    r |= (GetAsyncKeyState(VK_APPS)     & 1) != 0;
    r |= (GetAsyncKeyState(VK_SNAPSHOT) & 1) != 0;
    r |= (GetAsyncKeyState(VK_SCROLL)   & 1) != 0;
    r |= (GetAsyncKeyState(VK_PAUSE)    & 1) != 0;
    r |= (GetAsyncKeyState(VK_CAPITAL)  & 1) != 0;
    r |= (GetAsyncKeyState(VK_NUMLOCK)  & 1) != 0;
    return r;
}

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
    EnterCriticalSection(&g_cs);
    for (auto it = g_combos.begin(); it != g_combos.end(); )
        it = (it->owner_client_id == cid) ? g_combos.erase(it) : ++it;
    for (auto& c : g_clients)
        if (c.id == cid) { c.alive = false; break; }
    LeaveCriticalSection(&g_cs);

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
    }
}

// ---------- Low-level mouse hook -----------------------------------------------
// Cancels any pending modifier-only combo when the user clicks a mouse button.
// The modifier was never suppressed (passes through on keydown), so AHK already
// sees it held -- no injection needed; Win+MButton AHK shortcuts work natively.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_mouse_hook, nCode, wParam, lParam);

    bool isButtonDown = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                         wParam == WM_MBUTTONDOWN  || wParam == WM_XBUTTONDOWN);

    if (isButtonDown) {
        EnterCriticalSection(&g_cs);
        for (auto& combo : g_combos) {
            if (combo.pending_trigger) {
                combo.had_other_key   = true;
                combo.pending_trigger = false;
                combo.active          = false;
            }
        }
        LeaveCriticalSection(&g_cs);
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
            {
                EnterCriticalSection(&g_cs);
                for (auto& combo : g_combos) {
                    if (combo.pending_trigger && !combo.hasVk(p->vkCode)) {
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
            bool pending_modifier_set = false;
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
                        pending_modifier_set   = true;
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

            if (pending_modifier_set)
                ClearInputHistory();

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

        // Fallback: detect keys/clicks AHK may have swallowed without calling CallNextHookEx.
        // AnyNonModifierKeyPressedSince() consumes all history bits (no bleed into next window).
        bool input_fallback = AnyNonModifierKeyPressedSince();

        DWORD cleanup_vk = 0;
        std::vector<PendingWrite> pw;
        EnterCriticalSection(&g_cs);
        for (auto& combo : g_combos) {
            if (combo.pending_trigger && combo.hasVk(p->vkCode)) {
                combo.pending_trigger = false;
                combo.active          = false;
                if (!combo.had_other_key) {
                    if (input_fallback) {
                        combo.had_other_key = true;
                        Logf("CANCEL '%s': key/click detected via GetAsyncKeyState fallback",
                             combo.identifier().c_str());
                    } else {
                        // Solo modifier press: suppress keyup to prevent Start Menu,
                        // then inject cleanup sequence to release Windows modifier state.
                        cleanup_vk = p->vkCode;
                        FireCombo(combo, pw);
                    }
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

// ---------- Entry point --------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--version") {
            std::cout << "v1.0.1" << std::endl;
            return 0;
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
        Log("WARNING: SetWinEventHook failed -- UAC/lock recovery disabled");

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    if (!g_hook) { std::cerr << "ERREUR_HOOK\n"; return 1; }

    g_mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
    if (!g_mouse_hook)
        Log("WARNING: WH_MOUSE_LL failed -- Win+MouseButton combos may not work");

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
    DeleteCriticalSection(&g_cs);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    Log("=== WinKeyHook stopped ===");
    return 0;
}
