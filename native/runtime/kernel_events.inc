// Guest dispatcher event backing, included by kernel_sync.inc and the
// Windows regression fixture. The guest header is authoritative: retail
// D3D clears notification SignalState inline (0x177502 -> device+0x1DC0),
// without calling a kernel reset export. Refresh the host notification event
// before waiting, or a previous signal permits premature ring-buffer reuse.
std::unordered_map<uint32_t, HANDLE> g_wait_objects;
CRITICAL_SECTION g_wait_lock;

HANDLE WaitObjectFor(uint32_t object, const char* caller) {
    EnterCriticalSection(&g_wait_lock);
    uint8_t type = *At<uint8_t>(object);
    if (type != 0 && type != 1 && type != 8 && type != 9) Stop(192, "%s(%08x): dispatcher object type %u not provided", caller, object, type);
    bool notification = type == 0 || type == 8;
    bool signaled = *At<int32_t>(object + 4) != 0;
    HANDLE& h = g_wait_objects[object];
    if (!h) {
        h = CreateEventW(nullptr, notification ? TRUE : FALSE, signaled ? TRUE : FALSE, nullptr);
        if (!h) Stop(231, "%s: cannot create dispatcher event (%lu)", caller, GetLastError());
    } else if (notification) {
        if (!(signaled ? SetEvent(h) : ResetEvent(h))) Stop(231, "%s: cannot synchronize dispatcher event (%lu)", caller, GetLastError());
    }
    HANDLE r = h;
    LeaveCriticalSection(&g_wait_lock);
    return r;
}

void SignalObject(uint32_t object) {
    // Serialize host/guest signaling with WaitObjectFor's refresh. The lock
    // is recursive; WaitObjectFor also checks the declared dispatcher type.
    EnterCriticalSection(&g_wait_lock);
    *At<int32_t>(object + 4) = 1;
    if (!SetEvent(WaitObjectFor(object, "signal"))) Stop(231, "cannot signal dispatcher event (%lu)", GetLastError());
    LeaveCriticalSection(&g_wait_lock);
}
