// T008 diagnostics only. Full XBE SHA is verified before initialization/mapping.
// Diagnostic experiments require test-run. GH_FRAME_RATE selects the public 30/60 modes.
extern std::atomic<uint64_t> g_presented_frames;
extern std::atomic<unsigned long long> g_vblanks;
bool g_timing_trace = false, g_present_interval_one = false, g_timing_audit = false, g_timing_fields = false;
extern ULONGLONG g_input_script_origin;
bool g_timing_movement_watch=false;
uint32_t g_timing_watch_address=0, g_timing_watch_hits=0;
void LogStack(uint32_t,int);
uint32_t g_timing_consumed_fields=0;
uint32_t g_timing_previous_field=0;
bool g_timing_field_started=false;

bool TimingOption(const wchar_t* name, const wchar_t* expected) {
    wchar_t value[32] = {};
    DWORD n = GetEnvironmentVariableW(name, value, _countof(value));
    if (!n) return false;
    if (n >= _countof(value) || wcscmp(value, expected))
        Stop(301, "T008 option must be its documented value or unset");
    return true;
}
void InitializeTimingResearch() {
    g_timing_movement_watch = TimingOption(L"GH_TIMING_MOVEMENT_WATCH", L"1");
    g_timing_fields = TimingOption(L"GH_TIMING_FIELDS", L"1");
    g_timing_audit = TimingOption(L"GH_TIMING_AUDIT", L"1");
    g_timing_trace = TimingOption(L"GH_TIMING_TRACE", L"1");
    g_present_interval_one = TimingOption(L"GH_PRESENT_INTERVAL", L"1");
    wchar_t mode[8] = {};
    DWORD mode_n = GetEnvironmentVariableW(L"GH_FRAME_RATE", mode, _countof(mode));
    if (mode_n && (mode_n >= _countof(mode) || (wcscmp(mode,L"30") && wcscmp(mode,L"60"))))
        Stop(301,"GH_FRAME_RATE must be 30, 60 or unset");
    bool double_fields = TimingOption(L"GH_VBLANK_HZ", L"119.88");
    if (g_timing_movement_watch && !g_timing_audit) Stop(301,"T008 movement watch requires audit");
    if (g_timing_audit && !g_timing_trace) Stop(301, "T008 audit requires GH_TIMING_TRACE=1");
    if (g_timing_fields || g_timing_trace || g_present_interval_one || double_fields) {
        wchar_t profile[32] = {};
        DWORD n = GetEnvironmentVariableW(L"GH_STORAGE_NAME", profile, _countof(profile));
        if (n >= _countof(profile) || wcscmp(profile, L"test-run"))
            Stop(301, "T008 experiments require GH_STORAGE_NAME=test-run");
        Log("timing research: trace=%u interval_one=%u double_fields=%u audit=%u consume_fields=%u; experimental, not gameplay acceptance",
            unsigned(g_timing_trace), unsigned(g_present_interval_one), unsigned(double_fields), unsigned(g_timing_audit), unsigned(g_timing_fields));
    }
    if (mode_n) {
        if (g_timing_fields || g_timing_trace || g_present_interval_one || double_fields || g_timing_audit || g_timing_movement_watch)
            Stop(301,"GH_FRAME_RATE cannot be combined with timing research overrides");
        g_present_interval_one = g_timing_fields = !wcscmp(mode,L"60");
        Log("frame rate: %ls (%s)", mode, g_timing_fields ? "experimental conserved-field timing" : "original two-field timing");
    }
}

struct TimingAuditSite { uint32_t address; const char* hash; };
const TimingAuditSite kTimingAuditSites[] = {
    {0xfc114, "4f431893f7868168dde14072022653472eb85d8f8c3d578d252e52411d44a2c4"},
    {0xfc3ec, "40dc7b7a4bed2c713c20620adf20c3813be0f6b652a44272efa7fb10dd21d56d"},
};
void PatchTimingAudit(uint32_t base, uint32_t size) {
    if (!g_timing_audit) return;
    for (const auto& site : kTimingAuditSites) {
        if (site.address < base || uint64_t(site.address)+16 > uint64_t(base)+size) continue;
        auto* code = At<uint8_t>(site.address);
        if (Sha256Hex(code,16) != site.hash)
            Stop(301,"T008 audit return site differs: %08x",site.address);
        code[0]=0x0f; code[1]=0x0b; // RET + unreachable INT3 padding -> UD2
        if (!FlushInstructionCache(GetCurrentProcess(),code,2)) Stop(301,"T008 audit cache flush");
        Log("timing audit: patched return %08x hash %s",site.address,site.hash);
    }
}
std::atomic<uint64_t> g_audit_vblank_resyncs{0}, g_audit_vblank_phase_lost{0};
std::atomic<uint64_t> g_audit_callbacks{0}, g_audit_flips{0}, g_audit_fields{0}, g_audit_skips{0};
float g_audit_last_dt=0, g_audit_last_raw=0;
uint32_t g_audit_entry_d4=0, g_audit_entry_d0=0;
uint64_t g_audit_entry_flips=0;

void RequireTimingReadable(uint32_t address, uint32_t size) {
    if(address<0x10000 || uint64_t(address)+size>=0x100000000ULL) Stop(301,"T008 invalid read extent");
    uintptr_t cursor=reinterpret_cast<uintptr_t>(At<uint8_t>(address)),end=cursor+size;
    while(cursor<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !info.RegionSize) Stop(301,"T008 unreadable observation %08x",address);
        uintptr_t next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=cursor) Stop(301,"T008 invalid memory region");
        cursor=next;
    }
}

bool HandleTimingMovementWatch(CONTEXT* c) {
    if(!g_timing_movement_watch || !g_timing_watch_address || c->Dr0!=g_timing_watch_address || !(c->Dr6&1)) return false;
    Log("timing movement watch: hit=%u ms=%llu address=%08x x=%.9f next=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
        ++g_timing_watch_hits,GetTickCount64()-g_input_script_origin,g_timing_watch_address,double(*At<float>(g_timing_watch_address)),c->Eip,c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
    LogStack(c->Esp,12);
    c->Dr6 &= ~1u;
    if(g_timing_watch_hits>=40) { c->Dr0=0; c->Dr7=0; }
    return true;
}

bool HandleTimingAudit(DWORD code, CONTEXT* c) {
    if (!g_timing_audit || code!=EXCEPTION_ILLEGAL_INSTRUCTION ||
        (c->Eip!=0xfc114 && c->Eip!=0xfc3ec)) return false;
    if (*At<uint16_t>(c->Eip)!=0x0b0f) Stop(301,"T008 audit trap bytes changed");
    if (c->Eip==0xfc3ec) {
        ++g_audit_callbacks;
        if (*At<uint32_t>(0x4e5cd8) || *At<uint32_t>(0x4e5cdc)) ++g_audit_skips;
        else {
            uint32_t argument=*At<uint32_t>(c->Esp+4);
            if (*At<uint8_t>(argument+8)&1) {
                g_audit_fields.fetch_add(*At<uint32_t>(0x5096d0));
                ++g_audit_flips;
            }
        }
    } else {
        g_audit_last_dt=*At<float>(0x5096c0); g_audit_last_raw=*At<float>(0x5096b8);
        Log("timing audit: frame tsc=%llu dt=%.9f raw=%.9f d8=%u d4=%u d0=%u entry_d4=%u entry_d0=%u entry_flips=%llu consumed=%u flips=%llu produced=%llu callbacks=%llu skips=%llu resyncs=%llu phase_lost_100ns=%llu",
            ReadXboxTsc(),double(g_audit_last_dt),double(g_audit_last_raw),*At<uint32_t>(0x5096d8),*At<uint32_t>(0x5096d4),*At<uint32_t>(0x5096d0),
            g_audit_entry_d4,g_audit_entry_d0,g_audit_entry_flips,g_timing_consumed_fields,g_audit_flips.load(),g_audit_fields.load(),g_audit_callbacks.load(),g_audit_skips.load(),g_audit_vblank_resyncs.load(),g_audit_vblank_phase_lost.load());
        uint32_t node=*At<uint32_t>(0x511b40);
        if (node) {
            if (node<0x1093c) Stop(301,"T008 invalid player node");
            uint32_t p=node-0x93c;RequireTimingReadable(p,0x940);
            if(g_timing_movement_watch && !g_timing_watch_address && GetTickCount64()-g_input_script_origin>=39900) {
                if(c->Dr7&0xff) Stop(301,"T008 movement watch cannot replace active debug registers");
                g_timing_watch_address=p+8;c->Dr0=p+8;c->Dr6=0;c->Dr7=0x000d0001;
                Log("timing movement watch: armed %08x",p+8);
            }
            Log("timing actor: tsc=%llu ms=%llu actor=%08x xyz=%.6f,%.6f,%.6f attached=%08x timer=%.6f step=%.6f rate=%.6f",
                ReadXboxTsc(),GetTickCount64()-g_input_script_origin,p,double(*At<float>(p+8)),double(*At<float>(p+12)),double(*At<float>(p+16)),*At<uint32_t>(p+0x88),
                double(*At<float>(p+0x930)),double(*At<float>(p+0x924)),double(*At<float>(p+0x92c)));
        }
        uint32_t list=*At<uint32_t>(0x5064e0);
        if (list) {
            RequireTimingReadable(list,16);
            uint32_t begin=*At<uint32_t>(list+4), end=*At<uint32_t>(list+8);
            if (end<begin || end-begin>0x8800 || (end-begin)%0x88) Stop(301,"T008 event list shape");
            if(end>begin) RequireTimingReadable(begin,end-begin);
            for(uint32_t a=begin;a<end;a+=0x88) {
                uint32_t id=*At<uint32_t>(a);
                if(id==4 || id==5) Log("timing event: tsc=%llu ms=%llu id=%u address=%08x remaining=%.9f state=%u",
                    ReadXboxTsc(),GetTickCount64()-g_input_script_origin,id,a,double(*At<float>(a+0x18)),*At<uint32_t>(0x5064d8));
            }
        }
        // Challenge requirements: table 0x2fddc8, update dispatch 0x4ab40.
        // IDs 16/17 update the float at +8 (0x49870 / 0x49790).
        uint32_t challenges=*At<uint32_t>(0x4e17d8);
        if(challenges) {
            RequireTimingReadable(challenges,16);
            uint32_t begin=*At<uint32_t>(challenges+4),end=*At<uint32_t>(challenges+8);
            if(end<begin || end-begin>0x1400 || (end-begin)%0x14) Stop(301,"T008 challenge list shape");
            if(end>begin) RequireTimingReadable(begin,end-begin);
            for(uint32_t a=begin;a<end;a+=0x14) {
                uint32_t id=*At<uint32_t>(a);
                if(id==16 || id==17) Log("timing challenge: tsc=%llu ms=%llu id=%u address=%08x seconds=%.9f state=%u result=%u",
                    ReadXboxTsc(),GetTickCount64()-g_input_script_origin,id,a,double(*At<float>(a+8)),*At<uint32_t>(0x4e17c8),*At<uint32_t>(0x4e17d0));
            }
        }
    }
    // Exact near RET semantics: no arithmetic flags, GPR, XMM or x87 mutation.
    c->Eip=*At<uint32_t>(c->Esp); c->Esp+=4;
    return true;
}

void PatchTimingResearch(uint32_t base, uint32_t size) {
    PatchTimingAudit(base,size);
    constexpr uint32_t delta_site=0xfc0a1;
    if(g_timing_fields && delta_site>=base && uint64_t(delta_site)+16<=uint64_t(base)+size) {
        auto* code=At<uint8_t>(delta_site);
        if(Sha256Hex(code,16)!="36b7f49ec4381d386ea401469023da3c23623c347721e8164b7125f07a4db8e5") Stop(301,"T008 delta read site differs");
        uint32_t source=uint32_t(reinterpret_cast<uintptr_t>(&g_timing_consumed_fields));
        memcpy(code+2,&source,4);memcpy(code+8,&source,4);
        if(!FlushInstructionCache(GetCurrentProcess(),code,16)) Stop(301,"T008 delta read cache flush");
        Log("timing fields: verified reads %08x, consuming eligible fields once",delta_site);
    }
    if (!g_present_interval_one) return;
    constexpr uint32_t site = 0x000fc33c;
    if (site < base || uint64_t(site) + 16 > uint64_t(base) + size) return;
    auto* code = At<uint8_t>(site);
    if (Sha256Hex(code, 16) != "2ca0c01b23ad0979ad2e7077e504106a90a616fb4e2c71ca2ef82f6aa0038304")
        Stop(301, "T008 interval site %08x differs from original", site);
    code[6] = 1; // original MOV byte ptr [video_config+0x18], TWO -> ONE
    if (!FlushInstructionCache(GetCurrentProcess(), code, 16)) Stop(301, "T008 interval cache flush failed");
    Log("timing research: verified interval instruction %08x; immediate %08x TWO -> ONE", site, site + 6);
}

struct TimingCaller { std::atomic<uint32_t> address{0}; std::atomic<uint64_t> calls{0}; };
TimingCaller g_timing_callers[64];
// The XAPI RDTSC wrapper has not adjusted ESP at its trap. This routine only
// observes CONTEXT and optionally updates the private field-consumption cell.
// HandleNativeTscFault remains the sole ABI writer.
void TraceTimingCounter(const CONTEXT* c) {
    if (c->Eip != 0x14c2a0) return;
    if (g_timing_fields && *At<uint32_t>(c->Esp)==0xfc06f) {
        uint32_t fields=*At<volatile uint32_t>(0x5096d8);
        g_timing_consumed_fields=g_timing_field_started ? uint32_t(fields-g_timing_previous_field) : *At<uint32_t>(0x5096d0);
        g_timing_previous_field=fields;g_timing_field_started=true;
    }
    if (!g_timing_trace) return;
    uint32_t caller = *At<uint32_t>(c->Esp);
    bool counted = false;
    for (auto& slot : g_timing_callers) {
        uint32_t address = slot.address.load(std::memory_order_relaxed);
        if (!address) {
            if (slot.address.compare_exchange_strong(address, caller))
                Log("timing research: XAPI caller return=%08x", caller);
            address = slot.address.load(std::memory_order_relaxed);
        }
        if (address == caller) { slot.calls.fetch_add(1, std::memory_order_relaxed); counted = true; break; }
    }
    if (!counted) Stop(301, "T008 caller histogram exhausted");
    if (caller != 0xfc06f) return;
    if (g_timing_audit) {
        if (*At<float>(0x5096c0)!=g_audit_last_dt || *At<float>(0x5096b8)!=g_audit_last_raw)
            Log("timing audit: intervening delta write dt=%.9f previous=%.9f raw=%.9f previous_raw=%.9f",double(*At<float>(0x5096c0)),double(g_audit_last_dt),double(*At<float>(0x5096b8)),double(g_audit_last_raw));
        g_audit_entry_d4=*At<uint32_t>(0x5096d4); g_audit_entry_d0=*At<uint32_t>(0x5096d0); g_audit_entry_flips=g_audit_flips.load();
    }
    static DWORD main_tid = 0;
    if (!main_tid) main_tid = GetCurrentThreadId();
    if (main_tid != GetCurrentThreadId()) Stop(301, "T008 main timing caller changed thread");
    static uint64_t start = 0, last_frames = 0, last_fields = 0, samples = 0;
    static double sum = 0, raw_sum = 0;
    static float lo = 1, hi = 0;
    uint64_t now = ReadXboxTsc();
    if (!start) {
        start = now; last_frames = g_presented_frames.load(); last_fields = g_vblanks.load();
        return; // no preceding main-frame sample on the first invocation
    }
    float dt = *At<float>(0x5096c0), raw = *At<float>(0x5096b8);
    if (!(dt >= 0 && dt <= 0.11f && raw >= 0 && raw < 3600)) Stop(301, "T008 invalid observed delta");
    ++samples; sum += dt; raw_sum += raw; lo = (std::min)(lo, dt); hi = (std::max)(hi, dt);
    if (now - start < kXboxTscFrequency * 2) return;
    uint64_t frames = g_presented_frames.load(), fields = g_vblanks.load();
    unsigned active = 0;
    for (uint32_t a = 0x4dea64; a < 0x4deb54; a += 12) if (*At<uint32_t>(a)) ++active;
    Log("timing research: previous_delta wall=%.6f samples=%llu frames=%llu total_frames=%llu fields=%llu dt_sum=%.6f raw_sum=%.6f min=%.6f max=%.6f interval=%u fields_per_flip=%u frame_callbacks=%u main_counter=%u",
        double(now - start) / kXboxTscFrequency, samples, frames-last_frames, frames, fields-last_fields,
        sum, raw_sum, double(lo), double(hi), *At<uint32_t>(0x181d6c), *At<uint32_t>(0x5096d0), active, *At<uint32_t>(0x4dea58));
    for (auto& slot : g_timing_callers) {
        uint64_t calls = slot.calls.exchange(0, std::memory_order_relaxed);
        if (calls) Log("timing research: caller=%08x calls=%llu", slot.address.load(), calls);
    }
    start = now; last_frames = frames; last_fields = fields; samples = 0;
    sum = raw_sum = 0; lo = 1; hi = 0;
}
