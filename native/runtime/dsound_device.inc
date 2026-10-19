// Title ABI: DirectSoundCreate 0x1c8a36 returns CDirectSound+8;
// IDirectSound_Release 0x1c5aa1 subtracts 8 then calls vtable slot 2.
// Constructor 0x1c5b67 uses a 0x2c-byte object, vtable +0 and count +4.
// Own this HLE device identity with a host vtable, retaining original wrappers.
// The retail count includes hardware-owned references (constructor starts at 2,
// release 0x1c7aa0 handles internal voices at 5). Here XAudio2 owns those host
// resources separately: this count is external client references only, starts
// at 1, and is incremented for every DirectSoundCreate, including Bink's calls.
// Releasing the last client frees its guest identity. Live source/submix voices
// retain their independent host objects; the shared host engine lasts to exit.
uint32_t __stdcall DsDeviceAddRef(uint32_t object);
uint32_t __stdcall DsDeviceRelease(uint32_t object);
[[noreturn]] void __stdcall DsDeviceUnknown0() { Stop(267,"DirectSound device vtable slot 0 not provided"); }
[[noreturn]] void __stdcall DsDeviceUnknown3() { Stop(267,"DirectSound device vtable slot 3 not provided"); }
const uintptr_t kDsDeviceVtable[]={reinterpret_cast<uintptr_t>(&DsDeviceUnknown0),
    reinterpret_cast<uintptr_t>(&DsDeviceAddRef),reinterpret_cast<uintptr_t>(&DsDeviceRelease),
    reinterpret_cast<uintptr_t>(&DsDeviceUnknown3)};
static_assert(sizeof(uintptr_t)==4,"guest DirectSound vtable requires x86");

uint32_t DsAcquireDeviceInterface() {
    EnterCriticalSection(&g_ds_lock);
    if(!g_dsound_object) {
        g_dsound_object=static_cast<uint8_t*>(HeapAlloc(g_pool_heap,HEAP_ZERO_MEMORY,0x2c));
        if(!g_dsound_object || reinterpret_cast<uintptr_t>(g_dsound_object)>=0x80000000u)
            Stop(240,"cannot allocate DirectSound device identity");
        *reinterpret_cast<uint32_t*>(g_dsound_object)=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kDsDeviceVtable));
    }
    auto& refs=*reinterpret_cast<uint32_t*>(g_dsound_object+4);
    if(refs==UINT32_MAX) Stop(267,"DirectSound device reference overflow");
    ++refs;
    uint32_t result=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_dsound_object+8));
    Log("DirectSound device: acquire interface %08x, client references %u",result,refs);
    LeaveCriticalSection(&g_ds_lock);
    return result;
}
uint32_t __stdcall DsDeviceAddRef(uint32_t object) {
    EnterCriticalSection(&g_ds_lock);
    if(!g_dsound_object || object!=reinterpret_cast<uintptr_t>(g_dsound_object))
        Stop(267,"DirectSound device AddRef invalid object %08x",object);
    auto& refs=*reinterpret_cast<uint32_t*>(g_dsound_object+4);
    if(!refs || refs==UINT32_MAX) Stop(267,"DirectSound device AddRef invalid count %u",refs);
    uint32_t result=++refs;
    LeaveCriticalSection(&g_ds_lock);
    return result;
}
uint32_t __stdcall DsDeviceRelease(uint32_t object) {
    EnterCriticalSection(&g_ds_lock);
    if(!g_dsound_object || object!=reinterpret_cast<uintptr_t>(g_dsound_object))
        Stop(267,"DirectSound device Release invalid object %08x",object);
    auto& refs=*reinterpret_cast<uint32_t*>(g_dsound_object+4);
    if(!refs) Stop(267,"DirectSound device Release with no references");
    uint32_t result=--refs;
    Log("DirectSound device: release object %08x, client references %u",object,result);
    if(!result) {
        uint8_t* released=g_dsound_object; g_dsound_object=nullptr;
        if(!HeapFree(g_pool_heap,0,released)) Stop(241,"cannot free DirectSound device identity");
    }
    LeaveCriticalSection(&g_ds_lock);
    return result;
}
