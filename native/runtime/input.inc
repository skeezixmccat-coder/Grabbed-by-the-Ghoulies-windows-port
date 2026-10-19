// Gamepad input (included by ghoulies_runtime.cpp before the replacement table).
//
// The title's XPP library drives the console's USB host controller. Instead of
// emulating USB, the XPP entry points the title calls are replaced (after
// their recorded bytes are verified) by implementations over Windows XInput:
// XInitDevices, XGetDevices, XGetDeviceChanges, XInputOpen, XInputClose,
// XInputGetState, XInputSetState. Connected PC controllers are packed onto
// Xbox ports in XInput slot order (see PhysicalSlots).
//
// Xbox XINPUT_STATE: DWORD dwPacketNumber; WORD wButtons (d-pad, start,
// back, thumbs: same bits as Windows); BYTE bAnalogButtons[8] (A, B, X, Y,
// Black, White, left trigger, right trigger); SHORT thumbs LX, LY, RX, RY.
// Mapping (same convention as xemu): A/B/X/Y -> A/B/X/Y (same colours and
// positions; digital -> 255/0), Black <- RB, White <- LB, Start <- Menu,
// Back <- View, d-pad, thumbstick clicks, analog triggers and sticks as is.

constexpr uint32_t kGamepadType = 0x001FCDC8;   // g_DeviceType_Gamepad (XbSymbolDatabase)
struct WinGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct WinState { DWORD dwPacketNumber; WinGamepad Gamepad; };
struct WinVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, WinState*);
using XInputSetStateFn = DWORD (WINAPI*)(DWORD, WinVibration*);
XInputGetStateFn pXInputGetState = nullptr;
XInputSetStateFn pXInputSetState = nullptr;
uint32_t g_reported_gamepads = 0;
CRITICAL_SECTION g_input_lock;

// Opt-in test controller: rows are presented-frame, hex Windows buttons,
// LX LY RX RY LT RT. Values persist until the next row. The default remains
// real XInput only. An explicit synthetic device lets unattended tests drive
// the same XPP ABI and original menu/gameplay code as a physical controller.
struct ScriptInput { uint64_t frame; WinGamepad pad; };
std::vector<ScriptInput> g_input_script;
size_t g_input_script_index = 0;
bool g_input_script_enabled = false;
bool g_input_script_ms = false;
ULONGLONG g_input_script_origin = 0;
bool g_input_script_started = false;

template<class T> bool InputNumber(const char*& p, T& value, int base = 10) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    const char* end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
    auto result = std::from_chars(p, end, value, base);
    p = end;
    return result.ec == std::errc{} && result.ptr == end;
}
bool ParseInputRow(const char* text, ScriptInput& row) {
    const char* at = text;
    uint64_t frame; unsigned buttons; int lx, ly, rx, ry, lt, rt;
    if (!InputNumber(at, frame) || !InputNumber(at, buttons, 16) ||
        !InputNumber(at, lx) || !InputNumber(at, ly) || !InputNumber(at, rx) ||
        !InputNumber(at, ry) || !InputNumber(at, lt) || !InputNumber(at, rt)) return false;
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') ++at;
    if (*at || frame > 10000000 || (buttons & ~0xF3FFu) ||
        lx < -32768 || lx > 32767 || ly < -32768 || ly > 32767 ||
        rx < -32768 || rx > 32767 || ry < -32768 || ry > 32767 ||
        lt < 0 || lt > 255 || rt < 0 || rt > 255) return false;
    row = {frame, {static_cast<WORD>(buttons), static_cast<BYTE>(lt), static_cast<BYTE>(rt),
        static_cast<SHORT>(lx), static_cast<SHORT>(ly), static_cast<SHORT>(rx), static_cast<SHORT>(ry)}};
    return true;
}

void LoadInputScript() {
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(L"GH_INPUT_SCRIPT", path, _countof(path));
    wchar_t clock[32] = {};
    DWORD clock_length = GetEnvironmentVariableW(L"GH_INPUT_SCRIPT_CLOCK", clock, _countof(clock));
    if (clock_length) {
        if (clock_length >= _countof(clock) || wcscmp(clock, L"milliseconds") || !length)
            Stop(236, "GH_INPUT_SCRIPT_CLOCK requires milliseconds and an input script");
        g_input_script_ms = true;
    }
    if (!length) return;
    if (length >= _countof(path)) Stop(236, "GH_INPUT_SCRIPT path too long");
    FILE* file = nullptr; _wfopen_s(&file, path, L"rb");
    if (!file) Stop(236, "cannot open GH_INPUT_SCRIPT");
    char line[512]; unsigned line_number = 0;
    while (fgets(line, sizeof line, file)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file)) Stop(236, "input script line %u too long", line_number);
        char* first = line; while (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n') ++first;
        if (!*first || *first == '#') continue;
        ScriptInput row{};
        if (!ParseInputRow(first, row)) Stop(236, "invalid input script line %u", line_number);
        if (!g_input_script.empty() && row.frame <= g_input_script.back().frame)
            Stop(236, "input script frames must strictly increase (line %u)", line_number);
        if (g_input_script.size() >= 4096) Stop(236, "input script exceeds 4096 rows");
        g_input_script.push_back(row);
    }
    if (ferror(file)) Stop(236, "cannot read input script");
    fclose(file);
    if (g_input_script.empty()) Stop(236, "input script has no rows");
    if (g_input_script.front().frame != 0) g_input_script.insert(g_input_script.begin(), {0, {}});
    g_input_script_enabled = true;
    Log("input: scripted controller port 0, %zu states, %s clock", g_input_script.size(), g_input_script_ms ? "milliseconds from first poll" : "presented-frame");
}

// Physical controllers are packed onto Xbox ports in XInput slot order: the
// first connected pad (wired, wireless adapter or Bluetooth, whatever slot
// Windows gave it) is Xbox port 0, the next port 1, and so on. Empty slots are
// re-polled at most every 500 ms (XInputGetState on an empty slot is slow and
// the title polls for insertions every frame).
bool g_slot_connected[4] = {};
ULONGLONG g_slot_checked[4] = {};
uint32_t PhysicalSlots() {
    uint32_t mask = 0; ULONGLONG now = GetTickCount64();
    for (DWORD i = 0; i < 4 && pXInputGetState; ++i) {
        if (!g_slot_connected[i] && g_slot_checked[i] && now - g_slot_checked[i] < 500) continue;
        WinState probe{};
        bool connected = pXInputGetState(i, &probe) == ERROR_SUCCESS;
        if (connected != g_slot_connected[i]) Log("input: XInput slot %lu %s", i, connected ? "connected" : "disconnected");
        g_slot_connected[i] = connected; g_slot_checked[i] = now;
    }
    for (DWORD i = 0; i < 4; ++i) if (g_slot_connected[i]) mask |= 1u << i;
    return mask;
}
int SlotForPort(DWORD port) {
    uint32_t mask = PhysicalSlots();
    for (DWORD i = 0; i < 4; ++i) if (mask & (1u << i)) { if (port == 0) return static_cast<int>(i); --port; }
    return -1;
}
uint32_t ConnectedGamepads() {
    if (g_input_script_enabled) return 1; // the explicit test device occupies port 0 only
    uint32_t mask = g_keyboard_enabled ? 1 : 0;
    uint32_t slots = PhysicalSlots(), count = 0;
    for (DWORD i = 0; i < 4; ++i) if (slots & (1u << i)) ++count;
    return mask | ((1u << count) - 1);
}
DWORD PhysicalGetState(DWORD port, WinState* state) {
    int slot = SlotForPort(port);
    if (slot < 0) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD result = pXInputGetState(static_cast<DWORD>(slot), state);
    if (result == ERROR_DEVICE_NOT_CONNECTED) { g_slot_connected[slot] = false; g_slot_checked[slot] = GetTickCount64(); }
    return result;
}
DWORD PhysicalSetState(DWORD port, WinVibration* vibration) {
    int slot = SlotForPort(port);
    return slot < 0 ? ERROR_DEVICE_NOT_CONNECTED : pXInputSetState(static_cast<DWORD>(slot), vibration);
}

// Foreground-only keyboard overlay on port zero. Physical controllers remain
// usable; an explicitly selected keyboard also supplies a virtual port zero.
// Inactive windows never collect keys. D-pad and camera use separate keys.
WinGamepad KeyboardPad() {
    WinGamepad p{};
    if (!PresentationHasFocus()) return p;
    auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    p.wButtons=(down(VK_RETURN)?0x10:0)|(down(VK_TAB)?0x20:0)|
        (down(VK_SPACE)?0x1000:0)|(down(VK_BACK)?0x2000:0)|
        (down('E')?0x4000:0)|(down('Q')?0x8000:0)|
        (down('T')?1:0)|(down('G')?2:0)|(down('F')?4:0)|(down('H')?8:0)|
        (down('R')?0x100:0)|(down('V')?0x200:0);
    p.sThumbLX=SHORT((down('D')-down('A'))*32767); p.sThumbLY=SHORT((down('W')-down('S'))*32767);
    p.sThumbRX=SHORT((down(VK_RIGHT)-down(VK_LEFT))*32767); p.sThumbRY=SHORT((down(VK_UP)-down(VK_DOWN))*32767);
    p.bLeftTrigger=down('Z')?255:0; p.bRightTrigger=down('C')?255:0;
    return p;
}
WinState g_keyboard_last{};
DWORD ReadKeyboardGamepad(WinState* state) {
    EnterCriticalSection(&g_input_lock);
    WinState physical{};
    DWORD result=PhysicalGetState(0,&physical);
    if(result!=ERROR_SUCCESS && result!=ERROR_DEVICE_NOT_CONNECTED) { LeaveCriticalSection(&g_input_lock); return result; }
    WinGamepad pad=result==ERROR_SUCCESS ? physical.Gamepad : WinGamepad{};
    WinGamepad keyboard=KeyboardPad(); pad.wButtons|=keyboard.wButtons;
    pad.bLeftTrigger=std::max(pad.bLeftTrigger,keyboard.bLeftTrigger); pad.bRightTrigger=std::max(pad.bRightTrigger,keyboard.bRightTrigger);
    if(keyboard.sThumbLX) pad.sThumbLX=keyboard.sThumbLX;
    if(keyboard.sThumbLY) pad.sThumbLY=keyboard.sThumbLY;
    if(keyboard.sThumbRX) pad.sThumbRX=keyboard.sThumbRX;
    if(keyboard.sThumbRY) pad.sThumbRY=keyboard.sThumbRY;
    if(!g_keyboard_last.dwPacketNumber || memcmp(&g_keyboard_last.Gamepad,&pad,sizeof pad)) ++g_keyboard_last.dwPacketNumber;
    g_keyboard_last.Gamepad=pad; *state=g_keyboard_last;
    LeaveCriticalSection(&g_input_lock); return ERROR_SUCCESS;
}

DWORD ReadGamepad(DWORD port, WinState* state) {
    if (g_keyboard_enabled && port==0) return ReadKeyboardGamepad(state);
    if (!g_input_script_enabled) { EnterCriticalSection(&g_input_lock); DWORD r = PhysicalGetState(port, state); LeaveCriticalSection(&g_input_lock); return r; }
    if (port != 0) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&g_input_lock);
    uint64_t frame = g_presented_frames.load();
    if (g_input_script_ms) {
        if (!g_input_script_started) { g_input_script_origin = GetTickCount64(); g_input_script_started = true; }
        frame = GetTickCount64() - g_input_script_origin;
    }
    while (g_input_script_index + 1 < g_input_script.size() && g_input_script[g_input_script_index + 1].frame <= frame) {
        ++g_input_script_index;
        const auto& row = g_input_script[g_input_script_index];
        Log("input: %s %llu state %zu buttons %04x sticks %d,%d/%d,%d triggers %u,%u", g_input_script_ms ? "ms" : "frame", frame, g_input_script_index,
            row.pad.wButtons, row.pad.sThumbLX, row.pad.sThumbLY, row.pad.sThumbRX, row.pad.sThumbRY,
            row.pad.bLeftTrigger, row.pad.bRightTrigger);
    }
    state->dwPacketNumber = static_cast<DWORD>(g_input_script_index + 1);
    state->Gamepad = g_input_script[g_input_script_index].pad;
    LeaveCriticalSection(&g_input_lock);
    return ERROR_SUCCESS;
}


void __stdcall XppInitDevices(uint32_t count, uint32_t types) {
    InitializeCriticalSection(&g_input_lock);
    LoadInputScript();
    if(g_input_script_enabled && g_keyboard_enabled) Stop(236,"scripted and keyboard input cannot be selected together");
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
    if (!x) Stop(230, "no Windows XInput library");
    pXInputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, "XInputGetState"));
    pXInputSetState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(x, "XInputSetState"));
    if (!pXInputGetState || !pXInputSetState) Stop(231, "XInput entry points missing");
    Log("XInitDevices(%u preallocated types): Windows XInput, gamepads connected %#x", count, ConnectedGamepads());
}

uint32_t __stdcall XppGetDevices(uint32_t type) {
    if (type != kGamepadType) { Log("XGetDevices(type %08x): no such devices", type); return 0; }
    EnterCriticalSection(&g_input_lock);
    g_reported_gamepads = ConnectedGamepads();
    uint32_t m = g_reported_gamepads;
    LeaveCriticalSection(&g_input_lock);
    return m;
}

uint32_t __stdcall XppGetDeviceChanges(uint32_t type, uint32_t* insertions, uint32_t* removals) {
    if (type != kGamepadType) { *insertions = 0; *removals = 0; return 0; }
    EnterCriticalSection(&g_input_lock);
    uint32_t now = ConnectedGamepads();
    *insertions = now & ~g_reported_gamepads; *removals = g_reported_gamepads & ~now;
    g_reported_gamepads = now;
    LeaveCriticalSection(&g_input_lock);
    if (*insertions || *removals) Log("XGetDeviceChanges: inserted %#x removed %#x", *insertions, *removals);
    return (*insertions | *removals) ? 1 : 0;
}

uint32_t __stdcall XppInputOpen(uint32_t type, uint32_t port, uint32_t slot, uint32_t polling) {
    if (type != kGamepadType || port > 3 || slot != 0) Stop(232, "XInputOpen(type %08x, port %u, slot %u) not provided", type, port, slot);
    Log("XInputOpen(gamepad, port %u)", port);
    return 0x7000u + port;   // opaque handle
}

void __stdcall XppInputClose(uint32_t handle) { Log("XInputClose(%#x)", handle); }

uint32_t __stdcall XppInputGetState(uint32_t handle, uint8_t* state) {
    if (handle < 0x7000 || handle > 0x7003) Stop(233, "XInputGetState: bad handle %#x", handle);
    WinState s{};
    if (ReadGamepad(handle - 0x7000, &s) != ERROR_SUCCESS) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    const WinGamepad& g = s.Gamepad;
    uint16_t buttons = g.wButtons & 0xFF;
    uint8_t analog[8] = {
        static_cast<uint8_t>((g.wButtons & 0x1000) ? 255 : 0), static_cast<uint8_t>((g.wButtons & 0x2000) ? 255 : 0),
        static_cast<uint8_t>((g.wButtons & 0x4000) ? 255 : 0), static_cast<uint8_t>((g.wButtons & 0x8000) ? 255 : 0),
        static_cast<uint8_t>((g.wButtons & 0x0200) ? 255 : 0), static_cast<uint8_t>((g.wButtons & 0x0100) ? 255 : 0),
        g.bLeftTrigger, g.bRightTrigger };
    memcpy(state, &s.dwPacketNumber, 4); memcpy(state + 4, &buttons, 2); memcpy(state + 6, analog, 8);
    memcpy(state + 14, &g.sThumbLX, 2); memcpy(state + 16, &g.sThumbLY, 2); memcpy(state + 18, &g.sThumbRX, 2); memcpy(state + 20, &g.sThumbRY, 2);
    return ERROR_SUCCESS;
}

// XINPUT_FEEDBACK: header { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58] } then
// XINPUT_RUMBLE { WORD wLeftMotorSpeed, wRightMotorSpeed } at +66.
uint32_t __stdcall XppInputSetState(uint32_t handle, uint8_t* feedback) {
    if (handle < 0x7000 || handle > 0x7003) Stop(234, "XInputSetState: bad handle %#x", handle);
    uint32_t event; memcpy(&event, feedback + 4, 4);
    if (event) Stop(235, "XInputSetState with completion event not provided");
    WinVibration v{}; memcpy(&v.wLeftMotorSpeed, feedback + 66, 2); memcpy(&v.wRightMotorSpeed, feedback + 68, 2);
    DWORD result;
    if (g_input_script_enabled) {
        // Synthetic output is captured in the evidence log; no physical motor
        // is attached to this explicitly selected test controller.
        result = handle == 0x7000 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
        static DWORD previous[4] = {~0u, ~0u, ~0u, ~0u};
        DWORD motors = v.wLeftMotorSpeed | (DWORD(v.wRightMotorSpeed) << 16);
        EnterCriticalSection(&g_input_lock);
        if (previous[handle - 0x7000] != motors) {
            previous[handle - 0x7000] = motors;
            Log("input: scripted rumble port %u motors %u,%u", handle - 0x7000, v.wLeftMotorSpeed, v.wRightMotorSpeed);
        }
        LeaveCriticalSection(&g_input_lock);
    } else {
        EnterCriticalSection(&g_input_lock); result = PhysicalSetState(handle - 0x7000, &v); LeaveCriticalSection(&g_input_lock);
        if (g_keyboard_enabled && handle==0x7000 && result==ERROR_DEVICE_NOT_CONNECTED) {
            // Explicit keyboard device has no motor. Record its output while
            // still forwarding rumble to a physical port-zero pad when present.
            static DWORD previous=~0u; DWORD motors=v.wLeftMotorSpeed|(DWORD(v.wRightMotorSpeed)<<16);
            if(previous!=motors) { previous=motors; Log("input: keyboard rumble sink motors %u,%u",v.wLeftMotorSpeed,v.wRightMotorSpeed); }
            result=ERROR_SUCCESS;
        }
    }
    if (result != ERROR_SUCCESS) { memcpy(feedback, &result, 4); return result; }
    uint32_t done = ERROR_SUCCESS; memcpy(feedback, &done, 4);   // completed
    return 997;                                                  // ERROR_IO_PENDING (status already complete)
}

// 175 MmLockUnlockBufferPages / 176 MmLockUnlockPhysicalPage: pin pages for
// device DMA. All title memory here is resident; nothing to do.
void __stdcall MmLockUnlockBufferPages(uint32_t, uint32_t, uint32_t) {}
void __stdcall MmLockUnlockPhysicalPage(uint32_t, uint32_t) {}
