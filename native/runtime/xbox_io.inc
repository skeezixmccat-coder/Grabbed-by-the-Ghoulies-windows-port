// Device and file layer (included by ghoulies_runtime.cpp).
//
// Xbox object names are ANSI (12-byte OBJECT_ATTRIBUTES: RootDirectory,
// PANSI_STRING ObjectName, Attributes). Supported namespaces:
//   \Device\CdRom0[\path]               read-only XDVDFS image (the ISO)
//   \Device\Harddisk0\partitionN[\path] confined host directory
//       <project>\native-storage\native-run\hdd0-partitionN
//   \??\X:[\path]                        drive links (IoCreateSymbolicLink)
// Host files are opened with Windows NtCreateFile, which shares the Xbox
// kernel's NT file semantics, dispositions, options and status codes; IO
// status blocks have the same layout and are passed through. Reparse points
// are never followed. Every unsupported name, class or option stops the run.

constexpr int32_t kStatusInvalidDeviceRequest = static_cast<int32_t>(0xC0000010);
constexpr int32_t kStatusEndOfFile = static_cast<int32_t>(0xC0000011);
constexpr int32_t kStatusObjectNameInvalid = static_cast<int32_t>(0xC0000033);
constexpr int32_t kStatusObjectNameNotFound = static_cast<int32_t>(0xC0000034);
constexpr int32_t kStatusObjectPathNotFound = static_cast<int32_t>(0xC000003A);
constexpr int32_t kStatusMediaWriteProtected = static_cast<int32_t>(0xC00000A2);
constexpr int32_t kStatusNotADirectory = static_cast<int32_t>(0xC0000103);
constexpr int32_t kStatusFileIsADirectory = static_cast<int32_t>(0xC00000BA);

constexpr int64_t kPartition0Bytes = 1 << 20;
// Declared retail Xbox drive layout (byte offset and length of partitions 1-5).
struct PartitionLayout { uint64_t start, length; };
constexpr PartitionLayout kPartitions[6] = {
    {0, 0}, {0xABE80000ull, 0x1312D6000ull}, {0x8CA80000ull, 0x1F400000ull},
    {0x00080000ull, 0x2EE00000ull}, {0x2EE80000ull, 0x2EE00000ull}, {0x5DC80000ull, 0x2EE00000ull},
};

struct XAnsiString { uint16_t length, maximum; uint32_t buffer; };
struct XObjectAttributes { uint32_t root; uint32_t name; uint32_t attributes; };
struct XIoStatus { int32_t status; uint32_t information; };

std::string AnsiOf(uint32_t ansi_string) {
    if (!ansi_string) Stop(120, "null ANSI_STRING");
    auto* s = At<XAnsiString>(ansi_string);
    return std::string(At<const char>(s->buffer), s->length);
}

// ---- ntdll file API ----
struct WUnicodeString { USHORT Length, MaximumLength; PWSTR Buffer; };
struct WObjectAttributes { ULONG Length; HANDLE RootDirectory; WUnicodeString* ObjectName; ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService; };
using NtCreateFileFn = LONG (NTAPI*)(HANDLE*, ACCESS_MASK, WObjectAttributes*, void* iosb, LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtReadWriteFileFn = LONG (NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, void* iosb, PVOID, ULONG, LARGE_INTEGER*, PULONG);
using NtInfoFileFn = LONG (NTAPI*)(HANDLE, void* iosb, PVOID, ULONG, ULONG);
using NtFlushFn = LONG (NTAPI*)(HANDLE, void* iosb);
NtCreateFileFn pNtCreateFile; NtReadWriteFileFn pNtReadFile, pNtWriteFile;
NtInfoFileFn pNtQueryInformationFile, pNtSetInformationFile, pNtQueryVolumeInformationFile;
NtFlushFn pNtFlushBuffersFile;

// ---- Handles ----
enum class XKind { Thread, CdRomDevice, DiscFile, DiscDirectory, HostFile, HostDirectory, SymbolicLink, RawPartition0, RawVolume, Event, Mutant };
struct XObject {
    XKind kind; HANDLE host = nullptr; std::string name;   // name: normalized Xbox path
    uint64_t disc_offset = 0, size = 0, position = 0;      // disc objects
    uint32_t partition = 0;
    bool formatted = false;                                 // raw volume: boot sector written
    uint8_t disc_attributes = 0x10;                         // XDVDFS attribute byte (FILE_ATTRIBUTE_* values)
};
std::unordered_map<uint32_t, XObject> g_handles;
uint32_t g_next_handle = 0x1000;
CRITICAL_SECTION g_handle_lock;

uint32_t NewHandle(XObject o) {
    EnterCriticalSection(&g_handle_lock);
    uint32_t h = g_next_handle; g_next_handle += 4;
    g_handles.emplace(h, std::move(o));
    LeaveCriticalSection(&g_handle_lock);
    return h;
}
XObject* Lookup(uint32_t h, const char* caller) {
    EnterCriticalSection(&g_handle_lock);
    auto found = g_handles.find(h);
    XObject* o = found == g_handles.end() ? nullptr : &found->second;   // node-stable
    LeaveCriticalSection(&g_handle_lock);
    if (!o) Stop(121, "%s(%08x): not a handle this runtime issued", caller, h);
    return o;
}

void NtCloseObject(uint32_t handle);

// ---- Drive links ----
std::unordered_map<std::string, std::string> g_links;   // upper-case "\??\D:" -> target
CRITICAL_SECTION g_link_lock;
std::string Upper(std::string s) { for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return s; }

// ---- XDVDFS ----
HANDLE g_iso = INVALID_HANDLE_VALUE;
uint32_t g_root_sector = 0, g_root_size = 0;
uint64_t g_disc_time = 0;   // XDVDFS descriptor FILETIME
CRITICAL_SECTION g_iso_lock;

void IsoRead(uint64_t offset, void* buffer, uint32_t length) {
    EnterCriticalSection(&g_iso_lock);
    LARGE_INTEGER at; at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD got = 0;
    if (!SetFilePointerEx(g_iso, at, nullptr, FILE_BEGIN) || !ReadFile(g_iso, buffer, length, &got, nullptr) || got != length)
        Stop(122, "ISO read failed at %llx (+%u)", offset, length);
    LeaveCriticalSection(&g_iso_lock);
}

void OpenIso() {
    wchar_t iso_path[32768] = {};
    DWORD n = GetEnvironmentVariableW(L"GH_ISO_PATH", iso_path, _countof(iso_path));
    if (!n || n >= _countof(iso_path)) Stop(123, "GH_ISO_PATH must name your supported game-partition ISO");
    g_iso = CreateFileW(iso_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (g_iso == INVALID_HANDLE_VALUE) Stop(123, "cannot open the ISO (error %lu)", GetLastError());
    uint8_t descriptor[2048];
    IsoRead(32 * 2048, descriptor, sizeof descriptor);
    if (memcmp(descriptor, "MICROSOFT*XBOX*MEDIA", 20) || memcmp(descriptor + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20))
        Stop(124, "ISO has no XDVDFS descriptor at sector 32");
    memcpy(&g_root_sector, descriptor + 20, 4); memcpy(&g_root_size, descriptor + 24, 4); memcpy(&g_disc_time, descriptor + 28, 8);
    Log("disc: XDVDFS root sector %u, size %u", g_root_sector, g_root_size);
}

struct DiscEntry { bool found = false; bool directory = false; uint32_t sector = 0, size = 0; uint8_t attributes = 0x10; };

// Look up one name in a directory extent (binary tree of entries; searched
// exhaustively, case-insensitively).
DiscEntry DiscLookup(uint32_t dir_sector, uint32_t dir_size, const std::string& name) {
    std::vector<uint8_t> dir(dir_size);
    if (dir_size) IsoRead(static_cast<uint64_t>(dir_sector) * 2048, dir.data(), dir_size);
    std::vector<uint32_t> stack{0};
    std::string want = Upper(name);
    while (!stack.empty()) {
        uint32_t off = stack.back(); stack.pop_back();
        if (off + 14 > dir_size) continue;
        uint16_t left, right; uint32_t sector, size;
        memcpy(&left, &dir[off], 2); memcpy(&right, &dir[off + 2], 2);
        if (left == 0xFFFF) continue;   // padding
        memcpy(&sector, &dir[off + 4], 4); memcpy(&size, &dir[off + 8], 4);
        uint8_t attributes = dir[off + 12], length = dir[off + 13];
        if (off + 14 + length > dir_size) continue;
        std::string entry(reinterpret_cast<const char*>(&dir[off + 14]), length);
        if (Upper(entry) == want) return {true, (attributes & 0x10) != 0, sector, size, attributes};
        if (left) stack.push_back(left * 4u);
        if (right) stack.push_back(right * 4u);
    }
    return {};
}

std::vector<std::string> SplitPath(const std::string& path, bool& valid) {
    std::vector<std::string> parts; std::string cur; valid = true;
    for (char c : path) {
        if (c == '\\') { if (!cur.empty()) parts.push_back(cur); cur.clear(); continue; }
        if (c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20) valid = false;
        cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    for (const auto& p : parts) if (p == "." || p == "..") valid = false;
    return parts;
}

// ---- Name resolution ----
struct Resolved { enum { CdRom, Harddisk, Invalid } where = Invalid; uint32_t partition = 0; std::string rest; std::string full; };

Resolved ResolveName(uint32_t root_handle, std::string name) {
    if (root_handle == 0xFFFFFFFDu) {   // ObDosDevicesDirectory(): names are relative to \??
        name = "\\??\\" + name;
        root_handle = 0;
    }
    if (root_handle) {
        XObject* root = Lookup(root_handle, "object root");
        if (root->kind != XKind::DiscDirectory && root->kind != XKind::HostDirectory && root->kind != XKind::CdRomDevice)
            Stop(125, "relative open against a non-directory handle");
        name = root->name + "\\" + name;
    }
    for (int depth = 0; depth < 8; ++depth) {
        std::string up = Upper(name);
        if (up.rfind("\\??\\", 0) == 0) {
            size_t end = up.find('\\', 4);
            std::string link = up.substr(0, end == std::string::npos ? up.size() : end);
            EnterCriticalSection(&g_link_lock);
            auto found = g_links.find(link);
            std::string target = found == g_links.end() ? std::string() : found->second;
            LeaveCriticalSection(&g_link_lock);
            if (target.empty()) return {Resolved::Invalid, 0, "", name};
            name = target + name.substr(link.size());
            continue;
        }
        Resolved r; r.full = name;
        if (up.rfind("\\DEVICE\\CDROM0", 0) == 0 && (up.size() == 14 || up[14] == '\\')) {
            r.where = Resolved::CdRom; r.rest = name.substr(14); return r;
        }
        if (up.rfind("\\DEVICE\\HARDDISK0\\PARTITION", 0) == 0 && up.size() >= 28 && up[27] >= '0' && up[27] <= '9' &&
            (up.size() == 28 || up[28] == '\\')) {
            r.where = Resolved::Harddisk; r.partition = static_cast<uint32_t>(up[27] - '0'); r.rest = name.substr(28); return r;
        }
        return {Resolved::Invalid, 0, "", name};
    }
    Stop(126, "drive link loop resolving %s", name.c_str());
}

std::wstring g_storage_root;   // \??\<project>\native-storage\native-run

std::wstring PartitionRoot(uint32_t partition) {
    return g_storage_root + L"\\hdd0-partition" + std::to_wstring(partition);
}

void EnsureStorageRoots() {
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dll, MAX_PATH);
    std::wstring project(dll);
    for (int i = 0; i < 3; ++i) project = project.substr(0, project.find_last_of(L'\\'));   // ...\build\native\x.dll -> project
    // GH_STORAGE_NAME selects a sibling storage folder (hidden tests use their own,
    // so scripted runs never write the user's saves). Default: native-run.
    std::wstring name = L"native-run";
    { wchar_t v[64] = {}; DWORD n = GetEnvironmentVariableW(L"GH_STORAGE_NAME", v, 64);
      if (n) { if (n >= 64) Stop(127, "GH_STORAGE_NAME too long");
               for (DWORD i = 0; i < n; ++i) if (!iswalnum(v[i]) && v[i] != L'-') Stop(127, "GH_STORAGE_NAME must be letters, digits or '-'");
               name = v; } }
    std::wstring root = project + L"\\native-storage\\" + name;
    CreateDirectoryW((project + L"\\native-storage").c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    for (uint32_t p = 1; p <= 5; ++p) CreateDirectoryW((root + L"\\hdd0-partition" + std::to_wstring(p)).c_str(), nullptr);
    DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        Stop(127, "storage root is not a plain directory");
    g_storage_root = L"\\??\\" + root;
    Log("storage: native-storage\\%ls (partitions 1-5)", name.c_str());   // relative: logs carry no host paths
}

void WriteIosb(uint32_t iosb, int32_t status, uint32_t information) {
    if (iosb) { auto* io = At<XIoStatus>(iosb); io->status = status; io->information = information; }
}

// ---- Open/create ----
int32_t CreateOrOpen(const char* caller, uint32_t* handle_out, uint32_t access, uint32_t object_attributes, uint32_t iosb,
                     LARGE_INTEGER* allocation, uint32_t file_attributes, uint32_t share, uint32_t disposition, uint32_t options) {
    if (!handle_out || !object_attributes) Stop(130, "%s: null handle or attributes", caller);
    auto* oa = At<XObjectAttributes>(object_attributes);
    std::string name = AnsiOf(oa->name);
    Resolved r = ResolveName(oa->root, name);
    Log("%s(\"%s\" -> \"%s\", access %#x, share %#x, disposition %u, options %#x)", caller, name.c_str(), r.full.c_str(), access, share, disposition, options);
    constexpr uint32_t kSupportedOptions = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x800 | 0x4000 | 0x800000;
    if (options & ~kSupportedOptions) Stop(131, "%s: unsupported create options %#x", caller, options);
    if (r.where == Resolved::Invalid) {
        Log("  -> name not resolvable in the provided namespace");
        Stop(132, "%s: object name \"%s\" is outside the provided namespace", caller, r.full.c_str());
    }
    bool valid = true;
    std::vector<std::string> parts = SplitPath(r.rest, valid);
    if (!valid) { WriteIosb(iosb, kStatusObjectNameInvalid, 0); return kStatusObjectNameInvalid; }
    bool want_directory = (options & 0x1) != 0, want_file = (options & 0x40) != 0;

    if (r.where == Resolved::CdRom) {
        constexpr uint32_t kWriteAccess = 0x2 | 0x4 | 0x10 | 0x100 | 0x10000 | 0x40000000;   // data/append/EA/attributes/delete/generic write
        if ((access & kWriteAccess) || (disposition != 1 && disposition != 3)) { WriteIosb(iosb, kStatusMediaWriteProtected, 0); return kStatusMediaWriteProtected; }
        XObject o; o.name = r.full;
        if (parts.empty()) {
            if (r.rest.empty()) { o.kind = XKind::CdRomDevice; }
            else { o.kind = XKind::DiscDirectory; o.disc_offset = static_cast<uint64_t>(g_root_sector) * 2048; o.size = g_root_size; }
        } else {
            uint32_t sector = g_root_sector, size = g_root_size; DiscEntry e{true, true, sector, size};
            for (size_t i = 0; i < parts.size(); ++i) {
                if (!e.directory) { WriteIosb(iosb, kStatusObjectPathNotFound, 0); return kStatusObjectPathNotFound; }
                e = DiscLookup(e.sector, e.size, parts[i]);
                if (!e.found) {
                    int32_t st = i + 1 == parts.size() ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
                    Log("  -> disc: not found (%#x)", static_cast<uint32_t>(st));
                    WriteIosb(iosb, st, 0); return st;
                }
            }
            if (e.directory && want_file) { WriteIosb(iosb, kStatusFileIsADirectory, 0); return kStatusFileIsADirectory; }
            if (!e.directory && want_directory) { WriteIosb(iosb, kStatusNotADirectory, 0); return kStatusNotADirectory; }
            o.kind = e.directory ? XKind::DiscDirectory : XKind::DiscFile;
            o.disc_attributes = e.attributes;
            o.disc_offset = static_cast<uint64_t>(e.sector) * 2048; o.size = e.size;
        }
        *handle_out = NewHandle(o);
        WriteIosb(iosb, kStatusSuccess, 1);   // FILE_OPENED
        Log("  -> disc %s, handle %08x", o.kind == XKind::DiscFile ? "file" : o.kind == XKind::CdRomDevice ? "device" : "directory", *handle_out);
        return kStatusSuccess;
    }

    // Raw partition0 (whole-disk sectors): a virtual 1 MiB disk prefix backed
    // by <storage>\hdd0-partition0.bin, initially blank (as after a format).
    // The title reads/writes its utility-cache table at offset 0x800.
    if (r.partition == 0) {
        if (!parts.empty()) Stop(136, "%s: paths below partition0 are not provided", caller);
        std::wstring file = g_storage_root.substr(4) + L"\\hdd0-partition0.bin";
        HANDLE host = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (host == INVALID_HANDLE_VALUE) Stop(137, "%s: cannot open the partition0 backing file (error %lu)", caller, GetLastError());
        LARGE_INTEGER size{}; GetFileSizeEx(host, &size);
        if (size.QuadPart == 0) { LARGE_INTEGER end; end.QuadPart = kPartition0Bytes; SetFilePointerEx(host, end, nullptr, FILE_BEGIN); SetEndOfFile(host); }
        else if (size.QuadPart != kPartition0Bytes) { CloseHandle(host); Stop(138, "%s: partition0 backing file has an unexpected size", caller); }
        XObject o; o.kind = XKind::RawPartition0; o.host = host; o.name = r.full; o.size = kPartition0Bytes;
        *handle_out = NewHandle(o);
        WriteIosb(iosb, kStatusSuccess, 1);
        Log("  -> virtual raw partition0, handle %08x", *handle_out);
        return kStatusSuccess;
    }
    // Raw volume (\Device\Harddisk0\PartitionN with no trailing separator):
    // sectors kept in a sparse backing file of the declared partition length.
    // Used by the title to format a utility-cache partition; the partition's
    // files remain in its host directory.
    if (r.partition >= 1 && r.partition <= 5 && r.rest.empty()) {
        std::wstring file = g_storage_root.substr(4) + L"\\hdd0-partition" + std::to_wstring(r.partition) + L".volume.bin";
        HANDLE host = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (host == INVALID_HANDLE_VALUE) Stop(137, "%s: cannot open volume backing file (error %lu)", caller, GetLastError());
        DWORD bytes = 0;
        if (!DeviceIoControl(host, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr)) { CloseHandle(host); Stop(137, "%s: backing file cannot be sparse", caller); }
        LARGE_INTEGER end; end.QuadPart = static_cast<LONGLONG>(kPartitions[r.partition].length);
        SetFilePointerEx(host, end, nullptr, FILE_BEGIN); SetEndOfFile(host);
        XObject o; o.kind = XKind::RawVolume; o.host = host; o.name = r.full; o.partition = r.partition; o.size = kPartitions[r.partition].length;
        *handle_out = NewHandle(o);
        WriteIosb(iosb, kStatusSuccess, 1);
        Log("  -> raw volume partition%u (%#llx bytes, sparse), handle %08x", r.partition, o.size, *handle_out);
        return kStatusSuccess;
    }
    // Harddisk partition: confined host path.
    if (r.partition < 1 || r.partition > 5) Stop(133, "%s: harddisk partition %u not provided", caller, r.partition);
    std::wstring path = PartitionRoot(r.partition);
    for (const auto& p : parts) {
        int n = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, p.c_str(), static_cast<int>(p.size()), nullptr, 0);
        if (n <= 0) { WriteIosb(iosb, kStatusObjectNameInvalid, 0); return kStatusObjectNameInvalid; }
        std::wstring w(n, L'\0'); MultiByteToWideChar(CP_ACP, 0, p.c_str(), static_cast<int>(p.size()), w.data(), n);
        path += L"\\" + w;
    }
    if (parts.empty()) path += L"\\";
    WUnicodeString us{static_cast<USHORT>(path.size() * 2), static_cast<USHORT>(path.size() * 2), path.data()};
    WObjectAttributes woa{sizeof(WObjectAttributes), nullptr, &us, 0x40 /* OBJ_CASE_INSENSITIVE */, nullptr, nullptr};
    HANDLE host = nullptr;
    // FILE_READ_ATTRIBUTES is added host-side so confinement can be verified.
    LONG st = pNtCreateFile(&host, access | FILE_READ_ATTRIBUTES, &woa, At<void>(iosb), allocation, file_attributes, share, disposition,
                            options | 0x200000 /* FILE_OPEN_REPARSE_POINT */, nullptr, 0);
    Log("  -> host status %#lx, information %u", static_cast<unsigned long>(st), iosb ? At<XIoStatus>(iosb)->information : 0);
    if (st < 0) return st;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(host, FileAttributeTagInfo, &tag, sizeof tag)) { DWORD e = GetLastError(); CloseHandle(host); Stop(134, "%s: cannot verify host object (error %lu)", caller, e); }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { CloseHandle(host); Stop(135, "%s: host object is a reparse point", caller); }
    XObject o; o.kind = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? XKind::HostDirectory : XKind::HostFile;
    o.host = host; o.name = r.full; o.partition = r.partition;
    *handle_out = NewHandle(o);
    Log("  -> host %s, handle %08x", o.kind == XKind::HostFile ? "file" : "directory", *handle_out);
    return st;
}

// 190 NtCreateFile
int32_t __stdcall NtCreateFile(uint32_t* handle, uint32_t access, uint32_t object_attributes, uint32_t iosb, LARGE_INTEGER* allocation,
                               uint32_t file_attributes, uint32_t share, uint32_t disposition, uint32_t options) {
    return CreateOrOpen("NtCreateFile", handle, access, object_attributes, iosb, allocation, file_attributes, share, disposition, options);
}

// 202 NtOpenFile
int32_t __stdcall NtOpenFile(uint32_t* handle, uint32_t access, uint32_t object_attributes, uint32_t iosb, uint32_t share, uint32_t options) {
    return CreateOrOpen("NtOpenFile", handle, access, object_attributes, iosb, nullptr, 0, share, 1 /* FILE_OPEN */, options);
}

extern "C" void Nv2aOwnedFlushGuest(uint32_t va, uint32_t size);
extern "C" void Nv2aGlMarkGuestWritten(uint32_t va, uint32_t size);
int32_t RawPartitionTransfer(XObject* o, bool write, uint32_t iosb, uint32_t buffer, uint32_t length, LARGE_INTEGER* offset) {
    Nv2aOwnedFlushGuest(buffer, length);   // the OS cannot fault into renderer-owned pages
    uint64_t at = (!offset || offset->QuadPart == -2) ? o->position : static_cast<uint64_t>(offset->QuadPart);
    if ((at % 512) || (length % 512) || at + length > o->size) Stop(139, "raw %s %s at %llx (+%u) outside the provided sectors", o->name.c_str(), write ? "write" : "read", at, length);
    LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(at);
    DWORD done = 0;
    BOOL ok = SetFilePointerEx(o->host, pos, nullptr, FILE_BEGIN) &&
              (write ? WriteFile(o->host, At<void>(buffer), length, &done, nullptr) : ReadFile(o->host, At<void>(buffer), length, &done, nullptr));
    if (!ok || done != length) Stop(139, "raw partition0 host transfer failed");
    if (write && o->kind == XKind::RawVolume && at == 0) o->formatted = true;
    if (!write) Nv2aGlMarkGuestWritten(buffer, length);
    o->position = at + length;
    Log("raw %s %s at %#llx, %u bytes", o->name.c_str(), write ? "write" : "read", at, length);
    WriteIosb(iosb, kStatusSuccess, length);
    return kStatusSuccess;
}

// 219 NtReadFile(handle, event, apc, apc_context, iosb, buffer, length, offset*)
int32_t __stdcall NtReadFile(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb,
                             uint32_t buffer, uint32_t length, LARGE_INTEGER* offset) {
    if (event || apc) Stop(140, "NtReadFile: asynchronous reads not provided");
    XObject* o = Lookup(handle, "NtReadFile");
    Nv2aOwnedFlushGuest(buffer, length);   // the OS cannot fault into renderer-owned pages
    if (o->kind == XKind::HostFile) {
        LONG st = pNtReadFile(o->host, nullptr, nullptr, nullptr, At<void>(iosb), At<void>(buffer), length, offset, nullptr);
        if (st >= 0) Nv2aGlMarkGuestWritten(buffer, length);
        // Completion-status evidence for the intermittent startup investigation.
        // Preserve NT's status and completion behavior; do not invent success.
        static std::atomic<unsigned> logged_nonzero{0};
        if (st != 0 && logged_nonzero.fetch_add(1) < 32) Log("host read status %#lx: %s length %#x offset %#llx iosb %#x", static_cast<unsigned long>(st),
            o->name.c_str(), length, offset ? offset->QuadPart : -2ll, iosb);
        return st;
    }
    if (o->kind == XKind::RawPartition0 || o->kind == XKind::RawVolume) return RawPartitionTransfer(o, false, iosb, buffer, length, offset);
    if (o->kind != XKind::DiscFile && o->kind != XKind::CdRomDevice) Stop(141, "NtReadFile on %s", o->name.c_str());
    uint64_t at = (!offset || offset->QuadPart == -2) ? o->position : static_cast<uint64_t>(offset->QuadPart);
    if (o->kind == XKind::CdRomDevice) {
        if ((at % 2048) || (length % 2048)) Stop(142, "NtReadFile: unaligned raw device read");
        IsoRead(at, At<void>(buffer), length);
        Nv2aGlMarkGuestWritten(buffer, length);
        o->position = at + length; WriteIosb(iosb, kStatusSuccess, length); return kStatusSuccess;
    }
    if (at >= o->size) { WriteIosb(iosb, kStatusEndOfFile, 0); return kStatusEndOfFile; }
    uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(length, o->size - at));
    IsoRead(o->disc_offset + at, At<void>(buffer), n);
    Nv2aGlMarkGuestWritten(buffer, n);
    o->position = at + n;
    WriteIosb(iosb, kStatusSuccess, n);
    return kStatusSuccess;
}

// 236 NtWriteFile(handle, event, apc, apc_context, iosb, buffer, length, offset*)
int32_t __stdcall NtWriteFile(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb,
                              uint32_t buffer, uint32_t length, LARGE_INTEGER* offset) {
    if (event || apc) Stop(143, "NtWriteFile: asynchronous writes not provided");
    XObject* o = Lookup(handle, "NtWriteFile");
    Nv2aOwnedFlushGuest(buffer, length);
    if (o->kind == XKind::RawPartition0 || o->kind == XKind::RawVolume) return RawPartitionTransfer(o, true, iosb, buffer, length, offset);
    if (o->kind != XKind::HostFile) { WriteIosb(iosb, kStatusMediaWriteProtected, 0); return kStatusMediaWriteProtected; }
    return pNtWriteFile(o->host, nullptr, nullptr, nullptr, At<void>(iosb), At<void>(buffer), length, offset, nullptr);
}

// 198 NtFlushBuffersFile
int32_t __stdcall NtFlushBuffersFile(uint32_t handle, uint32_t iosb) {
    XObject* o = Lookup(handle, "NtFlushBuffersFile");
    if (o->kind == XKind::RawPartition0 || o->kind == XKind::RawVolume) { FlushFileBuffers(o->host); WriteIosb(iosb, kStatusSuccess, 0); return kStatusSuccess; }
    if (o->kind != XKind::HostFile) Stop(144, "NtFlushBuffersFile on %s", o->name.c_str());
    return pNtFlushBuffersFile(o->host, At<void>(iosb));
}

// 211 NtQueryInformationFile / 226 NtSetInformationFile. Classes whose Xbox
// layout equals NT's are passed through for host files; disc files are
// answered from the XDVDFS entry.
bool PlainInformationClass(uint32_t c) {
    return c == 4 /* Basic */ || c == 5 /* Standard */ || c == 13 /* Disposition */ || c == 14 /* Position */ ||
           c == 19 /* Allocation */ || c == 20 /* EndOfFile */ || c == 34 /* NetworkOpen */;
}

int32_t __stdcall NtQueryInformationFile(uint32_t handle, uint32_t iosb, uint32_t info, uint32_t length, uint32_t info_class) {
    XObject* o = Lookup(handle, "NtQueryInformationFile");
    if (!PlainInformationClass(info_class)) Stop(145, "NtQueryInformationFile class %u not provided (%s)", info_class, o->name.c_str());
    if (o->kind == XKind::HostFile || o->kind == XKind::HostDirectory)
        return pNtQueryInformationFile(o->host, At<void>(iosb), At<void>(info), length, info_class);
    if (o->kind == XKind::DiscFile && info_class == 14 && length >= 8) {
        *At<uint64_t>(info) = o->position; WriteIosb(iosb, kStatusSuccess, 8); return kStatusSuccess;
    }
    if ((o->kind == XKind::DiscFile || o->kind == XKind::DiscDirectory) && (info_class == 34 || info_class == 4)) {
        // FileNetworkOpenInformation (0x38) / FileBasicInformation (0x28): disc times, sizes, XDVDFS attributes.
        uint32_t need = info_class == 34 ? 0x38 : 0x28;
        if (length < need) Stop(149, "NtQueryInformationFile class %u: buffer %u too small", info_class, length);
        auto* p = At<uint8_t>(info);
        uint32_t attributes = o->kind == XKind::DiscFile ? (o->disc_attributes ? o->disc_attributes : 0x80) : 0x10;
        for (int i = 0; i < 4; ++i) memcpy(p + 8 * i, &g_disc_time, 8);
        if (info_class == 34) {
            uint64_t alloc = (o->size + 2047) & ~2047ull, eof = o->size;
            memcpy(p + 32, &alloc, 8); memcpy(p + 40, &eof, 8); memcpy(p + 48, &attributes, 4); memset(p + 52, 0, 4);
        } else { memcpy(p + 32, &attributes, 4); memset(p + 36, 0, 4); }
        WriteIosb(iosb, kStatusSuccess, need); return kStatusSuccess;
    }
    if (o->kind == XKind::DiscFile && info_class == 5 && length >= 24) {   // AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
        auto* p = At<uint8_t>(info);
        uint64_t alloc = (o->size + 2047) & ~2047ull, eof = o->size; uint32_t links = 1;
        memcpy(p, &alloc, 8); memcpy(p + 8, &eof, 8); memcpy(p + 16, &links, 4); p[20] = 0; p[21] = 0;
        WriteIosb(iosb, kStatusSuccess, 24); return kStatusSuccess;
    }
    Stop(146, "NtQueryInformationFile class %u on %s not provided", info_class, o->name.c_str());
}

int32_t __stdcall NtSetInformationFile(uint32_t handle, uint32_t iosb, uint32_t info, uint32_t length, uint32_t info_class) {
    XObject* o = Lookup(handle, "NtSetInformationFile");
    if (!PlainInformationClass(info_class)) Stop(147, "NtSetInformationFile class %u not provided (%s)", info_class, o->name.c_str());
    if (o->kind == XKind::HostFile || o->kind == XKind::HostDirectory)
        return pNtSetInformationFile(o->host, At<void>(iosb), At<void>(info), length, info_class);
    if (o->kind == XKind::DiscFile && info_class == 14 && length >= 8) {
        o->position = *At<uint64_t>(info); WriteIosb(iosb, kStatusSuccess, 0); return kStatusSuccess;
    }
    Stop(148, "NtSetInformationFile class %u on %s not provided", info_class, o->name.c_str());
}

// 218 NtQueryVolumeInformationFile. Class 3 (FileFsSizeInformation) for HDD
// partitions. Allocation unit: if the title formatted the partition (its raw
// volume begins with a FATX superblock: "FATX", volume id, sectors per
// cluster at +8), the formatted cluster size; otherwise the standard 16 KiB
// (32 x 512). Counts: host free/total space in those units, total capped at
// the declared partition length.
int32_t __stdcall NtQueryVolumeInformationFile(uint32_t handle, uint32_t iosb, uint32_t info, uint32_t length, uint32_t info_class) {
    XObject* o = Lookup(handle, "NtQueryVolumeInformationFile");
    if (info_class == 3 && length >= 24 && (o->kind == XKind::DiscDirectory || o->kind == XKind::DiscFile || o->kind == XKind::CdRomDevice)) {
        // Disc: 2048-byte sectors, one per allocation unit, image size in sectors, nothing free.
        LARGE_INTEGER size{}; GetFileSizeEx(g_iso, &size);
        int64_t total = size.QuadPart / 2048, available = 0; uint32_t sectors = 1, bytes = 2048;
        auto* p = At<uint8_t>(info);
        memcpy(p, &total, 8); memcpy(p + 8, &available, 8); memcpy(p + 16, &sectors, 4); memcpy(p + 20, &bytes, 4);
        WriteIosb(iosb, kStatusSuccess, 24);
        return kStatusSuccess;
    }
    if (info_class != 3 || length < 24 || o->kind != XKind::HostDirectory)
        Stop(150, "NtQueryVolumeInformationFile class %u on %s not provided", info_class, o->name.c_str());
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    std::wstring dir = PartitionRoot(o->partition).substr(4) + L"\\";
    if (!GetDiskFreeSpaceExW(dir.c_str(), &free_bytes, &total_bytes, nullptr)) Stop(151, "cannot query host free space");
    uint32_t sectors = 32, bytes = 512;
    std::wstring volume = g_storage_root.substr(4) + L"\\hdd0-partition" + std::to_wstring(o->partition) + L".volume.bin";
    HANDLE v = CreateFileW(volume.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (v != INVALID_HANDLE_VALUE) {
        uint8_t super[512]; DWORD got = 0;
        if (ReadFile(v, super, sizeof super, &got, nullptr) && got == sizeof super && !memcmp(super, "FATX", 4)) {
            memcpy(&sectors, super + 8, 4);
            if (!sectors || sectors > 1024) Stop(159, "partition%u FATX superblock has %u sectors per cluster", o->partition, sectors);
        }
        CloseHandle(v);
    }
    uint64_t unit = static_cast<uint64_t>(sectors) * bytes;
    uint64_t partition_bytes = kPartitions[o->partition].length;
    auto* p = At<uint8_t>(info);
    int64_t total = static_cast<int64_t>(std::min<uint64_t>(total_bytes.QuadPart, partition_bytes) / unit);
    int64_t available = static_cast<int64_t>(std::min<uint64_t>(free_bytes.QuadPart, partition_bytes) / unit);
    memcpy(p, &total, 8); memcpy(p + 8, &available, 8); memcpy(p + 16, &sectors, 4); memcpy(p + 20, &bytes, 4);
    WriteIosb(iosb, kStatusSuccess, 24);
    Log("NtQueryVolumeInformationFile(%s): %lld of %lld units of %llu bytes free", o->name.c_str(), available, total, unit);
    return kStatusSuccess;
}

// 196 NtDeviceIoControlFile. Only the title's recognised disc probe: SCSI
// pass-through direct (0x4D014) MODE SENSE(10) page 0x3E. A disc image has
// no physical DVD mode page or authentication, so it is rejected with
// STATUS_INVALID_DEVICE_REQUEST (same response as the lifted-trace backend;
// the original caller continues on this failure).
int32_t __stdcall NtDeviceIoControlFile(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb,
                                        uint32_t code, uint32_t input, uint32_t input_length, uint32_t output, uint32_t output_length) {
    XObject* o = Lookup(handle, "NtDeviceIoControlFile");
    if (event || apc) Stop(152, "NtDeviceIoControlFile: asynchronous requests not provided");
    if (o->kind == XKind::CdRomDevice && code == 0x4D014 && input_length == 44 && !output) {
        const uint8_t* cdb = At<uint8_t>(input + 0x1C);   // SCSI_PASS_THROUGH_DIRECT.Cdb on Xbox (after 0x1C header bytes)
        Log("NtDeviceIoControlFile(CdRom0, SCSI pass-through, CDB %02x %02x %02x)", cdb[0], cdb[1], cdb[2]);
        if (cdb[0] == 0x5A && cdb[2] == 0x3E) { WriteIosb(iosb, kStatusInvalidDeviceRequest, 0); return kStatusInvalidDeviceRequest; }
    }
    if (o->kind == XKind::RawVolume && code == 0x70000 && output && output_length >= 24) {   // IOCTL_DISK_GET_DRIVE_GEOMETRY
        auto* g = At<uint8_t>(output);
        uint64_t cylinders = o->size / 512; uint32_t media = 12 /* FixedMedia */, tracks = 1, sectors = 1, bytes = 512;
        memcpy(g, &cylinders, 8); memcpy(g + 8, &media, 4); memcpy(g + 12, &tracks, 4); memcpy(g + 16, &sectors, 4); memcpy(g + 20, &bytes, 4);
        WriteIosb(iosb, kStatusSuccess, 24);
        Log("NtDeviceIoControlFile(%s, GET_DRIVE_GEOMETRY): %llu sectors of 512", o->name.c_str(), cylinders);
        return kStatusSuccess;
    }
    if (o->kind == XKind::RawVolume && code == 0x74004 && output && output_length >= 32) {   // IOCTL_DISK_GET_PARTITION_INFO
        auto* p = At<uint8_t>(output);
        uint64_t start = kPartitions[o->partition].start, length = o->size; uint32_t hidden = 0, number = o->partition;
        memset(p, 0, 32); memcpy(p, &start, 8); memcpy(p + 8, &length, 8); memcpy(p + 16, &hidden, 4); memcpy(p + 20, &number, 4);
        p[24] = 0; /* PartitionType */ p[25] = 0; p[26] = 1; /* RecognizedPartition */ p[27] = 0;
        WriteIosb(iosb, kStatusSuccess, 32);
        Log("NtDeviceIoControlFile(%s, GET_PARTITION_INFO): start %#llx length %#llx", o->name.c_str(), start, length);
        return kStatusSuccess;
    }
    Stop(153, "NtDeviceIoControlFile(%s, code %#x, in %u, out %u) not provided", o->name.c_str(), code, input_length, output_length);
}

// Empty a directory tree inside the project's storage root (no reparse
// points are followed; links themselves are removed, never their targets).
void ClearDirectory(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring path = dir + L"\\" + n;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (is_dir && !is_link) { ClearDirectory(path); RemoveDirectoryW(path.c_str()); }
        else if (is_dir) RemoveDirectoryW(path.c_str());
        else { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(path.c_str()); }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// 200 NtFsControlFile. Only FSCTL_DISMOUNT_VOLUME (0x90020) on a raw volume.
// After a format (boot sector written) the partition's host directory is
// emptied, which is the state a freshly formatted FATX volume mounts in.
int32_t __stdcall NtFsControlFile(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb,
                                  uint32_t code, uint32_t input, uint32_t input_length, uint32_t output, uint32_t output_length) {
    XObject* o = Lookup(handle, "NtFsControlFile");
    if (event || apc || code != 0x90020 || o->kind != XKind::RawVolume)
        Stop(157, "NtFsControlFile(%s, code %#x) not provided", o->name.c_str(), code);
    if (o->formatted) {
        std::wstring dir = PartitionRoot(o->partition).substr(4);
        if (dir.rfind(g_storage_root.substr(4), 0) != 0) Stop(158, "format target outside storage root");
        ClearDirectory(dir);
        Log("NtFsControlFile(%s, DISMOUNT_VOLUME): formatted; host directory emptied", o->name.c_str());
    } else {
        Log("NtFsControlFile(%s, DISMOUNT_VOLUME)", o->name.c_str());
    }
    WriteIosb(iosb, kStatusSuccess, 0);
    return kStatusSuccess;
}

// 210 NtQueryFullAttributesFile(object_attributes, FILE_NETWORK_OPEN_INFORMATION*)
// (four FILETIMEs, AllocationSize, EndOfFile, FileAttributes; 0x38 bytes).
// Resolved through the same open path; disc entries report the disc's
// descriptor time and their XDVDFS attribute byte.
int32_t __stdcall NtQueryFullAttributesFile(uint32_t object_attributes, uint32_t info) {
    uint32_t handle = 0;
    XIoStatus io{};
    int32_t st = CreateOrOpen("NtQueryFullAttributesFile", &handle, 0x80 /* FILE_READ_ATTRIBUTES */, object_attributes,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&io)), nullptr, 0, 7, 1 /* FILE_OPEN */, 0);
    if (st < 0) return st;
    XObject* o = Lookup(handle, "NtQueryFullAttributesFile");
    auto* p = At<uint8_t>(info);
    if (o->kind == XKind::HostFile || o->kind == XKind::HostDirectory) {
        uint8_t buffer[0x38]; XIoStatus qio{};
        st = pNtQueryInformationFile(o->host, &qio, buffer, sizeof buffer, 34 /* FileNetworkOpenInformation */);
        if (st >= 0) memcpy(p, buffer, 0x38);
    } else {
        uint64_t alloc = (o->size + 2047) & ~2047ull, eof = o->size;
        uint32_t attributes = o->kind == XKind::DiscFile ? (o->disc_attributes ? o->disc_attributes : 0x80) : 0x10;
        if (o->kind == XKind::CdRomDevice) { alloc = eof = 0; }
        for (int i = 0; i < 4; ++i) memcpy(p + 8 * i, &g_disc_time, 8);
        memcpy(p + 32, &alloc, 8); memcpy(p + 40, &eof, 8); memcpy(p + 48, &attributes, 4); memset(p + 52, 0, 4);
        st = kStatusSuccess;
    }
    NtCloseObject(handle);
    return st;
}

// 207 NtQueryDirectoryFile(handle, event, apc, apc_context, iosb, info, length, class, mask (ANSI_STRING*), restart)
// Class 1 (FileDirectoryInformation), host directories. Xbox entries:
// NextEntryOffset, FileIndex, four FILETIMEs, EndOfFile, AllocationSize,
// FileAttributes, FileNameLength, ANSI FileName. One entry per call.
using NtQueryDirectoryFileFn = LONG (NTAPI*)(HANDLE, HANDLE, void*, void*, void*, void*, ULONG, ULONG, BOOLEAN, WUnicodeString*, BOOLEAN);
NtQueryDirectoryFileFn pNtQueryDirectoryFile;
int32_t __stdcall NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb,
                                       uint32_t info, uint32_t length, uint32_t info_class, uint32_t mask, uint32_t restart) {
    XObject* o = Lookup(handle, "NtQueryDirectoryFile");
    if (event || apc || info_class != 1) Stop(165, "NtQueryDirectoryFile(class %u, event %#x) not provided", info_class, event);
    if (o->kind != XKind::HostDirectory) Stop(166, "NtQueryDirectoryFile on %s not provided", o->name.c_str());
    std::wstring wmask; WUnicodeString us{}; WUnicodeString* pmask = nullptr;
    if (mask) {
        std::string m = AnsiOf(mask);
        wmask.assign(m.begin(), m.end());
        us = {static_cast<USHORT>(wmask.size() * 2), static_cast<USHORT>(wmask.size() * 2), wmask.data()};
        pmask = &us;
    }
    alignas(8) uint8_t buffer[0x800];
    XIoStatus io{};
    LONG st = pNtQueryDirectoryFile(o->host, nullptr, nullptr, nullptr, &io, buffer, sizeof buffer, 1, TRUE, pmask, (restart & 0xFF) ? TRUE : FALSE);
    if (st < 0 || st == static_cast<LONG>(0x80000006)) { WriteIosb(iosb, st, 0); return st; }
    // Windows FILE_DIRECTORY_INFORMATION: same fixed fields (0x40 bytes), then WCHAR name.
    uint32_t name_bytes; memcpy(&name_bytes, buffer + 0x3C, 4);
    std::wstring wname(reinterpret_cast<wchar_t*>(buffer + 0x40), name_bytes / 2);
    std::string name(wname.size(), '?');
    for (size_t i = 0; i < wname.size(); ++i) name[i] = wname[i] < 0x80 ? static_cast<char>(wname[i]) : '?';
    uint32_t need = 0x40 + static_cast<uint32_t>(name.size());
    if (length < need) { WriteIosb(iosb, static_cast<int32_t>(0x80000005), 0); return static_cast<int32_t>(0x80000005); }   // STATUS_BUFFER_OVERFLOW
    auto* p = At<uint8_t>(info);
    memcpy(p, buffer, 0x3C);
    uint32_t zero = 0, n = static_cast<uint32_t>(name.size());
    memcpy(p, &zero, 4);                 // NextEntryOffset: single entry
    memcpy(p + 0x3C, &n, 4);
    memcpy(p + 0x40, name.data(), n);
    WriteIosb(iosb, kStatusSuccess, need);
    return kStatusSuccess;
}

// 67 IoCreateSymbolicLink(PANSI_STRING link, PANSI_STRING target)
int32_t __stdcall IoCreateSymbolicLink(uint32_t link, uint32_t target) {
    std::string l = Upper(AnsiOf(link)), t = AnsiOf(target);
    if (l.rfind("\\??\\", 0) != 0 || l.size() != 6 || l[5] != ':') Stop(154, "IoCreateSymbolicLink(%s): only \\??\\X: drive links provided", l.c_str());
    EnterCriticalSection(&g_link_lock);
    bool exists = g_links.count(l) != 0;
    if (!exists) g_links[l] = t;
    LeaveCriticalSection(&g_link_lock);
    Log("IoCreateSymbolicLink(%s -> %s)%s", l.c_str(), t.c_str(), exists ? ": already exists" : "");
    return exists ? static_cast<int32_t>(0xC0000035) /* STATUS_OBJECT_NAME_COLLISION */ : kStatusSuccess;
}

// 69 IoDeleteSymbolicLink(PANSI_STRING link)
int32_t __stdcall IoDeleteSymbolicLink(uint32_t link) {
    std::string l = Upper(AnsiOf(link));
    EnterCriticalSection(&g_link_lock);
    size_t erased = g_links.erase(l);
    LeaveCriticalSection(&g_link_lock);
    Log("IoDeleteSymbolicLink(%s)%s", l.c_str(), erased ? "" : ": not found");
    return erased ? kStatusSuccess : kStatusObjectNameNotFound;
}

// 203 NtOpenSymbolicLinkObject(handle*, object_attributes)
int32_t __stdcall NtOpenSymbolicLinkObject(uint32_t* handle, uint32_t object_attributes) {
    auto* oa = At<XObjectAttributes>(object_attributes);
    if (oa->root) Stop(155, "NtOpenSymbolicLinkObject: relative names not provided");
    std::string l = Upper(AnsiOf(oa->name));
    EnterCriticalSection(&g_link_lock);
    auto found = g_links.find(l);
    std::string target = found == g_links.end() ? std::string() : found->second;
    LeaveCriticalSection(&g_link_lock);
    Log("NtOpenSymbolicLinkObject(%s)%s", l.c_str(), target.empty() ? ": not found" : "");
    if (target.empty()) return kStatusObjectNameNotFound;
    XObject o; o.kind = XKind::SymbolicLink; o.name = target;
    *handle = NewHandle(o);
    return kStatusSuccess;
}

// 215 NtQuerySymbolicLinkObject(handle, PANSI_STRING target, PULONG returned_length)
int32_t __stdcall NtQuerySymbolicLinkObject(uint32_t handle, uint32_t target, uint32_t* returned_length) {
    XObject* o = Lookup(handle, "NtQuerySymbolicLinkObject");
    if (o->kind != XKind::SymbolicLink) Stop(156, "NtQuerySymbolicLinkObject on a non-link handle");
    auto* s = At<XAnsiString>(target);
    uint32_t needed = static_cast<uint32_t>(o->name.size());
    if (returned_length) *returned_length = needed + 1;
    if (s->maximum < needed) return static_cast<int32_t>(0xC0000023);   // STATUS_BUFFER_TOO_SMALL
    memcpy(At<char>(s->buffer), o->name.data(), needed);
    s->length = static_cast<uint16_t>(needed);
    return kStatusSuccess;
}

void NtCloseObject(uint32_t handle) {
    XObject* o = Lookup(handle, "close");
    if (o->host) CloseHandle(o->host);
    EnterCriticalSection(&g_handle_lock);
    g_handles.erase(handle);
    LeaveCriticalSection(&g_handle_lock);
}

void InitializeIo() {
    InitializeCriticalSection(&g_handle_lock);
    InitializeCriticalSection(&g_link_lock);
    InitializeCriticalSection(&g_iso_lock);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtCreateFile = reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
    pNtReadFile = reinterpret_cast<NtReadWriteFileFn>(GetProcAddress(ntdll, "NtReadFile"));
    pNtWriteFile = reinterpret_cast<NtReadWriteFileFn>(GetProcAddress(ntdll, "NtWriteFile"));
    pNtQueryInformationFile = reinterpret_cast<NtInfoFileFn>(GetProcAddress(ntdll, "NtQueryInformationFile"));
    pNtSetInformationFile = reinterpret_cast<NtInfoFileFn>(GetProcAddress(ntdll, "NtSetInformationFile"));
    pNtQueryVolumeInformationFile = reinterpret_cast<NtInfoFileFn>(GetProcAddress(ntdll, "NtQueryVolumeInformationFile"));
    pNtFlushBuffersFile = reinterpret_cast<NtFlushFn>(GetProcAddress(ntdll, "NtFlushBuffersFile"));
    pNtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFileFn>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
    if (!pNtCreateFile || !pNtReadFile || !pNtWriteFile || !pNtQueryInformationFile || !pNtSetInformationFile || !pNtFlushBuffersFile)
        Stop(128, "ntdll file API missing");
    OpenIso();
    EnsureStorageRoots();
}
