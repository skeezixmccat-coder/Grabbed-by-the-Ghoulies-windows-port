// NV2A GPU register window (included by ghoulies_runtime.cpp).
//
// The original Direct3D library programs the GPU through MMIO at
// 0xFD000000-0xFDFFFFFF. That range is reserved no-access; each access
// faults, the faulting instruction is decoded and executed against the
// register model below, and execution resumes after it.
//
// Register model policy: registers with known hardware behaviour implement
// it; any other register is plain storage (reads return what was written);
// reading a register that was never written and has no known behaviour
// stops the run. Reference for register meanings: XQEMU/xemu-derived
// EmuNV2A_* files in Cxbx-Reloaded (GPL-2.0, read only).

ULONGLONG InterruptTime100ns();
extern HANDLE g_gpu_wake;
extern std::atomic<bool> g_gpu_busy;
constexpr uint32_t kNv2aBase = 0xFD000000u, kNv2aSize = 0x01000000u;

struct Nv2aState {
    // Plain register storage over the 16 MiB window (byte granular), with a
    // per-byte "stored" bit: same semantics as the former hash map, without
    // per-access hashing (the push-buffer loop touches CACHE1 every word).
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x01000000u);
    std::vector<uint8_t> stored = std::vector<uint8_t>(0x01000000u / 8);
    uint32_t pmc_intr = 0, pmc_intr_en = 0;
    uint32_t pcrtc_intr = 0, pcrtc_intr_en = 0;
    uint32_t ptimer_intr = 0, ptimer_intr_en = 0;
    ULONGLONG epoch_100ns = 0;
    uint64_t mmio_accesses = 0;
    uint8_t crtc_index = 0;
    uint8_t crtc[256] = {};   // VGA CRT controller registers (PRMCIO 0x6013D4 index / 0x6013D5 data)
} g_nv2a;
CRITICAL_SECTION g_nv2a_lock;

extern std::atomic<uint32_t> g_scanout_start;
void Nv2aStore(uint32_t offset, uint32_t value, uint32_t size) {
    if (offset >= 0x01000000u || size > 0x01000000u - offset) Stop(293, "NV2A register store outside the window: %#x+%u", offset, size);
    for (uint32_t i = 0; i < size; ++i) {
        g_nv2a.bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        g_nv2a.stored[(offset + i) >> 3] |= static_cast<uint8_t>(1u << ((offset + i) & 7));
    }
    if (offset < 0x600804u && offset + size > 0x600800u) {   // PCRTC_START copy for the flip path
        uint32_t start; memcpy(&start, &g_nv2a.bytes[0x600800], 4); g_scanout_start.store(start, std::memory_order_release);
    }
}

bool Nv2aStored(uint32_t offset, uint32_t size, uint32_t& value) {
    value = 0;
    if (offset >= 0x01000000u || size > 0x01000000u - offset) return false;
    for (uint32_t i = 0; i < size; ++i) {
        if (!(g_nv2a.stored[(offset + i) >> 3] & (1u << ((offset + i) & 7)))) return false;
        value |= static_cast<uint32_t>(g_nv2a.bytes[offset + i]) << (8 * i);
    }
    return true;
}

const char* Nv2aBlock(uint32_t o) {
    if (o < 0x1000) return "PMC"; if (o < 0x2000) return "PBUS"; if (o < 0x4000) return "PFIFO"; if (o >= 0x7000 && o < 0x8000) return "PRMA";
    if (o >= 0x8000 && o < 0x9000) return "PVIDEO"; if (o >= 0x9000 && o < 0xA000) return "PTIMER"; if (o >= 0xA000 && o < 0xB000) return "PCOUNTER";
    if (o >= 0xB000 && o < 0xC000) return "PVPE"; if (o >= 0xD000 && o < 0xE000) return "PTV"; if (o >= 0xA0000 && o < 0xC0000) return "PRMFB";
    if (o >= 0xC0000 && o < 0xD0000) return "PRMVIO"; if (o >= 0x100000 && o < 0x101000) return "PFB"; if (o >= 0x101000 && o < 0x102000) return "PSTRAPS";
    if (o >= 0x400000 && o < 0x402000) return "PGRAPH"; if (o >= 0x600000 && o < 0x601000) return "PCRTC"; if (o >= 0x601000 && o < 0x602000) return "PRMCIO";
    if (o >= 0x680000 && o < 0x681000) return "PRAMDAC"; if (o >= 0x681000 && o < 0x682000) return "PRMDIO"; if (o >= 0x700000 && o < 0x800000) return "PRAMIN";
    if (o >= 0x800000) return "USER";
    return "?";
}

// T008 E1 doubles the diagnostic field rate. Raster, field-pin and AV parity
// use this same period; unset preserves the exact original integer period.
ULONGLONG Nv2aFieldPeriod100ns() {
    static const ULONGLONG period = []() -> ULONGLONG {
        wchar_t value[32] = {};
        DWORD n = GetEnvironmentVariableW(L"GH_VBLANK_HZ", value, _countof(value));
        if (!n) return 10000000ull * 1001 / 60000;
        if (n >= _countof(value) || wcscmp(value, L"119.88")) Stop(301, "GH_VBLANK_HZ must be 119.88 or unset");
        return 10000000ull * 1001 / 120000;
    }();
    return period;
}
// NTSC timing: 525 lines per field at 60000/1001 Hz unless E1 is selected.
bool g_trace_nv2a_writes = false;
uint32_t Nv2aRasterLine() {
    ULONGLONG now = InterruptTime100ns() - g_nv2a.epoch_100ns;
    const ULONGLONG frame = Nv2aFieldPeriod100ns();
    return static_cast<uint32_t>((now % frame) * 525 / frame);
}

// Interrupt lines into PMC_INTR_0: PFIFO bit 8, PGRAPH bit 12, PTIMER bit 20,
// PCRTC bit 24. The GPU interrupt is asserted while any pending line is set
// and PMC_INTR_EN_0 has the hardware enable (bit 0).
uint32_t Nv2aPmcPending() {
    uint32_t p = g_nv2a.pmc_intr & 0x80000000u;   // software interrupt bit
    if (g_nv2a.ptimer_intr & g_nv2a.ptimer_intr_en) p |= 1u << 20;
    if (g_nv2a.pcrtc_intr & g_nv2a.pcrtc_intr_en) p |= 1u << 24;
    if (Nv2aGlPendingInterrupts()) p |= 1u << 12;
    return p;
}
bool Nv2aIrqAsserted() {
    EnterCriticalSection(&g_nv2a_lock);
    bool a = Nv2aPmcPending() && (g_nv2a.pmc_intr_en & 1);
    LeaveCriticalSection(&g_nv2a_lock);
    return a;
}
std::atomic<unsigned long long> g_vblanks{0};
void Nv2aVblank() {
    g_vblanks.fetch_add(1, std::memory_order_relaxed);
    EnterCriticalSection(&g_nv2a_lock);
    g_nv2a.pcrtc_intr |= 1;   // PCRTC_INTR_0_VBLANK
    LeaveCriticalSection(&g_nv2a_lock);
}

// PRAMIN (0x700000-0x7FFFFF): GPU instance memory (RAMHT, RAMFC, objects).
// Declared model: separate 1 MiB storage reached only through this window
// (as in Cxbx-Reloaded); the title addresses instance data by PRAMIN offset.
uint8_t* g_pramin = nullptr;

uint32_t Nv2aRead(uint32_t offset, uint32_t size, uint32_t eip) {
    uint32_t aligned = offset & ~3u, shift = (offset & 3) * 8;
    if (size == 1 && offset == 0x6013D4) return g_nv2a.crtc_index;
    if (size == 1 && offset == 0x6013D5) return g_nv2a.crtc[g_nv2a.crtc_index];
    if (offset >= 0x700000 && offset < 0x800000) {
        uint32_t v = 0; memcpy(&v, g_pramin + (offset - 0x700000), size); return v;
    }
    uint32_t value = 0; bool special = true;
    switch (aligned) {
    case 0x000000: value = 0x02A000A2; break;              // PMC_BOOT_0: NV2A A2
    case 0x000004: value = 0; break;                       // PMC_BOOT_1: little endian
    case 0x000100: value = Nv2aPmcPending(); break;        // PMC_INTR_0
    case 0x000140: value = g_nv2a.pmc_intr_en; break;      // PMC_INTR_EN_0
    case 0x001800: value = 0x02A010DE; break;              // PBUS_PCI_NV_0: NVIDIA, device 0x02A0
    case 0x001808: value = 0x030000A1; break;              // PBUS_PCI_NV_2: display class, revision A1
    case 0x009100: value = g_nv2a.ptimer_intr; break;      // PTIMER_INTR_0
    case 0x009140: value = g_nv2a.ptimer_intr_en; break;   // PTIMER_INTR_EN_0
    case 0x100200: value = 3; break;                       // PFB_CFG0: 4 memory partitions
    case 0x10020C: value = 0x04000000; break;              // PFB_CSTATUS: 64 MiB
    case 0x100410: value = 0; break;                       // PFB_WBC: no flush pending
    case 0x600100: value = g_nv2a.pcrtc_intr; break;       // PCRTC_INTR_0
    case 0x600140: value = g_nv2a.pcrtc_intr_en; break;    // PCRTC_INTR_EN_0
    case 0x600808: value = Nv2aRasterLine(); break;        // PCRTC_RASTER
    case 0x680514: value = 0x00000F00; break;              // PRAMDAC_PLL_TEST_COUNTER: all PLLs locked
    default: special = false;
    }
    if (!special) {
        special = true;
        switch (aligned) {
        // PGRAPH_STATUS reads the register file as the reference does (idle while waiting on the ISR).
        case 0x002400: value = 0x10; break;                                            // PFIFO_RUNOUT_STATUS: low mark (empty)
        case 0x003214: { uint32_t g = 0, p = 0; Nv2aStored(0x3244, 4, g); Nv2aStored(0x3240, 4, p);
                         value = (g == p) ? 0x10 : 0; break; }                          // PFIFO_CACHE1_STATUS: low mark when drained
        default: special = false;
        }
    }
    if (!special && offset >= 0x400000 && offset < 0x402000) {
        if (size != 4) Stop(192, "NV2A PGRAPH access of size %u at EIP %08x", size, eip);
        return Nv2aGlRead(offset - 0x400000);
    }
    if (!special && offset >= 0x800000) {                                              // USER: channel DMA_PUT/GET/REF
        uint32_t chid = (offset - 0x800000) >> 16, reg = offset & 0xFFFF, mode = 0, push1 = 0;
        Nv2aStored(0x2504, 4, mode); Nv2aStored(0x3204, 4, push1);
        if (size != 4 || !(mode & (1u << chid)) || (push1 & 0x1F) != chid || (reg != 0x40 && reg != 0x44 && reg != 0x48))
            Stop(210, "NV2A USER read %#x (channel %u, mode %#x) not provided", offset, chid, mode);
        Nv2aStored(reg == 0x40 ? 0x3240 : reg == 0x44 ? 0x3244 : 0x3248, 4, value);
        return value;
    }
    if (special) return size == 4 ? value : (value >> shift) & (size == 1 ? 0xFFu : 0xFFFFu);
    if (Nv2aStored(offset, size, value)) return value;
    // PGRAPH and PFIFO register files start at their reset value, zero (as both reference models do).
    if (offset >= 0x2000 && offset < 0x4000) return 0;
    // PFB tiling (0x100240-0x1002BF) and Z-compression (0x100300-0x10033F) regions: none configured at boot.
    if ((offset >= 0x100240 && offset < 0x1002C0) || (offset >= 0x100300 && offset < 0x100340)) return 0;
    Stop(180, "NV2A read of unprovided register %s+%#x (offset %#x, %u bytes) at EIP %08x", Nv2aBlock(offset), offset, offset, size, eip);
}

void Nv2aWrite(uint32_t offset, uint32_t value, uint32_t size, uint32_t eip) {
    { static int n = 0; if ((g_trace_nv2a_writes && (offset < 0x700000 || offset >= 0x800000)) && n < 400) { ++n; Log("nv2a write %06x = %08x (%u) eip %08x", offset, value, size, eip); } }
    uint32_t aligned = offset & ~3u;
    if (size == 1 && offset == 0x6013D4) { g_nv2a.crtc_index = static_cast<uint8_t>(value); return; }
    if (size == 1 && offset == 0x6013D5) { g_nv2a.crtc[g_nv2a.crtc_index] = static_cast<uint8_t>(value); return; }
    if ((offset & ~1u) == 0x6013D4) Stop(186, "NV2A VGA CRTC access of size %u at EIP %08x", size, eip);
    if (offset >= 0x700000 && offset < 0x800000) {
        memcpy(g_pramin + (offset - 0x700000), &value, size); return;
    }
    if (size != 4 && (aligned == 0x000100 || aligned == 0x000140 || aligned == 0x600100 || aligned == 0x600140 || aligned == 0x009100 || aligned == 0x009140))
        Stop(181, "NV2A sub-dword write to interrupt register %#x at EIP %08x", offset, eip);
    switch (aligned) {
    case 0x000100: g_nv2a.pmc_intr &= ~value; return;          // write 1 to clear
    case 0x000140: g_nv2a.pmc_intr_en = value; return;
    case 0x009100: g_nv2a.ptimer_intr &= ~value; return;
    case 0x009140: g_nv2a.ptimer_intr_en = value; return;
    case 0x600100: g_nv2a.pcrtc_intr &= ~value; return;
    case 0x600140: g_nv2a.pcrtc_intr_en = value; return;
    case 0x000000: case 0x001800: case 0x001808: case 0x100200: case 0x10020C: case 0x600808: case 0x680514:
        Stop(182, "NV2A write to read-only register %#x (%#x) at EIP %08x", offset, value, eip);
    default: break;
    }
    if (offset >= 0x400000 && offset < 0x402000) {
        if (size != 4) Stop(193, "NV2A PGRAPH write of size %u at EIP %08x", size, eip);
        Nv2aGlWrite(offset - 0x400000, value);
        return;
    }
    if (offset >= 0x800000) {
        uint32_t chid = (offset - 0x800000) >> 16, reg = offset & 0xFFFF, mode = 0, push1 = 0;
        Nv2aStored(0x2504, 4, mode); Nv2aStored(0x3204, 4, push1);
        if (size != 4 || !(mode & (1u << chid)) || (push1 & 0x1F) != chid || (reg != 0x40 && reg != 0x44 && reg != 0x48))
            Stop(211, "NV2A USER write %#x = %#x (channel %u, mode %#x) not provided", offset, value, chid, mode);
        Nv2aStore(reg == 0x40 ? 0x3240 : reg == 0x44 ? 0x3244 : 0x3248, value, 4);
        if (reg == 0x40) SetEvent(g_gpu_wake);
        return;
    }
    Nv2aStore(offset, value, size);
}

// ---- Instruction emulation for MMIO operands ----
uint32_t& Reg32(CONTEXT* c, uint32_t r) {
    switch (r & 7) { case 0: return reinterpret_cast<uint32_t&>(c->Eax); case 1: return reinterpret_cast<uint32_t&>(c->Ecx);
    case 2: return reinterpret_cast<uint32_t&>(c->Edx); case 3: return reinterpret_cast<uint32_t&>(c->Ebx);
    case 4: return reinterpret_cast<uint32_t&>(c->Esp); case 5: return reinterpret_cast<uint32_t&>(c->Ebp);
    case 6: return reinterpret_cast<uint32_t&>(c->Esi); default: return reinterpret_cast<uint32_t&>(c->Edi); }
}
uint32_t GetReg(CONTEXT* c, uint32_t r, uint32_t size) {
    if (size == 1) return r < 4 ? (Reg32(c, r) & 0xFF) : ((Reg32(c, r - 4) >> 8) & 0xFF);
    return size == 2 ? (Reg32(c, r) & 0xFFFF) : Reg32(c, r);
}
void SetReg(CONTEXT* c, uint32_t r, uint32_t size, uint32_t v) {
    if (size == 4) { Reg32(c, r) = v; return; }
    if (size == 2) { Reg32(c, r) = (Reg32(c, r) & 0xFFFF0000u) | (v & 0xFFFF); return; }
    if (r < 4) Reg32(c, r) = (Reg32(c, r) & 0xFFFFFF00u) | (v & 0xFF);
    else Reg32(c, r - 4) = (Reg32(c, r - 4) & 0xFFFF00FFu) | ((v & 0xFF) << 8);
}

void SetLogicFlags(CONTEXT* c, uint32_t result, uint32_t size) {
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu, sign = size == 4 ? 0x80000000u : size == 2 ? 0x8000u : 0x80u;
    result &= mask;
    uint32_t f = c->EFlags & ~(0x1u | 0x4u | 0x40u | 0x80u | 0x800u);   // CF PF ZF SF OF
    if (!result) f |= 0x40; if (result & sign) f |= 0x80;
    uint8_t low = static_cast<uint8_t>(result); low ^= low >> 4; low ^= low >> 2; low ^= low >> 1; if (!(low & 1)) f |= 0x4;
    c->EFlags = f;
}

void SetSubFlags(CONTEXT* c, uint32_t a, uint32_t b, uint32_t size) {
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu, sign = size == 4 ? 0x80000000u : size == 2 ? 0x8000u : 0x80u;
    a &= mask; b &= mask; uint32_t r = (a - b) & mask;
    SetLogicFlags(c, r, size);
    if (a < b) c->EFlags |= 0x1;
    if (((a ^ b) & (a ^ r)) & sign) c->EFlags |= 0x800;
    if (((a ^ b ^ r) & 0x10)) c->EFlags |= 0x10; else c->EFlags &= ~0x10u;
}

// MCPX APU register window (0xFE800000-0xFE87FFFF). Only the global sample
// counter (XGSCNT, +0x200C, 48 kHz), which XACT reads as a clock, is provided.
constexpr uint32_t kApuBase = 0xFE800000u, kApuSize = 0x80000u;
uint32_t ApuRead(uint32_t offset, uint32_t size, uint32_t eip) {
    if (offset == 0x200C && size == 4) return static_cast<uint32_t>((InterruptTime100ns() - g_nv2a.epoch_100ns) * 48000 / 10000000);
    Stop(190, "APU read of unprovided register +%#x (%u bytes) at EIP %08x", offset, size, eip);
}
void ApuWrite(uint32_t offset, uint32_t value, uint32_t size, uint32_t eip) {
    Stop(191, "APU write of unprovided register +%#x = %#x (%u bytes) at EIP %08x", offset, value, size, eip);
}

uint32_t DeviceRead(uint32_t address, uint32_t size, uint32_t eip) {
    if (address >= kApuBase && address < kApuBase + kApuSize) return ApuRead(address - kApuBase, size, eip);
    return Nv2aRead(address - kNv2aBase, size, eip);
}
void DeviceWrite(uint32_t address, uint32_t value, uint32_t size, uint32_t eip) {
    if (address >= kApuBase && address < kApuBase + kApuSize) { ApuWrite(address - kApuBase, value, size, eip); return; }
    Nv2aWrite(address - kNv2aBase, value, size, eip);
}

// Returns true if the access was emulated.
bool EmulateMmio(CONTEXT* c, uint32_t fault_address) {
    uint8_t* ip = At<uint8_t>(c->Eip);
    uint32_t len = 0; bool op16 = false;
    if (ip[0] == 0x66) { op16 = true; len = 1; }
    uint8_t op = ip[len++];
    bool two_byte = false; uint8_t op2 = 0;
    if (op == 0x0F) { two_byte = true; op2 = ip[len++]; }
    uint32_t offset = fault_address - kNv2aBase;
    uint32_t wsize = op16 ? 2 : 4;
    auto modrm_length = [&](uint32_t at) -> uint32_t {   // bytes of ModRM+SIB+disp starting at ip[at]
        uint8_t m = ip[at]; uint32_t mod = m >> 6, rm = m & 7, n = 1;
        if (mod == 3) return 0;
        if (rm == 4) { uint8_t sib = ip[at + 1]; ++n; if (mod == 0 && (sib & 7) == 5) n += 4; }
        else if (mod == 0 && rm == 5) n += 4;
        if (mod == 1) n += 1; else if (mod == 2) n += 4;
        return n;
    };
    EnterCriticalSection(&g_nv2a_lock);
    ++g_nv2a.mmio_accesses;
    bool done = true;
    if (!two_byte && (op == 0xA1 || op == 0xA0)) {                  // mov eax/al, [moffs]
        uint32_t s = op == 0xA0 ? 1 : wsize; SetReg(c, 0, s, DeviceRead(fault_address, s, c->Eip)); len += 4;
    } else if (!two_byte && (op == 0xA3 || op == 0xA2)) {           // mov [moffs], eax/al
        uint32_t s = op == 0xA2 ? 1 : wsize; DeviceWrite(fault_address, GetReg(c, 0, s), s, c->Eip); len += 4;
    } else if (!two_byte && (op == 0x8B || op == 0x8A)) {           // mov r, [m]
        uint32_t s = op == 0x8A ? 1 : wsize, n = modrm_length(len); if (!n) done = false;
        else { SetReg(c, (ip[len] >> 3) & 7, s, DeviceRead(fault_address, s, c->Eip)); len += n; }
    } else if (!two_byte && (op == 0x89 || op == 0x88)) {           // mov [m], r
        uint32_t s = op == 0x88 ? 1 : wsize, n = modrm_length(len); if (!n) done = false;
        else { DeviceWrite(fault_address, GetReg(c, (ip[len] >> 3) & 7, s), s, c->Eip); len += n; }
    } else if (!two_byte && (op == 0xC7 || op == 0xC6) && ((ip[len] >> 3) & 7) == 0) {   // mov [m], imm
        uint32_t s = op == 0xC6 ? 1 : wsize, n = modrm_length(len); if (!n) done = false;
        else { uint32_t imm = 0; memcpy(&imm, ip + len + n, s); DeviceWrite(fault_address, imm, s, c->Eip); len += n + s; }
    } else if (two_byte && (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF)) {  // movzx/movsx r, [m]
        uint32_t s = (op2 & 1) ? 2 : 1, n = modrm_length(len); if (!n) done = false;
        else {
            uint32_t v = DeviceRead(fault_address, s, c->Eip);
            if (op2 >= 0xBE) v = s == 1 ? static_cast<uint32_t>(static_cast<int8_t>(v)) : static_cast<uint32_t>(static_cast<int16_t>(v));
            SetReg(c, (ip[len] >> 3) & 7, op16 ? 2 : 4, v); len += n;
        }
    } else if (!two_byte && (op == 0x81 || op == 0x83 || op == 0x80)) {   // group 1 with immediate: add/or/adc/sbb/and/sub/xor/cmp
        uint32_t s = op == 0x80 ? 1 : wsize, n = modrm_length(len), sub = (ip[len] >> 3) & 7; if (!n) done = false;
        else {
            uint32_t isz = op == 0x81 ? s : 1; int32_t imm = 0;
            if (isz == 1) imm = static_cast<int8_t>(ip[len + n]); else if (isz == 2) imm = static_cast<int16_t>(ip[len + n] | (ip[len + n + 1] << 8)); else memcpy(&imm, ip + len + n, 4);
            uint32_t a = DeviceRead(fault_address, s, c->Eip), b = static_cast<uint32_t>(imm), r = 0; bool write = true;
            switch (sub) {
            case 1: r = a | b; SetLogicFlags(c, r, s); break;
            case 4: r = a & b; SetLogicFlags(c, r, s); break;
            case 6: r = a ^ b; SetLogicFlags(c, r, s); break;
            case 7: SetSubFlags(c, a, b, s); write = false; break;
            default: done = false; write = false;
            }
            if (done && write) DeviceWrite(fault_address, r, s, c->Eip);
            if (done) len += n + isz;
        }
    } else if (!two_byte && (op == 0x09 || op == 0x21 || op == 0x31 || op == 0x0B || op == 0x23 || op == 0x85 || op == 0x3B || op == 0x39)) {
        uint32_t s = wsize, n = modrm_length(len), reg = (ip[len] >> 3) & 7; if (!n) done = false;
        else {
            uint32_t a = DeviceRead(fault_address, s, c->Eip), b = GetReg(c, reg, s);
            switch (op) {
            case 0x09: { uint32_t r = a | b; SetLogicFlags(c, r, s); DeviceWrite(fault_address, r, s, c->Eip); break; }   // or [m], r
            case 0x21: { uint32_t r = a & b; SetLogicFlags(c, r, s); DeviceWrite(fault_address, r, s, c->Eip); break; }   // and [m], r
            case 0x31: { uint32_t r = a ^ b; SetLogicFlags(c, r, s); DeviceWrite(fault_address, r, s, c->Eip); break; }   // xor [m], r
            case 0x0B: { uint32_t r = a | b; SetLogicFlags(c, r, s); SetReg(c, reg, s, r); break; }              // or r, [m]
            case 0x23: { uint32_t r = a & b; SetLogicFlags(c, r, s); SetReg(c, reg, s, r); break; }              // and r, [m]
            case 0x85: SetLogicFlags(c, a & b, s); break;                                                       // test [m], r
            case 0x3B: SetSubFlags(c, b, a, s); break;                                                          // cmp r, [m]
            case 0x39: SetSubFlags(c, a, b, s); break;                                                          // cmp [m], r
            }
            len += n;
        }
    } else if (!two_byte && (op == 0xF7 || op == 0xF6) && ((ip[len] >> 3) & 7) == 0) {   // test [m], imm
        uint32_t s = op == 0xF6 ? 1 : wsize, n = modrm_length(len); if (!n) done = false;
        else { uint32_t imm = 0; memcpy(&imm, ip + len + n, s); SetLogicFlags(c, DeviceRead(fault_address, s, c->Eip) & imm, s); len += n + s; }
    } else {
        done = false;
    }
    LeaveCriticalSection(&g_nv2a_lock);
    if (!done) return false;
    c->Eip += len;
    return true;
}

// ---- I/O ports ----
// 0x80C0: MCPX GPIO; bit 5 reads the TV encoder's field pin, which alternates
// every NTSC field. Writes are stored. Every other port stops the run.
uint8_t g_gpio_80c0 = 0;

uint32_t IoRead(uint16_t port, uint32_t size, uint32_t eip) {
    if (port == 0x80C0 && size == 1) {
        const ULONGLONG field = Nv2aFieldPeriod100ns();
        uint32_t odd = static_cast<uint32_t>(((InterruptTime100ns() - g_nv2a.epoch_100ns) / field) & 1);
        return (g_gpio_80c0 & ~0x20u) | (odd << 5);
    }
    Stop(187, "I/O read of port %#x (%u bytes) at EIP %08x not provided", port, size, eip);
}

void IoWrite(uint16_t port, uint32_t value, uint32_t size, uint32_t eip) {
    if (port == 0x80C0 && size == 1) { g_gpio_80c0 = static_cast<uint8_t>(value); return; }
    Stop(188, "I/O write of port %#x = %#x (%u bytes) at EIP %08x not provided", port, value, size, eip);
}

// cli/sti: interrupts disabled = holding the dispatch lock (no DPC or
// interrupt routine can run), as on the single-CPU console.
extern CRITICAL_SECTION g_dispatch_lock;
thread_local uint32_t t_interrupts_disabled = 0;

bool EmulatePortIo(CONTEXT* c) {
    uint8_t* ip = At<uint8_t>(c->Eip);
    if (ip[0] == 0x0F && ip[1] == 0x09) { c->Eip += 2; return true; }   // wbinvd: RAM is coherent with the GPU model
    if (ip[0] == 0xFA) {                                                 // cli
        if (!t_interrupts_disabled) EnterCriticalSection(&g_dispatch_lock);
        t_interrupts_disabled = 1; c->EFlags &= ~0x200u; c->Eip += 1; return true;
    }
    if (ip[0] == 0xFB) {                                                 // sti
        if (t_interrupts_disabled) LeaveCriticalSection(&g_dispatch_lock);
        t_interrupts_disabled = 0; c->EFlags |= 0x200u; c->Eip += 1; return true;
    }
    uint32_t len = 0; bool op16 = false;
    if (ip[0] == 0x66) { op16 = true; len = 1; }
    uint8_t op = ip[len++];
    uint32_t wide = op16 ? 2 : 4;
    switch (op) {
    case 0xEE: IoWrite(static_cast<uint16_t>(c->Edx), c->Eax & 0xFF, 1, c->Eip); break;
    case 0xEF: IoWrite(static_cast<uint16_t>(c->Edx), wide == 2 ? (c->Eax & 0xFFFF) : c->Eax, wide, c->Eip); break;
    case 0xE6: IoWrite(ip[len++], c->Eax & 0xFF, 1, c->Eip); break;
    case 0xEC: SetReg(c, 0, 1, IoRead(static_cast<uint16_t>(c->Edx), 1, c->Eip)); break;
    case 0xED: SetReg(c, 0, wide, IoRead(static_cast<uint16_t>(c->Edx), wide, c->Eip)); break;
    case 0xE4: SetReg(c, 0, 1, IoRead(ip[len++], 1, c->Eip)); break;
    default: return false;
    }
    c->Eip += len;
    return true;
}

// 46 HalReadWritePCISpace(Bus, Slot, Register, Buffer, Length, Write).
// Bus 1 device 0 is the NV2A, whose configuration space is mirrored at PBUS
// 0x1800 (same storage). Other devices are not provided.
void __stdcall HalReadWritePCISpace(uint32_t bus, uint32_t slot, uint32_t reg, uint8_t* buffer, uint32_t length, uint32_t write) {
    uint32_t device = slot & 0x1F, function = (slot >> 5) & 7;
    if (bus != 1 || device != 0 || function != 0 || reg + length > 256)
        Stop(189, "HalReadWritePCISpace(bus %u, device %u, function %u, register %#x, %u bytes) not provided", bus, device, function, reg, length);
    EnterCriticalSection(&g_nv2a_lock);
    for (uint32_t i = 0; i < length; ++i) {
        if (write & 0xFF) Nv2aWrite(0x1800 + reg + i, buffer[i], 1, 0);
        else buffer[i] = static_cast<uint8_t>(Nv2aRead(0x1800 + reg + i, 1, 0));
    }
    LeaveCriticalSection(&g_nv2a_lock);
    Log("HalReadWritePCISpace(NV2A, register %#x, %u bytes, %s)", reg, length, (write & 0xFF) ? "write" : "read");
}

// ---- AV (TV encoder / display mode) kernel functions ----
struct DisplayMode { uint32_t mode = 0, format = 0, pitch = 0, framebuffer = 0; bool set = false; } g_display;
// The display mode and PCRTC_START are also read by the GPU thread at flips,
// where it holds the PGRAPH lock. MMIO emulation takes g_nv2a_lock and then
// the PGRAPH lock, so the flip path must not take g_nv2a_lock: it reads these
// copies (g_display under g_display_lock, which is never held while taking
// another lock).
SRWLOCK g_display_lock = SRWLOCK_INIT;
DisplayMode g_display_copy;
std::atomic<uint32_t> g_scanout_start{0};
uint32_t g_av_saved_data = 0;

// 2 AvSendTVEncoderOption(RegisterBase, Option, Param, Result*)
void __stdcall AvSendTVEncoderOption(uint32_t base, uint32_t option, uint32_t param, uint32_t* result) {
    switch (option) {
    case 6:   // AV_QUERY_AV_CAPABILITIES: AV pack (HDTV, from HalBootSMCVideoMode 1) | factory standard/refresh | user video flags
        if (!result) Stop(220, "AvSendTVEncoderOption(6): null result");
        *result = 0x4u | (0x00400100u & 0x00C00F00u) | (0x00010000u & ~0xFFFu);
        Log("AvSendTVEncoderOption(AV_QUERY_AV_CAPABILITIES) -> %#x", *result);
        return;
    case 15: {  // AV_OPTION_GUESS_FIELD: parity of the current NTSC field (the vblank field counter)
        if (!result) Stop(220, "AvSendTVEncoderOption(15): null result");
        ULONGLONG now = InterruptTime100ns() - g_nv2a.epoch_100ns;
        *result = static_cast<uint32_t>((now / Nv2aFieldPeriod100ns()) & 1);
        return;
    }
    case 1: case 2: case 3: case 4: case 9: case 10: case 11: case 12: case 14: case 18: case 19:
        // Options that only shape the analogue TV signal (Macrovision, closed captions, blanking,
        // flicker/luma filters, CGMS, widescreen signalling): recorded; no effect on rendered images.
        Log("AvSendTVEncoderOption(option %u, param %#x): recorded", option, param);
        if (result) *result = 0;
        return;
    default:
        Stop(221, "AvSendTVEncoderOption(option %u, param %#x) not provided", option, param);
    }
}

// 3 AvSetDisplayMode(RegisterBase, Step, Mode, Format, Pitch, FrameBuffer)
int32_t __stdcall AvSetDisplayMode(uint32_t base, uint32_t step, uint32_t mode, uint32_t format, uint32_t pitch, uint32_t framebuffer) {
    EnterCriticalSection(&g_nv2a_lock);
    g_display = {mode, format, pitch, framebuffer, true};
    AcquireSRWLockExclusive(&g_display_lock); g_display_copy = g_display; ReleaseSRWLockExclusive(&g_display_lock);
    Nv2aStore(0x600800, framebuffer, 4);   // PCRTC_START: scan-out address
    LeaveCriticalSection(&g_nv2a_lock);
    Log("AvSetDisplayMode(step %u, mode %#x, format %#x, pitch %u, framebuffer %#x)", step, mode, format, pitch, framebuffer);
    return 0;
}

// 1 AvGetSavedDataAddress / 4 AvSetSavedDataAddress: framebuffer kept across a title
// reboot; none on a cold boot.
uint32_t __stdcall AvGetSavedDataAddress() { return g_av_saved_data; }
void __stdcall AvSetSavedDataAddress(uint32_t address) { g_av_saved_data = address; Log("AvSetSavedDataAddress(%#x)", address); }

void ReserveNv2a() {
    if (VirtualAlloc(At<void>(kNv2aBase), kNv2aSize, MEM_RESERVE, PAGE_NOACCESS) != At<void>(kNv2aBase))
        Stop(183, "cannot reserve the NV2A register window (error %lu)", GetLastError());
    InitializeCriticalSection(&g_nv2a_lock);
    if (VirtualAlloc(At<void>(kApuBase), kApuSize, MEM_RESERVE, PAGE_NOACCESS) != At<void>(kApuBase)) Stop(183, "cannot reserve the APU register window");
    g_pramin = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_pramin) Stop(185, "cannot allocate PRAMIN");
    g_nv2a.epoch_100ns = InterruptTime100ns();
    // Power-on PLL coefficients (XQEMU defaults) and PCI command register.
    Nv2aStore(0x680500, 0x00011C01, 4);   // PRAMDAC_NVPLL_COEFF (core clock)
    Nv2aStore(0x680504, 0x00007702, 4);   // PRAMDAC_MPLL_COEFF (memory clock)
    Nv2aStore(0x680508, 0x0003C20D, 4);   // PRAMDAC_VPLL_COEFF (video clock)
    // PRAMDAC 0x680680 / 0x6806A0: analogue-encoder coefficient and a toggle bit D3D flips
    // when it reprograms the encoder (D3D 0x17cb1d). Declared zero after boot; they shape
    // only the analogue signal, never the rendered image.
    Nv2aStore(0x680680, 0, 4);
    Nv2aStore(0x6806A0, 0, 4);
    Nv2aStore(0x001804, 0x00000001, 4);   // PBUS_PCI_NV_1: I/O space enabled
    // Declared post-boot values for registers the title reads only to save
    // them for shutdown or to read-modify-write (same values as the reference
    // models, which leave them zero).
    Nv2aStore(0x100204, 0, 4);            // PFB_CFG1 (saved for shutdown)
    Nv2aStore(0x001218, 0, 4);            // PBUS 0x1218 (bit 8 selects a memory-mode value)
    Nv2aStore(0x100214, 0, 4);            // PFB 0x100214 (bit 0 cleared by the title)
    Nv2aStore(0x008088, 0, 4);            // PVIDEO 0x8088 (read-modify-write)
    Nv2aStore(0x00808C, 0, 4);            // PVIDEO 0x808C (read-modify-write)
    Nv2aStore(0x600804, 0, 4);            // PCRTC_CONFIG (read-modify-write)
    Nv2aStore(0x00184C, 0, 4);            // NV2A PCI config 0x4C (title ORs 0x1F into its top byte)
    Nv2aStore(0x000200, 0xFFFFFFFF, 4);   // PMC_ENABLE: engines on after kernel boot (title saves it for shutdown, then writes all ones)
    Log("NV2A register window %08x-%08x reserved", kNv2aBase, kNv2aBase + kNv2aSize);
}
