// DirectSound replacement on XAudio2 (included by ghoulies_runtime.cpp).
//
// The title's DirectSound C++ layer (CDirectSound*, CDirectSoundBuffer*,
// CDirectSoundStream*, CDirectSoundVoice*) and its hardware layer (CMcpx*)
// are replaced at their entry points (dsound_entries.inc, identified by
// XbSymbolDatabase, first 16 bytes verified). The C wrappers
// (IDirectSound*), XACT and DirectSound's pure helpers stay original code.
// Entry points not yet implemented stop the run by name.
// Semantics reference: Cxbx-Reloaded HLE DirectSound (GPL-2.0, read only).
#include <xaudio2.h>
#include <xapobase.h>
#include <cmath>
#pragma comment(lib, "xapobase.lib")
void SetXboxEventHandle(uint32_t handle);
#pragma comment(lib, "ole32.lib")

struct DsoundEntry { uint32_t address; const char* name; const char* convention; const char* sha256; uint32_t params; };
const DsoundEntry kDsoundEntries[] = {
#include "dsound_entries.inc"
};

// ---- Kernel pool (ExAllocatePoolWithTag 15 / ExFreePool 17 / ExQueryPoolBlockSize 23) ----
HANDLE g_pool_heap = nullptr;
uint32_t __stdcall ExAllocatePoolWithTag(uint32_t bytes, uint32_t tag) {
    void* p = HeapAlloc(g_pool_heap, HEAP_ZERO_MEMORY, bytes ? bytes : 1);
    if (!p || reinterpret_cast<uintptr_t>(p) >= 0x80000000u) Stop(240, "ExAllocatePoolWithTag(%u, %.4s) failed", bytes, reinterpret_cast<const char*>(&tag));
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}
void __stdcall ExFreePool(uint32_t p) { if (!HeapFree(g_pool_heap, 0, At<void>(p))) Stop(241, "ExFreePool(%08x): not a pool block", p); }
uint32_t __stdcall ExQueryPoolBlockSize(uint32_t p, uint32_t* quota) {
    SIZE_T n = HeapSize(g_pool_heap, 0, At<void>(p));
    if (n == static_cast<SIZE_T>(-1)) Stop(242, "ExQueryPoolBlockSize(%08x): not a pool block", p);
    if (quota) *quota = 0;
    return static_cast<uint32_t>(n);
}

// ---- Audio engine ----
IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
uint8_t* g_dsound_object = nullptr;   // CDirectSound identity; its interface is object+8

extern "C" [[noreturn]] void __cdecl ReportUnimplementedDsound(const uint32_t* frame) {
    // frame[0] = ecx (this for thiscall), frame[1] = entry index, frame[2] = return address, frame[3..] = arguments
    const DsoundEntry& e = kDsoundEntries[frame[1]];
    Log("unimplemented DirectSound entry %s (%08x, %s, %u params), called from %08x, thread %lu",
        e.name, e.address, e.convention, e.params, frame[2], GetCurrentThreadId());
    Log("  ecx=%08x args %08x %08x %08x %08x %08x %08x", frame[0], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8]);
    Stop(243, "DirectSound entry %s not provided", e.name);
}

extern "C" __declspec(naked) void UnimplementedDsound() {
    __asm {
        push ecx
        mov eax, esp
        push eax
        call ReportUnimplementedDsound
        int 3
    }
}

// ---- Object model ----
// Every DirectSound object handed to the title is a 0x100-byte pool block
// (opaque to the original code) mapped to a native object.
struct MixBinPair { uint32_t bin; int32_t volume; };
// XMEDIAPACKET fields copied at Process time: callers may pass a temporary
// packet structure (XACT passes one on its stack), so completion must not read
// it back. Layout: pvBuffer +0, dwMaxSize +4, pdwCompletedSize +8,
// pdwStatus +12, hCompletionEvent/pContext +16, prtTimestamp +20.
struct PacketInfo { uint32_t completed_ptr = 0, status_ptr = 0, event = 0, max_size = 0; };
struct DsVoice;
struct Packet { DsVoice* voice=nullptr; bool completed=false; uint64_t trace_hash=0, trace_id=0, trace_start=0; uint32_t packet; uint32_t buffer; uint32_t size; std::vector<int16_t> pcm; PacketInfo info; };

struct DsVoice {
    enum Kind { Buffer, Stream, Submix } kind;
    std::wstring trace_path; uint32_t trace_bytes=0, trace_status=0xffffffffu;
    uint32_t object = 0;                 // title-visible pointer
    uint32_t refs_at = 0;                // the object's CRefCount count (title-visible, see NewDsObject)
    WAVEFORMATEX format{};               // title format (PCM or Xbox ADPCM 0x69)
    IXAudio2SourceVoice* voice = nullptr;
    // MIXIN buffers (DSBCAPS_MIXIN, no format): one 48 kHz input channel mixed
    // from the voices routed to them with SetOutputBuffer, as on the console.
    IXAudio2SubmixVoice* submix = nullptr;
    uint32_t flags = 0;
    DsVoice* output = nullptr;
    bool use_3d_voice_data = false;
    uint64_t stat_plays = 0, stat_packets = 0;   // diagnostics
    // 3D voice data as Set3DVoiceData stores it (original 0x1c5e64): +0 dirty
    // flags, then fields at +4..+0x34 (see Ds3DComponent).
    uint32_t voice3d[14] = {};
    bool has_3d_data = false;           // SetOutputBuffer destination (MIXIN buffer) or null (mix bins)
    int32_t volume = 0, headroom = 0, pitch_override = 0; bool pitch_set = false;
    uint32_t frequency = 0;              // 0: format rate
    std::vector<MixBinPair> mixbins;
    // buffers
    uint32_t data = 0, bytes = 0;        // title memory
    std::vector<uint8_t> owned;          // DirectSound-allocated buffer memory (Lock)
    std::vector<int16_t> decoded;        // ADPCM decoded copy
    uint32_t loop_start = 0, loop_length = 0, play_start = 0, play_length = 0;
    bool playing = false, looping = false, paused = false;
    // MIXIN buffers: Play was called (and not stopped since). The original
    // CMcpxBuffer_GetStatus (0x1cdafc) reports PLAYING only from the voice
    // state Play sets; routing into the buffer is live regardless (playing).
    bool mixin_played = false;
    uint64_t base_samples = 0;           // SamplesPlayed at last submit
    // streams
    uint32_t callback = 0, context = 0, max_packets = 0;
    std::vector<Packet*> queue;          // pending completion, oldest first
    std::vector<Packet*> packets;        // stable callback identities, including flushed packets
    bool stream_started = false, discontinuity = false, pause_noactivate = false;
    uint32_t envelope_release[2] = {};
    bool envelope_complete = false, envelope_releasing = false, envelope_terminal = false;
    uint64_t envelope_started = 0;
    double envelope_duration_ms = 0;
    float envelope_start_gain = 1;
};
std::unordered_map<uint32_t, DsVoice*> g_ds_objects;
CRITICAL_SECTION g_ds_lock;
std::atomic<uint32_t> g_audio_releases{0};
void UpdateAudioEnvelopes();
static void ResetStreamEnvelope(DsVoice* v) {
    EnterCriticalSection(&g_ds_lock);
    if(v->envelope_releasing){v->envelope_releasing=false;--g_audio_releases;}
    v->envelope_complete=false;
    if(v->voice)v->voice->SetVolume(1.0f);
    LeaveCriticalSection(&g_ds_lock);
}
#include "dsound_device.inc"
int32_t g_mixbin_headroom[8] = {};

DsVoice* DsFind(uint32_t object, const char* caller) {
    auto f = g_ds_objects.find(object);
    if (f == g_ds_objects.end()) Stop(248, "%s(%08x): not a DirectSound object of this runtime", caller, object);
    return f->second;
}

DsVoice* DsFindSource(uint32_t object, const char* caller) {
    DsVoice* v = DsFind(object, caller);
    if (v->kind == DsVoice::Submix) Stop(266, "%s on a MIXIN buffer (%08x) not provided", caller, object);
    return v;
}

float DbToAmp(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f); }

// Xbox ADPCM (format tag 0x69): per channel 36-byte blocks of 64 samples (IMA ADPCM, 4-byte header per channel).
void DecodeXboxAdpcm(const uint8_t* src, uint32_t bytes, uint32_t channels, std::vector<int16_t>& out) {
    static const int kStep[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
    static const int kIndex[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
    uint32_t block = 36 * channels, blocks = bytes / block;
    out.assign(static_cast<size_t>(blocks) * 64 * channels, 0);
    for (uint32_t b = 0; b < blocks; ++b) {
        const uint8_t* p = src + b * block;
        int pred[8], index[8];
        for (uint32_t c = 0; c < channels; ++c) {
            pred[c] = static_cast<int16_t>(p[c * 4] | (p[c * 4 + 1] << 8)); index[c] = std::min(88, static_cast<int>(p[c * 4 + 2]));
            out[(static_cast<size_t>(b) * 64) * channels + c] = static_cast<int16_t>(pred[c]);
        }
        const uint8_t* data = p + 4 * channels;
        for (uint32_t chunk = 0; chunk < 8; ++chunk) {          // 8 groups of 4 bytes per channel = 63 samples after the header sample
            for (uint32_t c = 0; c < channels; ++c) {
                const uint8_t* q = data + (chunk * channels + c) * 4;
                for (int i = 0; i < 8; ++i) {
                    int nibble = (q[i / 2] >> ((i & 1) * 4)) & 0xF, step = kStep[index[c]];
                    int diff = step >> 3; if (nibble & 1) diff += step >> 2; if (nibble & 2) diff += step >> 1; if (nibble & 4) diff += step;
                    pred[c] += (nibble & 8) ? -diff : diff; pred[c] = std::max(-32768, std::min(32767, pred[c]));
                    index[c] = std::max(0, std::min(88, index[c] + kIndex[nibble]));
                    uint32_t sample = 1 + chunk * 8 + i;
                    if (sample < 64) out[(static_cast<size_t>(b) * 64 + sample) * channels + c] = static_cast<int16_t>(pred[c]);
                }
            }
        }
    }
}

bool IsAdpcm(const WAVEFORMATEX& f) { return f.wFormatTag == 0x69; }
uint32_t BytesToSamples(const DsVoice* v, uint32_t bytes) {
    if (IsAdpcm(v->format)) return bytes / (36u * v->format.nChannels) * 64;
    return bytes / v->format.nBlockAlign;
}
uint32_t SamplesToBytes(const DsVoice* v, uint64_t samples) {
    if (IsAdpcm(v->format)) return static_cast<uint32_t>(samples / 64 * 36 * v->format.nChannels);
    return static_cast<uint32_t>(samples * v->format.nBlockAlign);
}

// The hardware volume of one mix bin, in 1/100 dB (<= 0), as the original
// CMcpxVoiceClient volume calculation (0x1cbdc1) computes it: bin volume plus
// voice volume, and with 3D voice data in use (Use3DVoiceData) a 3D component
// chosen by bin: base = clamp(d[+4] + d[+8]) for bins 0,1,3,4,5 and >= 11;
// bin 2: d[+0x1c]+d[+0x18]+d[+0xc]+base; bins 6,7: d[+0x1c]+d[+0x14]+d[+0xc]+base;
// bins 8,9: d[+0x1c]+d[+0x10]+base; bin 10: d[+0x20]+base; each clamped to
// [-10000, 0]. The register then quantizes to 12-bit 1/64 dB attenuation.
int32_t Clamp3D(int64_t v) { return static_cast<int32_t>(std::max<int64_t>(-10000, std::min<int64_t>(0, v))); }
int32_t DsBinVolume(const DsVoice* v, uint32_t bin, int32_t bin_volume) {
    int64_t total = static_cast<int64_t>(bin_volume) + (v->volume - v->headroom);
    if (v->use_3d_voice_data && v->has_3d_data) {
        const int32_t* d = reinterpret_cast<const int32_t*>(v->voice3d);
        int32_t base = Clamp3D(static_cast<int64_t>(d[1]) + d[2]);
        int32_t c;
        if (bin == 2) c = Clamp3D(static_cast<int64_t>(d[7]) + d[6] + d[3] + base);
        else if (bin == 6 || bin == 7) c = Clamp3D(static_cast<int64_t>(d[7]) + d[5] + d[3] + base);
        else if (bin == 8 || bin == 9) c = Clamp3D(static_cast<int64_t>(d[7]) + d[4] + base);
        else if (bin == 10) c = Clamp3D(static_cast<int64_t>(d[8]) + base);
        else c = base;
        total += c;
    }
    int64_t att = std::min<int64_t>(0xFFF, std::max<int64_t>(0, -total * 64 / 100));
    return -static_cast<int32_t>((att * 100) >> 6);
}
// Pitch register as the original (0x1cc015): voice pitch plus 3D Doppler
// (d[+0x24]) when 3D data is in use, plus the destination MIXIN buffer's pitch
// and Doppler; clamped to [-32767, 8191]. MIXIN buffers themselves have 0.
int32_t DsPitchRegister(const DsVoice* v);

// GH_AUDIO_TRACE is disabled: every traced development run (4 of 4) froze
// the test PC (process stopped logging, drive and desktop unresponsive,
// forced power-off), while ~35 untraced runs completed. Cause not yet found.
// Refuse explicitly rather than run it.
static bool AudioTraceEnabled() {
    static bool enabled = [] {
        if (getenv("GH_AUDIO_TRACE")) Stop(295, "GH_AUDIO_TRACE is disabled: traced runs froze the host PC (2026-10-16); do not re-enable without review");
        return false;
    }();
    return enabled;
}
static std::atomic<uint64_t> g_audio_packet_serial{0};
static thread_local uint32_t g_audio_xact_call=0;
// T005 observation only. No original cue state or audio semantics are changed.
struct TracedCue { uint32_t index=0, bank=0, notifications=0; };
static std::unordered_map<uint32_t,TracedCue> g_traced_cues;
static uint64_t g_trace_prepares=0,g_trace_plays=0,g_trace_cue_frees=0;
static uint64_t g_trace_voice_creates=0,g_trace_voice_frees=0;
static void TraceAudioLifecycle() {
    if(!AudioTraceEnabled())return;
    EnterCriticalSection(&g_ds_lock);
    static uint64_t last=0;const auto now=GetTickCount64();
    if(now-last<10000){LeaveCriticalSection(&g_ds_lock);return;}last=now;
    uint32_t streams=0,buffers=0,submixes=0,host_queued=0,pending=0,reported_playing=0,reported_playing_empty=0;
    for(const auto& item:g_ds_objects){
        const auto* v=item.second;if(item.first!=v->object)continue;
        if(v->kind==DsVoice::Submix){++submixes;
            Log("audio lifecycle submix: tick %llu obj %08x refs %u playing %d routed_in %u",now,v->object,*At<uint32_t>(v->refs_at),v->playing,
                uint32_t(std::count_if(g_ds_objects.begin(),g_ds_objects.end(),[&](const auto& o){return o.first==o.second->object && o.second->output==v;})));
            continue;}
        if(v->kind==DsVoice::Stream)++streams;else ++buffers;
        XAUDIO2_VOICE_STATE st{};if(v->voice)v->voice->GetState(&st,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        host_queued+=st.BuffersQueued;pending+=uint32_t(v->queue.size());
        bool playing=v->trace_status!=0xffffffffu && (v->trace_status & (v->kind==DsVoice::Stream?0x10000u:1u));
        reported_playing+=playing;reported_playing_empty+=playing && !st.BuffersQueued;
        Log("audio lifecycle voice: tick %llu obj %08x kind %u last_status %#x host_queued %u pending %zu refs %u paused %d hold %d envelope %d",now,v->object,v->kind,v->trace_status,st.BuffersQueued,v->queue.size(),*At<uint32_t>(v->refs_at),v->paused,v->pause_noactivate,v->envelope_complete);
    }
    uint32_t started=0,stopped=0;
    for(const auto& item:g_traced_cues){started+=(item.second.notifications&1)!=0;stopped+=(item.second.notifications&2)!=0;}
    Log("audio lifecycle: tick %llu prepares %llu plays %llu live_cues %zu cue_frees %llu notified_started %u notified_stopped %u streams %u buffers %u submixes %u voice_creates %llu voice_frees %llu host_queued %u pending %u last_reported_playing %u last_reported_playing_empty %u",now,g_trace_prepares,g_trace_plays,g_traced_cues.size(),g_trace_cue_frees,started,stopped,streams,buffers,submixes,g_trace_voice_creates,g_trace_voice_frees,host_queued,pending,reported_playing,reported_playing_empty);
    LeaveCriticalSection(&g_ds_lock);
}
static uint64_t AudioPacketHash(uint32_t addr,uint32_t size) {
    uint64_t h=14695981039346656037ull;
    const uint8_t* p=At<const uint8_t>(addr);
    for(uint32_t i=0;i<size;++i)h=(h^p[i])*1099511628211ull;
    return h;
}
static void TraceAudioRoute(DsVoice* v, const char* reason) {
    if (!AudioTraceEnabled()) return;
    if(v->voice) { float ratio=0; v->voice->GetFrequencyRatio(&ratio); Log("audio rate: obj %08x actual ratio %.6f pitch %d set %d frequency %u",v->object,ratio,v->pitch_override,v->pitch_set,v->frequency); }

    XAUDIO2_VOICE_STATE st{}; if(v->voice) v->voice->GetState(&st);
    Log("audio route %s: obj %08x kind %d tag %#x ch %u rate %u bytes %u volume %d headroom %d active %d paused %d output %08x queued %u samples %llu 3d %d/%d",
        reason,v->object,v->kind,v->format.wFormatTag,v->format.nChannels,v->format.nSamplesPerSec,v->bytes,v->volume,v->headroom,v->playing,v->paused,
        v->output?v->output->object:0,st.BuffersQueued,st.SamplesPlayed,v->use_3d_voice_data,v->has_3d_data);
    for (const auto& b:v->mixbins) Log("  bin %u raw %d total %d amplitude %.8f",b.bin,b.volume,DsBinVolume(v,b.bin,b.volume),DbToAmp(DsBinVolume(v,b.bin,b.volume)));
    Log("  3d words: %d %d %d %d %d %d %d %d %d %d %d %d %d",
        int32_t(v->voice3d[1]),int32_t(v->voice3d[2]),int32_t(v->voice3d[3]),int32_t(v->voice3d[4]),int32_t(v->voice3d[5]),int32_t(v->voice3d[6]),int32_t(v->voice3d[7]),int32_t(v->voice3d[8]),int32_t(v->voice3d[9]),int32_t(v->voice3d[10]),int32_t(v->voice3d[11]),int32_t(v->voice3d[12]),int32_t(v->voice3d[13]));
    IXAudio2Voice* voice=v->kind==DsVoice::Submix?static_cast<IXAudio2Voice*>(v->submix):static_cast<IXAudio2Voice*>(v->voice);
    unsigned channels=v->kind==DsVoice::Submix?1:v->format.nChannels;
    if(voice && channels && channels<=2) { float matrix[4]={};
        float gain=0; voice->GetVolume(&gain); Log("  voice gain: %.8f",gain);
        voice->GetOutputMatrix(v->output?static_cast<IXAudio2Voice*>(v->output->submix):static_cast<IXAudio2Voice*>(g_master),channels,v->output?1:2,matrix);
        Log("  actual matrix: %.8f %.8f %.8f %.8f",matrix[0],matrix[1],matrix[2],matrix[3]);
    }
}

void ApplyVoiceParameters(DsVoice* v) {
    IXAudio2Voice* xv = v->kind == DsVoice::Submix ? static_cast<IXAudio2Voice*>(v->submix) : static_cast<IXAudio2Voice*>(v->voice);
    if (!xv) return;
    static std::unordered_map<uint32_t,ULONGLONG> trace_times;
    bool trace=AudioTraceEnabled() && GetTickCount64()-trace_times[v->object]>=1000;
    if(trace) trace_times[v->object]=GetTickCount64();
    float amp = DbToAmp(v->volume - v->headroom);
    bool silent = v->kind == DsVoice::Submix && !v->playing;   // a stopped MIXIN buffer produces no output
    if (silent) amp = 0.0f;
    uint32_t ch = v->kind == DsVoice::Submix ? 1 : v->format.nChannels;
    if (v->kind != DsVoice::Submix) {
        double rate = 48000.0 * pow(2.0, DsPitchRegister(v) / 4096.0);
        float ratio = std::max(1.0f / 1024, std::min(1024.0f, static_cast<float>(rate / v->format.nSamplesPerSec)));
        v->voice->SetFrequencyRatio(ratio);
        // Diagnostics: unusual playback rates, once per voice object.
        if (ratio > 3.0f || ratio < 0.34f) { static std::set<uint32_t> noted;
            if (noted.size() < 200 && noted.insert(v->object).second)
                Log("audio rate note: obj %08x kind %d fmt %#x %u Hz ratio %.3f pitch register %d (set %d, frequency %u, 3d %d)",
                    v->object, v->kind, v->format.wFormatTag, v->format.nSamplesPerSec, ratio, DsPitchRegister(v), v->pitch_set, v->frequency, v->use_3d_voice_data); }
    }
    { // Diagnostics: each distinct loudness/routing signature once (cheap; no metering).
        float peak = 0; for (const auto& b : v->mixbins) peak = std::max(peak, DbToAmp(DsBinVolume(v, b.bin, b.volume)));
        int bucket = int(std::lround(20.0 * log10(std::max(1e-6f, amp * (v->mixbins.empty() ? 1.0f : peak))) / 3.0));
        uint64_t sig = (uint64_t(v->kind) << 56) ^ (uint64_t(v->format.wFormatTag) << 40) ^ (uint64_t(v->format.nSamplesPerSec) << 16) ^
                       (uint64_t(uint8_t(bucket)) << 8) ^ (uint64_t(v->output != nullptr) << 4) ^ (uint64_t(v->use_3d_voice_data) << 5) ^ uint64_t(v->mixbins.size() & 15);
        static std::set<uint64_t> seen;
        if (!silent && amp > 0 && seen.size() < 300 && seen.insert(sig).second) {
            char bins[160] = ""; int n = 0;
            for (const auto& b : v->mixbins) n += snprintf(bins + n, sizeof bins - n, " %u:%d", b.bin, DsBinVolume(v, b.bin, b.volume));
            Log("audio voice signature: obj %08x kind %d fmt %#x %u Hz %u ch vol %d headroom %d -> %+.0f dB, output %08x, 3d %d, bins%s",
                v->object, v->kind, v->format.wFormatTag, v->format.nSamplesPerSec, ch, v->volume, v->headroom, bucket * 3.0,
                v->output ? v->output->object : 0, v->use_3d_voice_data, bins);
        }
    }
    if (v->output) {
        // Routed into a MIXIN buffer's single input: every channel at the voice volume.
        std::vector<float> matrix(ch, amp);
        if (FAILED(xv->SetOutputMatrix(v->output->submix, ch, 1, matrix.data()))) Stop(268, "audio routing matrix to MIXIN failed");
        if(trace) TraceAudioRoute(v,"parameters");
        return;
    }
    // Mix bins to stereo: 0/4/6/8 left, 1/5/7/9 right, 2 centre (both, -3 dB); LFE and effect sends are not mixed.
    // The bins of one side carry one source split across front/back speakers
    // (3D panning distributes power), so they combine by power, not amplitude:
    // adding amplitudes made positional ambiences (fires) up to ~3 dB too loud.
    std::vector<float> matrix(ch * 2, 0.0f);
    std::vector<MixBinPair> bins = v->mixbins;
    if (bins.empty()) bins = {{0, 0}, {1, 0}};
    for (size_t k = 0; k < bins.size(); ++k) {
        uint32_t src = ch == 1 ? 0 : static_cast<uint32_t>(k % ch);
        float a = silent ? 0.0f : DbToAmp(DsBinVolume(v, bins[k].bin, bins[k].volume));
        uint32_t b = bins[k].bin;
        if (b == 0 || b == 4 || b == 6 || b == 8) matrix[0 * ch + src] += a * a;
        else if (b == 1 || b == 5 || b == 7 || b == 9) matrix[1 * ch + src] += a * a;
        else if (b == 2) { matrix[0 * ch + src] += 0.5f * a * a; matrix[1 * ch + src] += 0.5f * a * a; }
    }
    for (float& m : matrix) m = std::sqrt(m);
    XAUDIO2_VOICE_DETAILS md{}; g_master->GetVoiceDetails(&md);
    if (md.InputChannels == 2) { if (FAILED(xv->SetOutputMatrix(g_master, ch, 2, matrix.data()))) Stop(268, "audio routing matrix to master failed"); }
    else xv->SetVolume(amp);
    if(trace) TraceAudioRoute(v,"parameters");
}

static void DestroySourceVoice(DsVoice* v);
void CreateSourceVoice(DsVoice* v, IXAudio2VoiceCallback* callback) {
    v->trace_path.clear();v->trace_bytes=0;
    DestroySourceVoice(v);
    WAVEFORMATEX f = v->format;
    if (IsAdpcm(f)) { f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 16; f.nBlockAlign = static_cast<WORD>(2 * f.nChannels); f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign; f.cbSize = 0; }
    else if (f.wFormatTag != WAVE_FORMAT_PCM) Stop(249, "DirectSound format tag %#x not provided", f.wFormatTag);
    else f.cbSize = 0;
    XAUDIO2_SEND_DESCRIPTOR send{0, v->output ? static_cast<IXAudio2Voice*>(v->output->submix) : static_cast<IXAudio2Voice*>(g_master)};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    HRESULT hr = g_xaudio->CreateSourceVoice(&v->voice, &f, 0, XAUDIO2_MAX_FREQ_RATIO, callback, &sends);
    if (FAILED(hr)) Stop(250, "XAudio2 source voice failed (%#lx; %u ch, %u Hz, %u bit)", static_cast<unsigned long>(hr), f.nChannels, f.nSamplesPerSec, f.wBitsPerSample);
    ApplyVoiceParameters(v);
}

void ReadMixBins(DsVoice* v, uint32_t mixbins) {
    v->mixbins.clear();
    if (!mixbins) return;
    uint32_t count = *At<uint32_t>(mixbins), pairs = *At<uint32_t>(mixbins + 4);
    for (uint32_t i = 0; i < count && i < 32; ++i) v->mixbins.push_back({*At<uint32_t>(pairs + i * 8), *At<int32_t>(pairs + i * 8 + 4)});
}

// Object layout follows the original classes, because the title's inline
// interface code uses it (DSOUND, XBE 5838 build of this title):
//  - CDirectSoundBuffer (0x24 bytes, ctor 0x1c77c6): vtable 0x20a018 at +0,
//    CRefCount count at +4; callers hold the IDirectSoundBuffer pointer
//    object+0x1c (CreateSoundBuffer 0x1c8646), which the IDirectSoundBuffer_*
//    wrappers convert back (e.g. 0x1c7a48). Its vtable AddRef (0x1c60ba)
//    increments +4 inline; Release reaches DSound_CRefCount_Release (ours).
//  - CDirectSoundStream (0x28 bytes, ctor 0x1c7dba): XMediaObject vtable
//    0x20a038 at +0 (entries are the replaced CDirectSoundStream_* entry
//    points), CDirectSoundVoice base at +4 (vtable 0x20a028), count at +8.
// The vtables are the title's own, so virtual calls reach the replaced entry
// points or the original inline helpers that operate on these fields.
uint32_t NewDsObject(DsVoice* v) {
    uint8_t* block = static_cast<uint8_t*>(HeapAlloc(g_pool_heap, HEAP_ZERO_MEMORY, 0x100));
    v->object = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
    if (v->kind == DsVoice::Stream) {
        *reinterpret_cast<uint32_t*>(block) = 0x20a038u;
        *reinterpret_cast<uint32_t*>(block + 4) = 0x20a028u;
        v->refs_at = v->object + 8;
        g_ds_objects[v->object + 4] = v;   // CDirectSoundVoice/CRefCount subobject
    } else {
        *reinterpret_cast<uint32_t*>(block) = 0x20a018u;
        v->refs_at = v->object + 4;
    }
    *At<uint32_t>(v->refs_at) = 1;
    g_ds_objects[v->object] = v;
    if(AudioTraceEnabled()){++g_trace_voice_creates;Log("audio voice create: tick %llu obj %08x kind %u",GetTickCount64(),v->object,v->kind);}
    return v->object;
}
// The pointer handed back to the title from CreateSoundBuffer/CreateSoundStream.
uint32_t DsInterface(const DsVoice* v) { return v->kind == DsVoice::Stream ? v->object : v->object + 0x1c; }

// ---- Streams: packet completion on the DPC thread ----
struct StreamCallback : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override;
    void STDMETHODCALLTYPE OnBufferEnd(void*) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
} g_stream_callback;

// Completion work owns snapshots, not a possibly released DsVoice pointer.
struct CompletedPacket { uint32_t callback,context; PacketInfo info; };
std::vector<CompletedPacket> g_completed_packets;
// Callbacks only collect timestamps in memory. File output stays on the DPC
// thread; no diagnostic effect is inserted into any voice.
struct PacketTraceEnd { uint64_t id,start,end; uint32_t object,buffer,size; bool cancelled; };
std::vector<PacketTraceEnd> g_packet_trace_ends;
void QueueDsoundWork();
// Only signal the existing DPC worker from the audio callback. No object
// access, allocation, logging, guest callbacks, or new voice effects here.
struct AudioEngineCallback : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {if(g_audio_releases.load())QueueDsoundWork();}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) override {Stop(268,"XAudio2 engine error %#lx",static_cast<unsigned long>(hr));}
} g_audio_engine_callback;

void CompletePacketValues(uint32_t callback,uint32_t context,const PacketInfo& p,uint32_t status,uint32_t completed) {
    if(AudioTraceEnabled())Log("audio packet completion: tick %llu completed_ptr %08x status_ptr %08x status %#x bytes %u callback %08x context %08x event %08x",GetTickCount64(),p.completed_ptr,p.status_ptr,status,completed,callback,context,p.event);
    if (p.completed_ptr) *At<uint32_t>(p.completed_ptr) = completed;
    if (p.status_ptr) *At<uint32_t>(p.status_ptr) = status;
    if (callback) reinterpret_cast<void (__stdcall*)(uint32_t,uint32_t,uint32_t)>(uintptr_t(callback))(context,p.event,status);
    else if (p.event) SetXboxEventHandle(p.event);
}
void CompletePacket(DsVoice* v,const PacketInfo& p,uint32_t status,uint32_t completed) {
    CompletePacketValues(v->callback,v->context,p,status,completed);
}

void STDMETHODCALLTYPE StreamCallback::OnBufferStart(void* context) {
    auto* p=static_cast<Packet*>(context);
    if(!p->trace_id)return;
    EnterCriticalSection(&g_ds_lock);p->trace_start=GetTickCount64();LeaveCriticalSection(&g_ds_lock);
}

void STDMETHODCALLTYPE StreamCallback::OnBufferEnd(void* context) {
    auto* p=static_cast<Packet*>(context);DsVoice* v=p->voice;
    EnterCriticalSection(&g_ds_lock);
    bool notify=!p->completed;
    const bool traced=p->trace_id!=0;
    if(traced)g_packet_trace_ends.push_back({p->trace_id,p->trace_start,GetTickCount64(),v->object,p->buffer,p->size,p->completed});
    if(notify) {
        if(p->trace_hash && p->trace_hash!=AudioPacketHash(p->buffer,p->size))
            Log("audio packet changed before completion: obj %08x buffer %08x size %u",v->object,p->buffer,p->size);
        auto it=std::find(v->queue.begin(),v->queue.end(),p);
        if(it==v->queue.end())Stop(268,"stream completion has no pending packet identity");
        v->queue.erase(it);p->completed=true;
        g_completed_packets.push_back({v->callback,v->context,p->info});
    }
    auto it=std::find(v->packets.begin(),v->packets.end(),p);
    if(it==v->packets.end())Stop(268,"stream callback identity was retired twice");
    v->packets.erase(it);
    LeaveCriticalSection(&g_ds_lock);
    delete p; // XAudio2 has now finished reading this packet's PCM.
    if(notify || traced)QueueDsoundWork();
}

static void DestroySourceVoice(DsVoice* v) {
    if(!v->voice)return;
    ResetStreamEnvelope(v);
    std::vector<PacketInfo> cancelled;
    EnterCriticalSection(&g_ds_lock);
    for(auto* p:v->queue){
        if(p->trace_id)Log("audio packet cancel: id %llu tick %llu obj %08x start %llu reason destroy",p->trace_id,GetTickCount64(),v->object,p->trace_start);
        p->completed=true;cancelled.push_back(p->info);
    }
    v->queue.clear();
    LeaveCriticalSection(&g_ds_lock);
    v->voice->DestroyVoice();v->voice=nullptr; // waits for in-flight callbacks; no later callbacks
    for(auto* p:v->packets)delete p;
    v->packets.clear();
    const uint32_t callback=v->callback,context=v->context;
    for(const auto& info:cancelled)CompletePacketValues(callback,context,info,0x80004004,0);
}

// Runs on the DPC thread (dispatch level, dispatch lock held).
uint64_t g_completed_total = 0;
void DsoundWork() {
    UpdateAudioEnvelopes();
    TraceAudioLifecycle();
    std::vector<CompletedPacket> done;
    std::vector<PacketTraceEnd> trace;
    EnterCriticalSection(&g_ds_lock);done.swap(g_completed_packets);trace.swap(g_packet_trace_ends);LeaveCriticalSection(&g_ds_lock);
    for(const auto& t:trace)Log("audio packet end: id %llu tick %llu obj %08x buffer %08x size %u start %llu cancelled %d",t.id,t.end,t.object,t.buffer,t.size,t.start,t.cancelled);
    for(const auto& d:done){++g_completed_total;CompletePacketValues(d.callback,d.context,d.info,0,d.info.max_size);}
}

// ---- Evidence capture of the mixed output ----
// A pass-through effect on the mastering voice copies the final stereo float
// mix into a buffer; a writer thread appends it to <evidence>/audio-mix.wav and
// keeps the RIFF sizes current, so the file is valid even when a STOP ends the
// process. Capped at 120 s. Private evidence for verification; it changes
// nothing that is played.
CRITICAL_SECTION g_capture_lock;
std::vector<float> g_capture_pending;
uint64_t g_capture_frames = 0;
bool g_capture_recording = true;
constexpr uint64_t kCaptureMaxFrames = 120ull * 48000;

class __declspec(uuid("7b1d1f3e-2c4a-4f61-9a55-3c0e8d6b4a21")) CaptureXapo : public CXAPOBase {
    static XAPO_REGISTRATION_PROPERTIES s_props;
    UINT32 channels_ = 2;
public:
    CaptureXapo() : CXAPOBase(&s_props) {}
    STDMETHOD(LockForProcess)(UINT32 in_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* in, UINT32 out_count, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* out) override {
        channels_ = in[0].pFormat->nChannels;
        return CXAPOBase::LockForProcess(in_count, in, out_count, out);
    }
    STDMETHOD_(void, Process)(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS* in, UINT32, XAPO_PROCESS_BUFFER_PARAMETERS* out, BOOL) override {
        UINT32 frames = in[0].ValidFrameCount;
        // Only this title's mastering mix can release the first-view gate.
        // Check outside the capture-size limit so interactive runs remain valid.
        if (!g_authentic_audio_active.load() && in[0].BufferFlags==XAPO_BUFFER_VALID) {
            const float* samples=static_cast<const float*>(in[0].pBuffer);
            for (size_t i=0;i<size_t(frames)*channels_;++i)
                if (samples[i]>0.00001f || samples[i]<-0.00001f) { g_authentic_audio_active.store(true); break; }
        }
        if (g_capture_recording) {
        EnterCriticalSection(&g_capture_lock);
        if (channels_ == 2 && g_capture_frames + g_capture_pending.size() / 2 < kCaptureMaxFrames) {
            const float* src = static_cast<const float*>(in[0].pBuffer);
            if (in[0].BufferFlags == XAPO_BUFFER_VALID) g_capture_pending.insert(g_capture_pending.end(), src, src + frames * 2);
            else g_capture_pending.insert(g_capture_pending.end(), static_cast<size_t>(frames) * 2, 0.0f);
        }
        LeaveCriticalSection(&g_capture_lock);
        }
        out[0].BufferFlags = in[0].BufferFlags; out[0].ValidFrameCount = frames;
    }
};
XAPO_REGISTRATION_PROPERTIES CaptureXapo::s_props = {
    __uuidof(CaptureXapo), L"GhouliesCapture", L"", 1, 0,
    XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
    XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED, 1, 1, 1, 1};

DWORD WINAPI CaptureWriter(void*) {
    std::wstring path = g_frame_dir + L"\\audio-mix.wav";
    FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) Stop(268, "cannot create audio capture file");
    auto header = [&](uint32_t data_bytes) {
        uint8_t h[44] = {'R','I','F','F'}; uint32_t v;
        v = 36 + data_bytes; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
        v = 16; memcpy(h + 16, &v, 4); uint16_t tag = 3, ch = 2; memcpy(h + 20, &tag, 2); memcpy(h + 22, &ch, 2);
        v = 48000; memcpy(h + 24, &v, 4); v = 48000 * 8; memcpy(h + 28, &v, 4); uint16_t align = 8, bits = 32; memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
        memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
        fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, 0, SEEK_END);
    };
    header(0);
    for (;;) {
        Sleep(250);
        std::vector<float> chunk;
        EnterCriticalSection(&g_capture_lock); chunk.swap(g_capture_pending); LeaveCriticalSection(&g_capture_lock);
        if (chunk.empty()) continue;
        fwrite(chunk.data(), sizeof(float), chunk.size(), f);
        g_capture_frames += chunk.size() / 2;
        header(static_cast<uint32_t>(g_capture_frames * 8));
        fflush(f);
    }
}

void StartCapture() {
    if (g_frame_dir.empty()) {
        if (g_present_enabled) Stop(291,"interactive presentation requires an evidence report path");
        return;
    }
    wchar_t no_capture[8]={}; DWORD n=GetEnvironmentVariableW(L"GH_NO_AUDIO_CAPTURE",no_capture,_countof(no_capture));
    if(n && (n!=1 || no_capture[0]!=L'1')) Stop(268,"GH_NO_AUDIO_CAPTURE must be 1 or unset");
    g_capture_recording=!n;
    InitializeCriticalSection(&g_capture_lock);
    auto* xapo = new CaptureXapo();
    XAUDIO2_EFFECT_DESCRIPTOR d{static_cast<IXAPO*>(xapo), TRUE, 2};
    XAUDIO2_EFFECT_CHAIN chain{1, &d};
    HRESULT hr = g_master->SetEffectChain(&chain);
    xapo->Release();
    if (FAILED(hr)) Stop(269, "audio capture effect failed (%#lx)", static_cast<unsigned long>(hr));
    if(!g_capture_recording) { Log("audio: authentic mastering-mix gate active; file capture disabled"); return; }
    HANDLE t = CreateThread(nullptr, 0, CaptureWriter, nullptr, 0, nullptr);
    if (!t) Stop(268, "cannot start audio capture writer");
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t);
    Log("audio: capturing the mixed output to audio-mix.wav (evidence only, max 120 s)");
}

// ---- CDirectSound ----
int32_t __stdcall DsDirectSoundCreate(uint32_t guid, uint32_t* out, uint32_t outer) {
    if (outer) Stop(244, "DirectSoundCreate with aggregation");
    if (!g_xaudio) {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) Stop(245, "XAudio2Create failed (%#lx, CoInitializeEx %#lx)", static_cast<unsigned long>(hr), static_cast<unsigned long>(co));
        hr = g_xaudio->CreateMasteringVoice(&g_master, 2, 48000);
        if (FAILED(hr)) Stop(246, "XAudio2 mastering voice failed (%#lx)", static_cast<unsigned long>(hr));
        InitializeCriticalSection(&g_ds_lock);
        hr=g_xaudio->RegisterForCallbacks(&g_audio_engine_callback);
        if(FAILED(hr))Stop(268,"XAudio2 engine callback registration failed");
        Log("DirectSoundCreate: XAudio2 stereo 48 kHz output");
        StartCapture();
    }
    *out = DsAcquireDeviceInterface();
    return 0;   // DS_OK
}

int32_t __stdcall DsSetMixBinHeadroom(uint32_t ds, uint32_t mask, uint32_t headroom) {
    for (int i = 0; i < 8; ++i) if (mask & (1u << i)) g_mixbin_headroom[i] = static_cast<int32_t>(headroom);
    return 0;
}
int32_t __stdcall DsCommitDeferredSettings(uint32_t) { return 0; }   // settings are applied immediately
int32_t __stdcall DsSetI3DL2Listener(uint32_t, uint32_t listener, uint32_t apply) {
    Log("SetI3DL2Listener(%08x): recorded (reverb effects are not rendered)", listener);
    return 0;
}
void __stdcall DsDoWork() { DsoundWork(); }
int32_t __stdcall DsDownloadEffectsImage(uint32_t ds, uint32_t image, uint32_t size, uint32_t location, uint32_t* desc) {
    Log("DownloadEffectsImage(%u bytes): recorded (DSP effects are not emulated)", size);
    if (desc) Stop(251, "DownloadEffectsImage with an image descriptor out-parameter not provided");
    return 0;
}

// CreateSoundBuffer(this, DSBUFFERDESC*, ppBuffer, outer). DSBUFFERDESC: dwSize, dwFlags,
// dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin.
int32_t __stdcall DsCreateSoundBuffer(uint32_t ds, uint32_t desc, uint32_t* out, uint32_t outer) {
    auto* d = At<uint32_t>(desc);
    auto* v = new DsVoice{DsVoice::Buffer};
    v->flags = d[1];
    if (!d[3] && (d[1] & 0x2000u) && !(d[1] & ~0x6020A0u) && !d[2]) {
        // MIXIN buffer. 0x200000/0x400000 are set by XACT CreateSoundSource for
        // 3D sources (XACTENG 0x1c3908); the original voice-settings code uses
        // them only to select 3D/I3DL2 parameter storage (DSOUND 0x1c8262,
        // 0x1c82e3), consumed by the 3D voice-data calls, which still stop.
        v->kind = DsVoice::Submix;
        // MIXIN is a live routing destination as soon as it is created. XACT
        // does not Play it before sending positional sources into it.
        v->playing = true;
        ReadMixBins(v, d[4]);
        HRESULT hr = g_xaudio->CreateSubmixVoice(&v->submix, 1, 48000);
        if (FAILED(hr)) Stop(264, "XAudio2 submix voice failed (%#lx)", static_cast<unsigned long>(hr));
        EnterCriticalSection(&g_ds_lock);
        NewDsObject(v); *out = DsInterface(v);
        LeaveCriticalSection(&g_ds_lock);
        ApplyVoiceParameters(v);
        Log("CreateSoundBuffer: MIXIN buffer %08x (flags %#x, %zu mix bins)", *out, d[1], v->mixbins.size());
        if (AudioTraceEnabled()) {   // likely guest return addresses on the stack (diagnostic)
            const uint32_t* sp = static_cast<const uint32_t*>(_AddressOfReturnAddress());
            char line[256]; int n = 0;
            for (int i = 0; i < 96 && n < 220; ++i) if (sp[i] >= 0x11000 && sp[i] < 0x200000) n += snprintf(line + n, sizeof line - n, " %08x", sp[i]);
            Log("audio mixin create stack:%s", line);
        }
        return 0;
    }
    if (d[3]) memcpy(&v->format, At<void>(d[3]), sizeof(WAVEFORMATEX) - 2);
    else {
        Log("CreateSoundBuffer desc: size %u flags %#x bytes %u format %08x mixbins %08x input bin %u; caller %08x",
            d[0], d[1], d[2], d[3], d[4], d[5], static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        if (d[4]) { uint32_t n = *At<uint32_t>(d[4]), pp = *At<uint32_t>(d[4] + 4); for (uint32_t i = 0; i < n && i < 8; ++i) Log("  mixbin %u volume %d", *At<uint32_t>(pp + 8 * i), *At<int32_t>(pp + 8 * i + 4)); }
        Stop(252, "CreateSoundBuffer without a format not provided (flags %#x)", d[1]);
    }
    ReadMixBins(v, d[4]);
    if (d[2]) { v->owned.assign(d[2], 0); v->data = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v->owned.data())); v->bytes = d[2]; }
    EnterCriticalSection(&g_ds_lock);
    NewDsObject(v); *out = DsInterface(v);
    LeaveCriticalSection(&g_ds_lock);
    CreateSourceVoice(v, nullptr);
    return 0;
}

// CreateSoundStream(this, DSSTREAMDESC*, ppStream, outer). DSSTREAMDESC: dwFlags,
// dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins.
int32_t __stdcall DsCreateSoundStream(uint32_t ds, uint32_t desc, uint32_t* out, uint32_t outer) {
    auto* d = At<uint32_t>(desc);
    auto* v = new DsVoice{DsVoice::Stream};
    v->flags=d[0];
    if (!d[2]) Stop(253, "CreateSoundStream without a format not provided");
    memcpy(&v->format, At<void>(d[2]), sizeof(WAVEFORMATEX) - 2);
    v->max_packets = d[1]; v->callback = d[3]; v->context = d[4];
    ReadMixBins(v, d[5]);
    EnterCriticalSection(&g_ds_lock);
    NewDsObject(v); *out = DsInterface(v);
    LeaveCriticalSection(&g_ds_lock);
    CreateSourceVoice(v, &g_stream_callback);
    v->voice->Start();
    Log("CreateSoundStream(%u ch, %u Hz, tag %#x, %u packets, callback %08x)", v->format.nChannels, v->format.nSamplesPerSec, v->format.wFormatTag, v->max_packets, v->callback);
    return 0;
}

// ---- Reference counting (DSound_CRefCount_AddRef / Release) ----
uint32_t __stdcall DsAddRef(uint32_t object) {
    if (object && object == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_dsound_object))) return DsDeviceAddRef(object);
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "AddRef"); uint32_t r = ++*At<uint32_t>(v->refs_at); LeaveCriticalSection(&g_ds_lock); return r;
}
uint32_t __stdcall DsRelease(uint32_t object) {
    if (object && object == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_dsound_object))) return DsDeviceRelease(object);
    EnterCriticalSection(&g_ds_lock);
    DsVoice* v = DsFind(object, "Release");
    if (*At<uint32_t>(v->refs_at) == 0) Stop(267, "Release of DirectSound object %08x with no references", object);
    int32_t r = static_cast<int32_t>(--*At<uint32_t>(v->refs_at));
    if(AudioTraceEnabled())Log("audio voice release: tick %llu obj %08x kind %u refs %d",GetTickCount64(),v->object,v->kind,r);
    if (r == 0) {
        if(AudioTraceEnabled())++g_trace_voice_frees;
        for (auto& o : g_ds_objects) if (o.second->output == v) Stop(265, "MIXIN buffer %08x released while voice %08x still routes to it", v->object, o.first);
        object = v->object;
        g_ds_objects.erase(object);
        if (v->kind == DsVoice::Stream) g_ds_objects.erase(object + 4);
        LeaveCriticalSection(&g_ds_lock);
        TraceAudioRoute(v,"release");
        DestroySourceVoice(v);
        if (v->submix) v->submix->DestroyVoice();
        delete v;
        HeapFree(g_pool_heap, 0, At<void>(object));
        return 0;
    }
    LeaveCriticalSection(&g_ds_lock);
    return static_cast<uint32_t>(r);
}

// A Flush/format-change completion may release the caller's last reference.
// Keep this host object alive until the API has finished touching it.
struct DsCallRef {
    uint32_t object;
    explicit DsCallRef(uint32_t o):object(o){DsAddRef(object);}
    ~DsCallRef(){DsRelease(object);}
};

// ---- Buffers ----
void SubmitBuffer(DsVoice* v) {
    v->voice->Stop(); v->voice->FlushSourceBuffers();
    XAUDIO2_VOICE_STATE st{}; v->voice->GetState(&st); v->base_samples = st.SamplesPlayed;
    XAUDIO2_BUFFER b{};
    if (IsAdpcm(v->format)) {
        DecodeXboxAdpcm(At<uint8_t>(v->data), v->bytes, v->format.nChannels, v->decoded);
        b.pAudioData = reinterpret_cast<const BYTE*>(v->decoded.data()); b.AudioBytes = static_cast<UINT32>(v->decoded.size() * 2);
    } else { b.pAudioData = At<const BYTE>(v->data); b.AudioBytes = v->bytes; }
    if (!b.AudioBytes) return;
    b.PlayBegin = BytesToSamples(v, v->play_start); b.PlayLength = v->play_length ? BytesToSamples(v, v->play_length) : 0;
    if (v->looping) {
        b.LoopBegin = BytesToSamples(v, v->loop_start); b.LoopLength = v->loop_length ? BytesToSamples(v, v->loop_length) : 0;
        b.LoopCount = XAUDIO2_LOOP_INFINITE;
        if (b.PlayLength && b.LoopBegin + (b.LoopLength ? b.LoopLength : 0) > b.PlayBegin + b.PlayLength) { b.PlayLength = 0; }
    }
    b.Flags = XAUDIO2_END_OF_STREAM;
    if (FAILED(v->voice->SubmitSourceBuffer(&b))) Stop(254, "XAudio2 buffer submission failed");
}

int32_t __stdcall DsBufferPlay(uint32_t object, uint32_t, uint32_t, uint32_t flags) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Play"); LeaveCriticalSection(&g_ds_lock);
    if (flags & 4) Stop(255, "Play with DSBPLAY_SYNCHPLAYBACK not provided");
    if (v->kind == DsVoice::Submix) { if(AudioTraceEnabled())Log("audio mixin play: tick %llu obj %08x flags %#x",GetTickCount64(),v->object,flags);
        v->playing = true; v->mixin_played = true; v->looping = (flags & 1) != 0; ApplyVoiceParameters(v); return 0; }
    v->looping = (flags & 1) != 0; v->paused = false; ++v->stat_plays;
    SubmitBuffer(v);
    TraceAudioRoute(v,"buffer play");
    if(AudioTraceEnabled()) {
        const int16_t* samples=IsAdpcm(v->format)?v->decoded.data():At<int16_t>(v->data);
        size_t count=IsAdpcm(v->format)?v->decoded.size():v->bytes/2;
        int peak=0; for(size_t i=0;i<count;++i) peak=std::max(peak,abs(int(samples[i])));
        Log("  source PCM peak %d samples %zu",peak,count);
        if(v->output) TraceAudioRoute(v->output,"buffer destination");
    }
    v->voice->Start(); v->playing = true;
    return 0;
}
int32_t __stdcall DsBufferStop(uint32_t object) {
    if(AudioTraceEnabled())Log("audio buffer stop: tick %llu obj %08x",GetTickCount64(),object);
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Stop"); LeaveCriticalSection(&g_ds_lock);
    if (v->kind == DsVoice::Submix) { if(AudioTraceEnabled())Log("audio mixin stop: tick %llu obj %08x",GetTickCount64(),v->object);
        v->playing = false; v->mixin_played = false; ApplyVoiceParameters(v); return 0; }
    v->voice->Stop(); v->voice->FlushSourceBuffers(); v->playing = false;
    return 0;
}
int32_t __stdcall DsBufferStopEx(uint32_t object, uint32_t lo, uint32_t hi, uint32_t flags) {
    if(AudioTraceEnabled())Log("audio buffer stop-ex: tick %llu obj %08x timestamp %08x%08x flags %#x",GetTickCount64(),object,hi,lo,flags);
    return DsBufferStop(object);
}   // envelope release not rendered
int32_t __stdcall DsBufferGetStatus(uint32_t object, uint32_t* status) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "GetStatus"); LeaveCriticalSection(&g_ds_lock);
    // XACT's engine (0x1bbc89) destroys a released 3D sound source and returns
    // its voices to the pool only once IDirectSoundBuffer_GetStatus on the
    // source's MIXIN buffer no longer reports PLAYING (source check 0x1c3a03).
    // XACT never Plays these buffers, so reporting the always-live routing as
    // PLAYING leaked every source until CreateSoundSource ran out of voices
    // and the title stopped issuing positional sound effects.
    if (v->kind == DsVoice::Submix) { *status = v->mixin_played ? (1u | (v->looping ? 4u : 0u)) : 0u;
        if(AudioTraceEnabled()){static std::unordered_map<uint32_t,uint32_t> last; if(!last.count(v->object)||last[v->object]!=*status){last[v->object]=*status;
            Log("audio mixin status: tick %llu obj %08x status %#x caller %08x",GetTickCount64(),v->object,*status,uint32_t(uintptr_t(_ReturnAddress())));}}
        return 0; }
    XAUDIO2_VOICE_STATE st{}; v->voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (v->playing && st.BuffersQueued == 0) v->playing = false;
    *status = v->playing ? (1u | (v->looping ? 4u : 0u)) : 0u;
    if(AudioTraceEnabled() && v->trace_status!=*status){v->trace_status=*status;Log("audio buffer status: tick %llu obj %08x status %#x host_queued %u",GetTickCount64(),object,*status,st.BuffersQueued);}
    return 0;
}
int32_t __stdcall DsBufferGetCurrentPosition(uint32_t object, uint32_t* play, uint32_t* write) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "GetCurrentPosition"); LeaveCriticalSection(&g_ds_lock);
    XAUDIO2_VOICE_STATE st{}; v->voice->GetState(&st);
    uint64_t played = st.SamplesPlayed - v->base_samples;
    uint32_t total = BytesToSamples(v, v->bytes), pos = BytesToSamples(v, v->play_start) + static_cast<uint32_t>(played);
    if (v->looping && total) {
        uint32_t ls = BytesToSamples(v, v->loop_start), ll = v->loop_length ? BytesToSamples(v, v->loop_length) : total - ls;
        if (ll && pos >= ls + ll) pos = ls + (pos - ls) % ll;
    } else if (pos > total) pos = total;
    uint32_t bytes = SamplesToBytes(v, pos);
    if (play) *play = bytes;
    if (write) *write = bytes;
    return 0;
}
int32_t __stdcall DsBufferSetCurrentPosition(uint32_t object, uint32_t position) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "SetCurrentPosition"); LeaveCriticalSection(&g_ds_lock);
    v->play_start = position;
    if (v->playing) { SubmitBuffer(v); v->voice->Start(); }
    return 0;
}
int32_t __stdcall DsBufferSetBufferData(uint32_t object, uint32_t data, uint32_t bytes) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "SetBufferData"); LeaveCriticalSection(&g_ds_lock);
    if (v->playing) { v->voice->Stop(); v->voice->FlushSourceBuffers(); v->playing = false; }
    v->owned.clear(); v->data = data; v->bytes = bytes; v->play_start = v->play_length = v->loop_start = v->loop_length = 0;
    return 0;
}
int32_t __stdcall DsBufferSetLoopRegion(uint32_t object, uint32_t start, uint32_t length) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "SetLoopRegion"); LeaveCriticalSection(&g_ds_lock);
    v->loop_start = start; v->loop_length = length; return 0;
}
int32_t __stdcall DsBufferSetPlayRegion(uint32_t object, uint32_t start, uint32_t length) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "SetPlayRegion"); LeaveCriticalSection(&g_ds_lock);
    v->play_start = start; v->play_length = length; return 0;
}
int32_t __stdcall DsBufferLock(uint32_t object, uint32_t offset, uint32_t bytes, uint32_t* p1, uint32_t* n1, uint32_t* p2, uint32_t* n2, uint32_t flags) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Lock"); LeaveCriticalSection(&g_ds_lock);
    if (!v->bytes) Stop(256, "Lock on a buffer without data");
    if (flags & 2) { offset = 0; bytes = v->bytes; }                 // DSBLOCK_ENTIREBUFFER
    else if (flags & 1) Stop(257, "Lock from the write cursor not provided");
    offset %= v->bytes;
    uint32_t first = std::min(bytes, v->bytes - offset);
    *p1 = v->data + offset; *n1 = first;
    if (p2) *p2 = bytes > first ? v->data : 0;
    if (n2) *n2 = bytes > first ? bytes - first : 0;
    return 0;
}

// ---- Voice settings (buffers and streams) ----
int32_t __stdcall DsSetVolume(uint32_t object, int32_t volume) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetVolume"); LeaveCriticalSection(&g_ds_lock);
    if(AudioTraceEnabled() && v->volume!=volume)Log("audio volume: tick %llu obj %08x old %d new %d",GetTickCount64(),v->object,v->volume,volume);
    v->volume = volume; ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetHeadroom(uint32_t object, uint32_t headroom) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetHeadroom"); LeaveCriticalSection(&g_ds_lock);
    v->headroom = static_cast<int32_t>(headroom); ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetFrequency(uint32_t object, uint32_t frequency) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetFrequency"); LeaveCriticalSection(&g_ds_lock);
    v->frequency = frequency; v->pitch_set = false; ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetPitch(uint32_t object, int32_t pitch) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetPitch"); LeaveCriticalSection(&g_ds_lock);
    if(AudioTraceEnabled())Log("audio pitch: tick %llu obj %08x old %d new %d rate %u caller %08x",GetTickCount64(),v->object,DsPitchRegister(v),pitch,v->format.nSamplesPerSec,uint32_t(uintptr_t(_ReturnAddress())));
    v->pitch_override = pitch; v->pitch_set = true; ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetMixBins(uint32_t object, uint32_t mixbins) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetMixBins"); LeaveCriticalSection(&g_ds_lock);
    ReadMixBins(v, mixbins); ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetMixBinVolumes(uint32_t object, uint32_t mixbins) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "SetMixBinVolumes"); LeaveCriticalSection(&g_ds_lock);
    uint32_t count = *At<uint32_t>(mixbins), pairs = *At<uint32_t>(mixbins + 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bin = *At<uint32_t>(pairs + i * 8); int32_t vol = *At<int32_t>(pairs + i * 8 + 4);
        bool found = false;
        for (auto& m : v->mixbins) if (m.bin == bin) { m.volume = vol; found = true; }
        if (!found) v->mixbins.push_back({bin, vol});
    }
    ApplyVoiceParameters(v); return 0;
}
int32_t __stdcall DsSetFormat(uint32_t object, uint32_t format) {
    DsCallRef hold(object);
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFindSource(object, "SetFormat"); LeaveCriticalSection(&g_ds_lock);
    if(AudioTraceEnabled())Log("audio format: tick %llu obj %08x old rate %u pending %zu",GetTickCount64(),v->object,v->format.nSamplesPerSec,v->queue.size());
    memcpy(&v->format, At<void>(format), sizeof(WAVEFORMATEX) - 2);
    v->envelope_complete=false;
    CreateSourceVoice(v, v->kind == DsVoice::Stream ? &g_stream_callback : nullptr);
    if (v->kind == DsVoice::Stream && !v->paused && !v->pause_noactivate) v->voice->Start();
    return 0;
}
// Envelope descriptors have ten DWORDs. The amplitude generator is 1.
// Keep release state for stream stop semantics. Attack/hold/decay and filter
// modulation remain outside this release/completion repair.
static void DsLogDescriptorOnce(const char* what, uint32_t object, uint32_t descriptor, unsigned dwords);
int32_t __stdcall DsSetEG(uint32_t object, uint32_t descriptor) {
    EnterCriticalSection(&g_ds_lock);DsVoice* v=DsFind(object,"SetEG");
    const auto* d=At<const uint32_t>(descriptor);
    if(d[0]>1){LeaveCriticalSection(&g_ds_lock);return int32_t(0x80070057u);}
    v->envelope_release[d[0]]=d[6]&0xfff;
    LeaveCriticalSection(&g_ds_lock);
    DsLogDescriptorOnce("SetEG", object, descriptor, 10);   // only release is rendered
    if(AudioTraceEnabled())Log("audio envelope: tick %llu obj %08x generator %u mode %u delay %u attack %u hold %u decay %u release %u sustain %u",GetTickCount64(),object,d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
    return 0;
}
// Diagnostics (cheap): each distinct descriptor is logged once with the voice
// it was first applied to, so scene-specific shaping can be identified.
static void DsLogDescriptorOnce(const char* what, uint32_t object, uint32_t descriptor, unsigned dwords) {
    static CRITICAL_SECTION lock; static bool init = [] { InitializeCriticalSection(&lock); return true; }(); (void)init;
    static std::set<std::string> seen;
    char key[256]; int n = snprintf(key, sizeof key, "%s", what);
    if (descriptor) { const auto* d = At<const uint32_t>(descriptor); for (unsigned i = 0; i < dwords; ++i) n += snprintf(key + n, sizeof key - n, " %08x", d[i]); }
    else snprintf(key + n, sizeof key - n, " null");
    EnterCriticalSection(&lock);
    bool fresh = seen.size() < 400 && seen.insert(key).second;
    LeaveCriticalSection(&lock);
    if (!fresh) return;
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, what);
    int kind = v->kind; unsigned tag = v->format.wFormatTag, rate = v->format.nSamplesPerSec, ch = v->format.nChannels;
    LeaveCriticalSection(&g_ds_lock);
    Log("audio descriptor (not rendered unless noted): %s | obj %08x kind %d fmt %#x %u Hz %u ch", key, object, kind, tag, rate, ch);
}
// LFO and filter shaping remain unrendered (logged once per descriptor).
int32_t __stdcall DsSetLFO(uint32_t object, uint32_t descriptor) { DsLogDescriptorOnce("SetLFO", object, descriptor, 6); return 0; }
int32_t __stdcall DsSetFilter(uint32_t object, uint32_t descriptor) { DsLogDescriptorOnce("SetFilter", object, descriptor, 6); return 0; }
int32_t __stdcall DsSetOutputBuffer(uint32_t object, uint32_t output) {
    EnterCriticalSection(&g_ds_lock);
    DsVoice* v = DsFind(object, "SetOutputBuffer");
    // pOutputBuffer is the IDirectSoundBuffer interface pointer (object + 0x1c).
    DsVoice* dst = output ? DsFind(output - 0x1c, "SetOutputBuffer destination") : nullptr;
    LeaveCriticalSection(&g_ds_lock);
    if (v->kind == DsVoice::Submix) {
        if (dst) Stop(258, "SetOutputBuffer on a MIXIN buffer (submix chaining) not provided");
        return 0;   // a MIXIN buffer already outputs through its mix bins
    }
    if (dst && dst->kind != DsVoice::Submix) Stop(258, "SetOutputBuffer to a buffer that is not MIXIN (flags %#x)", dst->flags);
    if (dst) { XAUDIO2_SEND_DESCRIPTOR sd{0, dst->submix}; XAUDIO2_VOICE_SENDS sends{1, &sd}; v->voice->SetOutputVoices(&sends); }
    else v->voice->SetOutputVoices(nullptr);
    v->output = dst;
    ApplyVoiceParameters(v);
    return 0;
}

// ---- Streams ----
int32_t __stdcall DsStreamProcess(uint32_t object, uint32_t input, uint32_t output) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Process"); LeaveCriticalSection(&g_ds_lock);
    if (output) Stop(259, "stream Process with an output packet");
    uint32_t buffer = *At<uint32_t>(input), size = *At<uint32_t>(input + 4), pdw_status = *At<uint32_t>(input + 12);
    if (pdw_status) *At<uint32_t>(pdw_status) = 0x8000000A;   // XMP_STATUS_PENDING
    auto* p=new Packet{};p->voice=v;p->packet=input;p->buffer=buffer;p->size=size;
    if(AudioTraceEnabled())p->trace_id=++g_audio_packet_serial;
    p->info={*At<uint32_t>(input+8),pdw_status,*At<uint32_t>(input+16),size};++v->stat_packets;
    if (v->stat_packets <= 4) Log("stream %08x Process packet %08x: buffer %08x size %u completed* %08x status* %08x event %08x context %08x",
        object, input, buffer, size, *At<uint32_t>(input + 8), pdw_status, *At<uint32_t>(input + 16), *At<uint32_t>(input + 20));
    if(AudioTraceEnabled() && !IsAdpcm(v->format))p->trace_hash=AudioPacketHash(buffer,size);
    XAUDIO2_BUFFER b{};
    if (IsAdpcm(v->format)) { DecodeXboxAdpcm(At<uint8_t>(buffer), size, v->format.nChannels, p->pcm); }
    EnterCriticalSection(&g_ds_lock);
    v->queue.push_back(p);v->packets.push_back(p);
    Packet& q = *p;
    LeaveCriticalSection(&g_ds_lock);
    if (IsAdpcm(v->format)) { b.pAudioData = reinterpret_cast<const BYTE*>(q.pcm.data()); b.AudioBytes = static_cast<UINT32>(q.pcm.size() * 2); }
    else { b.pAudioData = At<const BYTE>(buffer); b.AudioBytes = size; }
    if(AudioTraceEnabled() && v->stat_packets<=8) { TraceAudioRoute(v,"stream packet"); if(v->output) TraceAudioRoute(v->output,"stream destination"); }
    if(AudioTraceEnabled() && v->stat_packets<=4) {
        int peak=0; uint64_t nonzero=0;
        const int16_t* pcm=reinterpret_cast<const int16_t*>(b.pAudioData);
        if(IsAdpcm(v->format) || v->format.wBitsPerSample==16) {
            for(unsigned i=0;i<b.AudioBytes/2;++i) { peak=std::max(peak,abs(int(pcm[i]))); nonzero+=pcm[i]!=0; }
            Log("audio packet samples: obj %08x packet %llu peak %d nonzero %llu/%u",v->object,v->stat_packets,peak,nonzero,b.AudioBytes/2);
        }
    }
    // Copy complete submitted packets without inserting an effect into playback.
    // This is source evidence, including data that a later Flush may cancel.
    if(AudioTraceEnabled() && !g_frame_dir.empty() && !CaptureDisabled()) {
        static unsigned saved=0;
        if(v->trace_path.empty() && saved<128) {
            ++saved; wchar_t name[128]; swprintf_s(name,L"\\source-%llu-%08x.wav",GetTickCount64(),v->object);
            v->trace_path=g_frame_dir+name;
        }
        uint16_t bits=IsAdpcm(v->format)?16:v->format.wBitsPerSample,ch=v->format.nChannels;
        uint16_t align=ch*bits/8; uint32_t rate=v->format.nSamplesPerSec;
        uint32_t cap=rate*align*8;
        if(!v->trace_path.empty() && v->trace_bytes<cap) {
            FILE* f=nullptr;_wfopen_s(&f,v->trace_path.c_str(),v->trace_bytes?L"r+b":L"w+b");
            if(!f)Stop(268,"cannot create diagnostic source WAV");
            uint32_t bytes=std::min(b.AudioBytes,cap-v->trace_bytes);v->trace_bytes+=bytes;
            uint8_t header[44]={};memcpy(header,"RIFF",4);memcpy(header+8,"WAVEfmt ",8);memcpy(header+36,"data",4);
            uint32_t size=36+v->trace_bytes,fmt_size=16,avg=rate*align;uint16_t tag=1;
            memcpy(header+4,&size,4);memcpy(header+16,&fmt_size,4);memcpy(header+20,&tag,2);memcpy(header+22,&ch,2);
            memcpy(header+24,&rate,4);memcpy(header+28,&avg,4);memcpy(header+32,&align,2);memcpy(header+34,&bits,2);memcpy(header+40,&v->trace_bytes,4);
            fwrite(header,1,44,f);fseek(f,0,SEEK_END);fwrite(b.pAudioData,1,bytes,f);fclose(f);
        }
    }
    b.pContext = p;
    if(p->trace_id)Log("audio packet submit: id %llu tick %llu obj %08x buffer %08x size %u rate %u ch %u bits %u pitch %d volume %d output %08x paused %d hold %d xact %u filebytes %u",p->trace_id,GetTickCount64(),v->object,buffer,size,v->format.nSamplesPerSec,v->format.nChannels,v->format.wBitsPerSample,DsPitchRegister(v),v->volume,v->output?v->output->object:0,v->paused,v->pause_noactivate,g_audio_xact_call,v->trace_bytes);
    if (FAILED(v->voice->SubmitSourceBuffer(&b))) Stop(260, "XAudio2 stream submission failed");
    return 0;
}
int32_t __stdcall DsStreamGetStatus(uint32_t object, uint32_t* status) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "stream GetStatus");
    uint32_t queued = static_cast<uint32_t>(v->queue.size()); LeaveCriticalSection(&g_ds_lock);
    *status = (queued < v->max_packets ? 1u : 0u) | (v->envelope_complete ? 0x80000u : ((queued ? 0x10000u : 0x40000u) | (v->paused ? 0x20000u : 0u)));
    if(AudioTraceEnabled() && v->trace_status!=*status){v->trace_status=*status;Log("audio status: tick %llu obj %08x status %#x queued %u paused %d hold %d caller %08x",GetTickCount64(),v->object,*status,queued,v->paused,v->pause_noactivate,uint32_t(uintptr_t(_ReturnAddress())));}
    return 0;
}
int32_t __stdcall DsStreamPause(uint32_t object, uint32_t pause) {
    Log("stream %08x Pause(%u)", object, pause);
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "stream Pause"); LeaveCriticalSection(&g_ds_lock);
    if(AudioTraceEnabled())Log("audio pause: tick %llu obj %08x mode %u pending %zu",GetTickCount64(),v->object,pause,v->queue.size());
    // Original CMcpxStream::Pause (0x1cb290): PAUSENOACTIVATE (3)
    // holds an idle stream while XACT fills it, but does not stop an active
    // stream. It survives SetFormat; RESUME clears the activation hold.
    if (pause == 2) Stop(259,"stream synchronized playback not provided");
    if (pause > 3) return static_cast<int32_t>(0x80004005u);
    if (pause == 3) {
        XAUDIO2_VOICE_STATE st{};v->voice->GetState(&st);
        if (!st.BuffersQueued || v->pause_noactivate) {v->pause_noactivate=true;v->voice->Stop();}
        else {v->paused=false;v->pause_noactivate=false;v->voice->Start();}
    } else {
        v->paused=pause==1;
        if (!v->paused) v->pause_noactivate=false;
        if (v->paused || v->pause_noactivate) v->voice->Stop(); else v->voice->Start();
    }
    if(AudioTraceEnabled() && pause==0){TraceAudioRoute(v,"resume");if(v->output)TraceAudioRoute(v->output,"resume destination");}
    return 0;
}
void FlushStream(DsVoice* v) {
    ResetStreamEnvelope(v);
    v->trace_path.clear();v->trace_bytes=0;
    v->voice->Stop();
    std::vector<PacketInfo> flushed;
    EnterCriticalSection(&g_ds_lock);
    // Detach and mark these exact identities before XAudio2 can deliver any
    // Flush callbacks. PCM/context stay alive until their own OnBufferEnd.
    for(auto* p:v->queue){
        if(p->trace_id)Log("audio packet cancel: id %llu tick %llu obj %08x start %llu reason flush",p->trace_id,GetTickCount64(),v->object,p->trace_start);
        p->completed=true;flushed.push_back(p->info);
    }
    v->queue.clear();
    LeaveCriticalSection(&g_ds_lock);
    v->voice->FlushSourceBuffers();
    const uint32_t callback=v->callback,context=v->context;
    if (!v->paused && !v->pause_noactivate) v->voice->Start();
    for(const auto& info:flushed)CompletePacketValues(callback,context,info,0x80004004,0);
}
int32_t __stdcall DsStreamFlush(uint32_t object) {
    DsCallRef hold(object);
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "stream Flush"); LeaveCriticalSection(&g_ds_lock);
    if(AudioTraceEnabled())Log("audio flush: tick %llu obj %08x queued %zu caller %08x",GetTickCount64(),object,v->queue.size(),uint32_t(uintptr_t(_ReturnAddress())));
    FlushStream(v); return 0;
}
void UpdateAudioEnvelopes() {
    if(!g_audio_releases.load())return;
    std::vector<uint32_t> flush;
    EnterCriticalSection(&g_ds_lock);
    const auto now=GetTickCount64();
    for(const auto& item:g_ds_objects) {
        DsVoice* v=item.second;
        if(item.first!=v->object || !v->envelope_releasing)continue;
        double elapsed=double(now-v->envelope_started);
        bool done=elapsed>=v->envelope_duration_ms;
        // The MCPX release interval is release * 512 samples at 48 kHz.
        // A T60 decay approximates its exponential gain curve; XAudio2
        // applies each update at its processing boundary (no inserted XAPO).
        float gain=done?0.0f:v->envelope_start_gain*powf(10.0f,float(-3.0*elapsed/v->envelope_duration_ms));
        if(FAILED(v->voice->SetVolume(gain)))Stop(268,"stream release gain failed");
        if(!done)continue;
        v->voice->Stop();v->envelope_releasing=false;--g_audio_releases;
        if(v->envelope_terminal)v->envelope_complete=true;
        else {++*At<uint32_t>(v->refs_at);flush.push_back(v->object);}
        if(AudioTraceEnabled())Log("audio envelope complete: tick %llu obj %08x elapsed %.3f terminal %d",now,v->object,elapsed,v->envelope_terminal);
    }
    LeaveCriticalSection(&g_ds_lock);
    for(uint32_t object:flush){DsStreamFlush(object);DsRelease(object);}
}
int32_t __stdcall DsStreamFlushEx(uint32_t object, uint32_t lo, uint32_t hi, uint32_t flags) {
    if(AudioTraceEnabled())Log("audio flush-ex: tick %llu obj %08x timestamp %08x%08x flags %#x caller %08x",GetTickCount64(),object,hi,lo,flags,uint32_t(uintptr_t(_ReturnAddress())));
    if(flags && (lo || hi))Stop(259,"scheduled stream FlushEx not provided");
    if((flags&~7u) || (flags && !(flags&1u)))return int32_t(0x80070057u);
    if(!(flags&6u))return DsStreamFlush(object);
    DsCallRef hold(object);
    EnterCriticalSection(&g_ds_lock);DsVoice* v=DsFindSource(object,"stream FlushEx");LeaveCriticalSection(&g_ds_lock);
    // Original CMcpxStream::FlushEx(ASYNC|ENVELOPE2), 0x1ca73b,
    // retains terminal envelope state until explicit Flush. XACT tests
    // ENVELOPECOMPLETE at 0x1c2747, then Flushes at 0x1c2758. Returning
    // STARVED here makes it refill and restart the stopped cue instead.
    EnterCriticalSection(&g_ds_lock);
    v->envelope_complete=false;v->envelope_terminal=(flags&4u)!=0;
    v->voice->GetVolume(&v->envelope_start_gain);
    v->envelope_started=GetTickCount64();
    v->envelope_duration_ms=v->envelope_release[1]*512.0/48.0;
    if(!v->envelope_releasing){v->envelope_releasing=true;++g_audio_releases;}
    LeaveCriticalSection(&g_ds_lock);
    UpdateAudioEnvelopes();QueueDsoundWork();
    return 0;
}
int32_t __stdcall DsStreamDiscontinuity(uint32_t object) {
    if(AudioTraceEnabled())Log("audio discontinuity: tick %llu obj %08x caller %08x",GetTickCount64(),object,uint32_t(uintptr_t(_ReturnAddress())));
    return 0;
}   // remaining packets play out
int32_t __stdcall DsStreamGetInfo(uint32_t object, uint32_t* info) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "stream GetInfo"); LeaveCriticalSection(&g_ds_lock);
    info[0] = 0x2; /* XMO_STREAMF_FIXED_SAMPLE_SIZE */ info[1] = IsAdpcm(v->format) ? 36u * v->format.nChannels : v->format.nBlockAlign; info[2] = 0; info[3] = 0;
    if(AudioTraceEnabled())Log("audio info: tick %llu obj %08x flags %#x caller %08x",GetTickCount64(),object,info[0],uint32_t(uintptr_t(_ReturnAddress())));
    return 0;
}

// Voice properties and 3D voice data come from the original 3D calculator; not rendered yet.
// DSVOICEPROPS as the original CMcpxVoiceClient code fills it (0x1ccb0f):
// +0 mix-bin count, +4 eight {bin, volume} pairs (unused: {-1, -10000}),
// +0x44 hardware pitch, +0x48 distance, +0x4c cone, +0x50 Doppler,
// +0x54 I3DL2 direct, +0x58 I3DL2 room. Original 0x1ccb0f tests whether
// a hardware voice is allocated, not whether Play has been called. Host
// source/submix creation allocates the equivalent resource immediately.
int32_t __stdcall DsGetVoiceProperties(uint32_t object, uint32_t props) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "GetVoiceProperties"); LeaveCriticalSection(&g_ds_lock);
    if (!v->voice && !v->submix) return static_cast<int32_t>(0x88780032u);
    std::vector<MixBinPair> bins=v->mixbins;
    if(bins.empty())bins={{0,0},{1,0}};
    uint32_t* out=At<uint32_t>(props);uint32_t n=uint32_t(std::min<size_t>(8,bins.size()));out[0]=n;
    for(uint32_t i=0;i<8;++i) {
        out[1+2*i]=i<n?bins[i].bin:0xffffffffu;
        out[2+2*i]=uint32_t(i<n?DsBinVolume(v,bins[i].bin,bins[i].volume):-10000);
    }
    out[0x44/4]=uint32_t(DsPitchRegister(v));
    bool spatial=v->use_3d_voice_data && v->has_3d_data;
    const uint32_t fields[]={1,2,9,7,8};
    for(uint32_t i=0;i<5;++i)out[0x48/4+i]=spatial?v->voice3d[fields[i]]:0;
    if(AudioTraceEnabled())Log("audio voice properties: obj %08x kind %d allocated 1 bins %u pitch %d caller %08x",v->object,v->kind,n,int32_t(out[0x44/4]),uint32_t(uintptr_t(_ReturnAddress())));
    return 0;
}
bool g_hrtf_noted = false;
int32_t DsPitchRegister(const DsVoice* v) {
    if (v->kind == DsVoice::Submix) return 0;
    int64_t p;
    if (v->pitch_set) p = v->pitch_override;
    else {
        uint32_t rate = v->frequency ? v->frequency : v->format.nSamplesPerSec;
        p = lround(4096.0 * log2(rate / 48000.0));
    }
    const int32_t* d = reinterpret_cast<const int32_t*>(v->voice3d);
    if (v->use_3d_voice_data && v->has_3d_data) p += d[9];
    if (v->output) {
        const int32_t* od = reinterpret_cast<const int32_t*>(v->output->voice3d);
        if (v->output->use_3d_voice_data && v->output->has_3d_data) p += od[9];
    }
    return static_cast<int32_t>(std::max<int64_t>(-32767, std::min<int64_t>(8191, p)));
}

// Set3DVoiceData (original 0x1c5e64): data[0] low byte selects which field
// groups to store: 1:+4 (distance volume) 2:+8 (cone volume) 4:+0xc,+0x10
// 8:+0x14,+0x18 0x10:+0x1c,+0x20 (I3DL2 direct, room) 0x20:+0x24 (Doppler
// pitch) 0x40:+0x28,+0x2c (HRTF position, floats) 0x80:+0x30,+0x34. Requires
// the 3D parameter block (flags 0x400010); the original otherwise only applies
// group 0x40 to a voice without it. Volume and pitch groups are rendered.
// The HRTF filter groups (0x40, 0x80) program the console's per-voice HRTF FIR;
// like the envelope/LFO/filter and DSP effect settings above, they are
// recorded but not rendered (listed in docs/NEXT-WORK.md as an audio gap):
// 3D sources keep their exact 3D volumes, speaker-bin panning and Doppler.
int32_t __stdcall DsSet3DVoiceData(uint32_t object, uint32_t data) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Set3DVoiceData"); LeaveCriticalSection(&g_ds_lock);
    const uint32_t* d = At<uint32_t>(data);
    uint32_t f = d[0] & 0xFF;
    if (!(v->flags & 0x400010u)) {
        if (f & 0x40) Stop(262, "Set3DVoiceData HRTF position on a voice without 3D parameters not provided");
        return 0;
    }
    if (!f) return 0;
    if ((f & 0xC0) && !g_hrtf_noted) { g_hrtf_noted = true; Log("Set3DVoiceData: HRTF filter data recorded, not rendered (3D volume/pan/Doppler are)"); }
    static const struct { uint32_t bit, first, count; } groups[] = {{1,1,1},{2,2,1},{4,3,2},{8,5,2},{0x10,7,2},{0x20,9,1},{0x40,10,2},{0x80,12,2}};
    for (const auto& g : groups) if (f & g.bit) for (uint32_t i = 0; i < g.count; ++i) v->voice3d[g.first + i] = d[g.first + i];
    v->voice3d[0] = d[0];
    v->has_3d_data = true;
    ApplyVoiceParameters(v);
    // Voices routed into this MIXIN buffer inherit its Doppler pitch.
    if (v->kind == DsVoice::Submix && (f & 0x20)) for (auto& o : g_ds_objects) if (o.second->output == v && o.first == o.second->object) ApplyVoiceParameters(o.second);
    return 0;
}
// CDirectSoundVoice_Use3DVoiceData (original 0x1c5f95) only sets or clears
// settings flag 0x01000000: take 3D voice data from Set3DVoiceData rather than
// DirectSound's own 3D calculation. Recorded; the data itself is applied (or
// refused) by Set3DVoiceData.
int32_t __stdcall DsUse3DVoiceData(uint32_t object, uint32_t use) {
    EnterCriticalSection(&g_ds_lock); DsVoice* v = DsFind(object, "Use3DVoiceData"); LeaveCriticalSection(&g_ds_lock);
    v->use_3d_voice_data = use != 0;
    ApplyVoiceParameters(v);
    return 0;
}

// DirectSoundUseFullHRTF: the original (0x1c5ad2) runs the pure helper
// CHRTFSource_SetFullHRTF5Channel (0x1c9576), which installs the HRTF function
// table (0x1e3b64...) used by the original 3D calculator kept from XACT/DSOUND,
// inside the DirectSound critical section (our DirectSound calls are serialized
// by g_ds_lock and the title's own callers). Run that helper unchanged.
void __stdcall DsUseFullHRTF() {
    reinterpret_cast<void (__stdcall*)()>(static_cast<uintptr_t>(0x1c9576u))();
    Log("DirectSoundUseFullHRTF: original HRTF table installed (3D output is mixed through mix bins to stereo)");
}

// DirectSoundEnterCriticalSection, as the original (0x1c5a15): at raised IRQL
// return FALSE; otherwise enter DirectSound's critical section (0x1e34c0)
// through the title's own RtlEnterCriticalSection import (0x202ea0) and return
// TRUE; callers leave it through their RtlLeaveCriticalSection import.
uint8_t CurrentIrqlForDsound();
uint32_t __stdcall DsEnterCriticalSection() {
    if (CurrentIrqlForDsound() != 0) return 0;
    reinterpret_cast<void (__stdcall*)(uint32_t)>(static_cast<uintptr_t>(*At<uint32_t>(0x202ea0)))(0x1e34c0);
    return 1;
}

// Optional observation of the original XACT sound-bank Play, never an HLE
// replacement. Its verified six-byte prologue has no relative operands.
using XactPlayFn=int32_t (__stdcall*)(uint32_t,uint32_t,uint32_t*);
XactPlayFn g_original_xact_play=nullptr;
using XactPrepareFn=int32_t (__thiscall*)(uint32_t,uint32_t,uint32_t,uint32_t*);
XactPrepareFn g_original_xact_prepare=nullptr;
static int32_t __fastcall TraceXactPrepare(uint32_t bank,uint32_t,uint32_t prepare,uint32_t source,uint32_t* cue) {
    static std::atomic<uint32_t> serial{0};uint32_t id=++serial;
    uint32_t flags=*At<uint32_t>(prepare),index=*At<uint32_t>(prepare+4);
    Log("xact prepare begin: id %u tick %llu bank %08x flags %#x index %u source %08x caller %08x",id,GetTickCount64(),bank,flags,index,source,uint32_t(uintptr_t(_ReturnAddress())));
    int32_t result=g_original_xact_prepare(bank,prepare,source,cue);
    EnterCriticalSection(&g_ds_lock);++g_trace_prepares;
    if(result>=0 && cue && *cue){auto& tracked=g_traced_cues[*cue];tracked.index=*At<uint16_t>(*cue+0x18);tracked.bank=bank;}
    LeaveCriticalSection(&g_ds_lock);
    Log("xact prepare result: id %u tick %llu hr %#x cue %08x",id,GetTickCount64(),uint32_t(result),result>=0 && cue?*cue:0);
    return result;
}
static int32_t __stdcall TraceXactPlay(uint32_t bank,uint32_t prepare,uint32_t* cue) {
    uint32_t flags=*At<uint32_t>(prepare),index=*At<uint32_t>(prepare+4),source=*At<uint32_t>(prepare+8);
    static std::atomic<uint32_t> serial{0};uint32_t id=++serial;
    EnterCriticalSection(&g_ds_lock);++g_trace_plays;LeaveCriticalSection(&g_ds_lock);
    const uint32_t previous=g_audio_xact_call;g_audio_xact_call=id;
    Log("xact cue begin: id %u tick %llu thread %lu bank %08x flags %#x index %u source %08x caller %08x",id,GetTickCount64(),GetCurrentThreadId(),bank,flags,index,source,uint32_t(uintptr_t(_ReturnAddress())));
    if(flags&4)Log("xact prepared: id %u handle %08x cue-in %08x",id,*At<uint32_t>(prepare+12),cue?*cue:0);
    int32_t result=g_original_xact_play(bank,prepare,cue);
    g_audio_xact_call=previous;
    uint32_t resolved=result>=0 && cue?*cue:0;
    Log("xact cue result: id %u hr %#x cue %08x resolved-index %u",id,uint32_t(result),resolved,resolved?*At<uint16_t>(resolved+0x18):0xffffffffu);
    return result;
}
using XactInitFn=int32_t (__thiscall*)(uint32_t,uint32_t);
static XactInitFn g_original_xact_init=nullptr;
static int32_t __fastcall TraceXactInit(uint32_t engine,uint32_t,uint32_t params) {
    // Original 0x1bad04..10 copies these five words verbatim into engine+0x80.
    auto* p=At<uint32_t>(params);
    Log("xact engine parameters: tick %llu engine %08x words %u %u %u %u %#x",GetTickCount64(),engine,p[0],p[1],p[2],p[3],p[4]);
    return g_original_xact_init(engine,params);
}
using XactReleaseFn=uint32_t (__stdcall*)(uint32_t);
using XactStopFn=int32_t (__thiscall*)(uint32_t,uint32_t);
using XactNotifyFn=void (__thiscall*)(uint32_t,uint32_t,uint32_t);
static XactReleaseFn g_original_xact_release=nullptr;
static XactStopFn g_original_xact_stop=nullptr;
static XactNotifyFn g_original_xact_notify=nullptr;
static uint32_t __stdcall TraceXactRelease(uint32_t cue) {
    Log("xact release begin: tick %llu cue %08x refs %u index %u caller %08x",GetTickCount64(),cue,*At<uint32_t>(cue+0x34),*At<uint16_t>(cue+0x18),uint32_t(uintptr_t(_ReturnAddress())));
    const uint32_t result=g_original_xact_release(cue);
    if(!result){EnterCriticalSection(&g_ds_lock);g_traced_cues.erase(cue);++g_trace_cue_frees;LeaveCriticalSection(&g_ds_lock);}
    Log("xact release result: tick %llu cue %08x refs %u",GetTickCount64(),cue,result);
    return result;
}
static int32_t __fastcall TraceXactStop(uint32_t cue,uint32_t,uint32_t flags) {
    Log("xact stop: tick %llu cue %08x index %u flags %#x caller %08x",GetTickCount64(),cue,*At<uint16_t>(cue+0x18),flags,uint32_t(uintptr_t(_ReturnAddress())));
    return g_original_xact_stop(cue,flags);
}
static void __fastcall TraceXactNotify(uint32_t cue,uint32_t,uint32_t data,uint32_t type) {
    Log("xact notification: tick %llu cue %08x index %u type %u data %08x",GetTickCount64(),cue,*At<uint16_t>(cue+0x18),type,data);
    EnterCriticalSection(&g_ds_lock);auto found=g_traced_cues.find(cue);if(found!=g_traced_cues.end() && type<32)found->second.notifications|=1u<<type;LeaveCriticalSection(&g_ds_lock);
    g_original_xact_notify(cue,data,type);
}
// Each supplied span was disassembled: whole instructions, no relative operands.
static void* InstallXactObserver(uint32_t addr,const char* sha,const void* observer,size_t span=6) {
    if(Sha256Hex(At<uint8_t>(addr),16)!=sha)Stop(247,"XACT observer does not match original at %08x",addr);
    auto* trampoline=static_cast<uint8_t*>(VirtualAlloc(nullptr,16,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!trampoline)Stop(268,"cannot allocate XACT observation trampoline");
    if(span<5 || span>11)Stop(268,"invalid XACT observation prologue span");
    memcpy(trampoline,At<void>(addr),span);trampoline[span]=0xe9;
    int32_t rel=int32_t(uintptr_t(addr+span)-uintptr_t(trampoline+span+5));memcpy(trampoline+span+1,&rel,4);
    DWORD old=0;if(!VirtualProtect(trampoline,16,PAGE_EXECUTE_READ,&old))Stop(268,"cannot protect XACT observation trampoline");
    auto* at=At<uint8_t>(addr);rel=int32_t(uintptr_t(observer)-uintptr_t(at+5));at[0]=0xe9;memcpy(at+1,&rel,4);
    if(!FlushInstructionCache(GetCurrentProcess(),nullptr,0))Stop(268,"cannot publish XACT cue observation");
    return trampoline;
}
static void InstallXactTrace() {
    if(!AudioTraceEnabled())return;
    g_original_xact_play=reinterpret_cast<XactPlayFn>(InstallXactObserver(0x1c3608,"5c0457a717921d07cf57f0fcc2f408602c567ad3582c11af0560ba8aec3a8093",reinterpret_cast<const void*>(&TraceXactPlay)));
    g_original_xact_prepare=reinterpret_cast<XactPrepareFn>(InstallXactObserver(0x1c34cb,"8177a8cfe93b1dccf4bd3ef6581ccbc16a0ecf134e9750b6b4cb5073d3789548",reinterpret_cast<const void*>(&TraceXactPrepare)));
    g_original_xact_init=reinterpret_cast<XactInitFn>(InstallXactObserver(0x1bacf6,"fadada6a9a367486198670525d74f0aec9fda071c463a26638b276e3e79b5eda",reinterpret_cast<const void*>(&TraceXactInit)));
    g_original_xact_release=reinterpret_cast<XactReleaseFn>(InstallXactObserver(0x1bf6e3,"160d7099bff1916aa44ee41ee162e3e68c70683178735cef52d55f66252257d1",reinterpret_cast<const void*>(&TraceXactRelease),8));
    g_original_xact_stop=reinterpret_cast<XactStopFn>(InstallXactObserver(0x1bf824,"c879c91fedd8f2a1e0b0ce85e2af89518c95a24bb5d59e07a3561f1b0e0c7ef0",reinterpret_cast<const void*>(&TraceXactStop),7));
    g_original_xact_notify=reinterpret_cast<XactNotifyFn>(InstallXactObserver(0x1be874,"ffc42ccf6a4626856bfb86d2a50c1677dec2402e8a5d70c4bc49eea7f267a91c",reinterpret_cast<const void*>(&TraceXactNotify)));
    Log("audio trace: observing original XACT Prepare/Play/Stop/notification/Release; original cue processing retained");
}

struct DsoundImpl { const char* name; const void* function; };
#define DS_IMPL(name, fn) {name, reinterpret_cast<const void*>(&fn)}
const DsoundImpl kDsoundImpls[] = {
    DS_IMPL("DirectSoundCreate", DsDirectSoundCreate),
    DS_IMPL("DirectSoundUseFullHRTF", DsUseFullHRTF),
    DS_IMPL("DirectSoundEnterCriticalSection", DsEnterCriticalSection),
    DS_IMPL("CDirectSound_SetMixBinHeadroom", DsSetMixBinHeadroom),
    DS_IMPL("CDirectSound_CommitDeferredSettings", DsCommitDeferredSettings),
    DS_IMPL("CDirectSound_SetI3DL2Listener", DsSetI3DL2Listener),
    DS_IMPL("CDirectSound_DoWork", DsDoWork),
    DS_IMPL("DirectSoundDoWork", DsDoWork),
    DS_IMPL("CDirectSound_DownloadEffectsImage", DsDownloadEffectsImage),
    DS_IMPL("CDirectSound_CreateSoundBuffer", DsCreateSoundBuffer),
    DS_IMPL("CDirectSound_CreateSoundStream", DsCreateSoundStream),
    DS_IMPL("DSound_CRefCount_AddRef", DsAddRef),
    DS_IMPL("DSound_CRefCount_Release", DsRelease),
    DS_IMPL("CDirectSoundStream_AddRef", DsAddRef),
    DS_IMPL("CDirectSoundStream_Release", DsRelease),
    DS_IMPL("CDirectSoundBuffer_Play", DsBufferPlay),
    DS_IMPL("CDirectSoundBuffer_Stop", DsBufferStop),
    DS_IMPL("CDirectSoundBuffer_StopEx", DsBufferStopEx),
    DS_IMPL("CDirectSoundBuffer_GetStatus", DsBufferGetStatus),
    DS_IMPL("CDirectSoundBuffer_GetCurrentPosition", DsBufferGetCurrentPosition),
    DS_IMPL("CDirectSoundBuffer_SetCurrentPosition", DsBufferSetCurrentPosition),
    DS_IMPL("CDirectSoundBuffer_SetBufferData", DsBufferSetBufferData),
    DS_IMPL("CDirectSoundBuffer_SetLoopRegion", DsBufferSetLoopRegion),
    DS_IMPL("CDirectSoundBuffer_SetPlayRegion", DsBufferSetPlayRegion),
    DS_IMPL("CDirectSoundBuffer_Lock", DsBufferLock),
    DS_IMPL("CDirectSoundBuffer_SetVolume", DsSetVolume), DS_IMPL("CDirectSoundStream_SetVolume", DsSetVolume),
    DS_IMPL("CDirectSoundBuffer_SetHeadroom", DsSetHeadroom), DS_IMPL("CDirectSoundStream_SetHeadroom", DsSetHeadroom),
    DS_IMPL("CDirectSoundBuffer_SetFrequency", DsSetFrequency),
    DS_IMPL("CDirectSoundBuffer_SetPitch", DsSetPitch), DS_IMPL("CDirectSoundStream_SetPitch", DsSetPitch),
    DS_IMPL("CDirectSoundBuffer_SetMixBins", DsSetMixBins), DS_IMPL("CDirectSoundStream_SetMixBins", DsSetMixBins),
    DS_IMPL("CDirectSoundBuffer_SetMixBinVolumes_8", DsSetMixBinVolumes), DS_IMPL("CDirectSoundStream_SetMixBinVolumes_8", DsSetMixBinVolumes),
    DS_IMPL("CDirectSoundBuffer_SetFormat", DsSetFormat), DS_IMPL("CDirectSoundStream_SetFormat", DsSetFormat),
    DS_IMPL("CDirectSoundBuffer_SetEG", DsSetEG), DS_IMPL("CDirectSoundStream_SetEG", DsSetEG),
    DS_IMPL("CDirectSoundBuffer_SetLFO", DsSetLFO), DS_IMPL("CDirectSoundStream_SetLFO", DsSetLFO),
    DS_IMPL("CDirectSoundBuffer_SetFilter", DsSetFilter), DS_IMPL("CDirectSoundStream_SetFilter", DsSetFilter),
    DS_IMPL("CDirectSoundBuffer_SetOutputBuffer", DsSetOutputBuffer), DS_IMPL("CDirectSoundStream_SetOutputBuffer", DsSetOutputBuffer),
    DS_IMPL("CDirectSoundBuffer_GetVoiceProperties", DsGetVoiceProperties), DS_IMPL("CDirectSoundStream_GetVoiceProperties", DsGetVoiceProperties),
    DS_IMPL("CDirectSoundBuffer_Set3DVoiceData", DsSet3DVoiceData), DS_IMPL("CDirectSoundStream_Set3DVoiceData", DsSet3DVoiceData),
    DS_IMPL("CDirectSoundBuffer_Use3DVoiceData", DsUse3DVoiceData), DS_IMPL("CDirectSoundStream_Use3DVoiceData", DsUse3DVoiceData),
    DS_IMPL("CDirectSoundStream_Process", DsStreamProcess),
    DS_IMPL("CDirectSoundStream_GetStatus__r2", DsStreamGetStatus),
    DS_IMPL("CDirectSoundStream_Pause", DsStreamPause),
    DS_IMPL("CDirectSoundStream_Flush", DsStreamFlush),
    DS_IMPL("CDirectSoundStream_FlushEx", DsStreamFlushEx),
    DS_IMPL("CDirectSoundStream_Discontinuity", DsStreamDiscontinuity),
    DS_IMPL("CDirectSoundStream_GetInfo", DsStreamGetInfo),
};

void InstallDsound() {
    static uint8_t* stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeof kDsoundEntries / sizeof kDsoundEntries[0] * 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    size_t implemented = 0, count = sizeof kDsoundEntries / sizeof kDsoundEntries[0];
    for (size_t i = 0; i < count; ++i) {
        const DsoundEntry& e = kDsoundEntries[i];
        if (Sha256Hex(At<uint8_t>(e.address), 16) != e.sha256) Stop(247, "DirectSound entry %s at %08x does not match its recorded bytes", e.name, e.address);
        const void* target = nullptr;
        for (const auto& impl : kDsoundImpls) if (!strcmp(impl.name, e.name)) target = impl.function;
        if (!target) {
            uint8_t* st = stubs + i * 16;
            uint32_t index = static_cast<uint32_t>(i);
            st[0] = 0x68; memcpy(st + 1, &index, 4);
            int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&UnimplementedDsound) - reinterpret_cast<uintptr_t>(st + 10));
            st[5] = 0xE9; memcpy(st + 6, &rel, 4);
            target = st;
        } else ++implemented;
        uint8_t* at = At<uint8_t>(e.address);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(at + 5));
        at[0] = 0xE9; memcpy(at + 1, &rel, 4);
    }
    Log("DirectSound: %zu entry points replaced (%zu implemented, others stop by name)", count, implemented);
    InstallXactTrace();
}

// Untraced 10-second summary: live DirectSound objects by kind (the 3D MIXIN
// count must stay bounded once XACT reclaims finished sound sources).
void AudioLiveCounts(uint32_t& streams, uint32_t& buffers, uint32_t& mixins) {
    streams = buffers = mixins = 0;
    EnterCriticalSection(&g_ds_lock);
    for (const auto& item : g_ds_objects) { const DsVoice* v = item.second; if (item.first != v->object) continue;
        if (v->kind == DsVoice::Submix) ++mixins; else if (v->kind == DsVoice::Stream) ++streams; else ++buffers; }
    LeaveCriticalSection(&g_ds_lock);
}
void AudioDiagnostics() {
    if (!g_xaudio) return;
    Log("audio: %llu packets completed", g_completed_total);
    EnterCriticalSection(&g_ds_lock);
    for (auto& o : g_ds_objects) {
        DsVoice* v = o.second;
        if (o.first != v->object) continue;
        Log("audio object %08x kind %d flags %#x fmt tag %#x %u ch %u Hz bytes %u vol %d headroom %d plays %llu packets %llu queued %zu playing %d out %08x bins %zu 3d %d",
            v->object, v->kind, v->flags, v->format.wFormatTag, v->format.nChannels, v->format.nSamplesPerSec, v->bytes, v->volume, v->headroom,
            v->stat_plays, v->stat_packets, v->queue.size(), v->playing, v->output ? v->output->object : 0, v->mixbins.size(), v->use_3d_voice_data);
        if (v->voice) { XAUDIO2_VOICE_STATE st{}; v->voice->GetState(&st);
            Log("  xaudio2: queued %u samples played %llu paused %d stream_started %d", st.BuffersQueued, st.SamplesPlayed, v->paused, v->stream_started);
            for (auto& pk : v->queue) Log("  packet buffer %08x size %u status %08x", pk->buffer, pk->size, pk->info.status_ptr ? *At<uint32_t>(pk->info.status_ptr) : 0); }
    }
    LeaveCriticalSection(&g_ds_lock);
}
