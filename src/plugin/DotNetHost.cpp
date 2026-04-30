#include "plugin/DotNetHost.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// ── Minimal hostfxr types (from Microsoft's hostfxr.h, stable C interface) ────

using hostfxr_handle = void*;

using hostfxr_initialize_fn = int(*)(
    const wchar_t* runtime_config_path,
    const void*    parameters,
    hostfxr_handle* host_context_handle);

using hostfxr_get_delegate_fn = int(*)(
    hostfxr_handle host_context_handle,
    int            type,
    void**         delegate);

using hostfxr_close_fn = int(*)(hostfxr_handle host_context_handle);

// get_runtime_delegate type id for load_assembly_and_get_function_pointer
constexpr int kHdtLoadAssembly = 5;

using load_assembly_fn = int(*)(
    const wchar_t* assembly_path,
    const wchar_t* type_name,
    const wchar_t* method_name,
    const wchar_t* delegate_type_name,   // (wchar_t*)-1 for UnmanagedCallersOnly
    void*          reserved,
    void**         delegate_);

// Sentinel for [UnmanagedCallersOnly] methods — (wchar_t*)-1 per hostfxr contract
static const wchar_t* const kUnmanagedCallersOnly =
    reinterpret_cast<const wchar_t*>(static_cast<intptr_t>(-1));

// ── Bridge function pointer types ─────────────────────────────────────────────

using HkxToXmlFn  = int  (*)(const char* path, char** outXml, int* outLen);
using FreeBufferFn = void (*)(void* ptr);
using LastErrorFn  = int  (*)(char* buf, int bufLen);

// Plugin bridge (SctBridge.PluginBridge)
using PluginLoadFn      = int  (*)(const char* pluginPath, const char* dataFolder);
using LoadOrderLoadFn   = int  (*)(const char* dataFolder);
using PluginUnloadFn    = void (*)();
using NpcSearchFn       = int  (*)(const char* query, int maxResults, char** outJson, int* outLen);
using NpcGetFn          = int  (*)(unsigned int formId, char** outJson, int* outLen);
using ProjectNewFn      = int  (*)(const char* modName);
using ProjectLoadFn     = int  (*)(const char* path);
using ProjectSaveFn     = int  (*)(const char* path);
using NpcCreateFn       = int  (*)(const char* inJson, char** outJson, int* outLen);
using CellSearchFn          = int  (*)(const char* query, int maxResults, char** outJson, int* outLen);
using CellGetRefsFn         = int  (*)(const char* formKey, char** outJson, int* outLen);
using WorldspaceSearchFn    = int  (*)(const char* query, int maxResults, char** outJson, int* outLen);
using ExteriorCellGetRefsFn = int  (*)(const char* wsFormKey, int cellX, int cellY, char** outJson, int* outLen);
using LandGetDataFn                  = int  (*)(const char* wsFormKey, int cellX, int cellY, char** outJson, int* outLen);
using WorldspaceGetTerrainBulkFn     = int  (*)(const char* wsFormKey, uint8_t** outData, int* outLen);

// ── Module-level state ────────────────────────────────────────────────────────

static bool         s_ready       = false;
static HkxToXmlFn   s_hkxToXml   = nullptr;
static FreeBufferFn s_freeBuffer  = nullptr;
static LastErrorFn  s_lastError   = nullptr;

static bool             s_pluginReady     = false;
static PluginLoadFn     s_pluginLoad      = nullptr;
static LoadOrderLoadFn  s_loadOrderLoad   = nullptr;
static PluginUnloadFn   s_pluginUnload    = nullptr;
static NpcSearchFn    s_pluginNpcSearch = nullptr;
static NpcGetFn       s_pluginNpcGet    = nullptr;
static ProjectNewFn   s_projectNew      = nullptr;
static ProjectLoadFn  s_projectLoad     = nullptr;
static ProjectSaveFn  s_projectSave     = nullptr;
static NpcCreateFn    s_pluginNpcCreate = nullptr;
static CellSearchFn          s_cellSearch          = nullptr;
static CellGetRefsFn         s_cellGetRefs         = nullptr;
static WorldspaceSearchFn    s_worldspaceSearch    = nullptr;
static ExteriorCellGetRefsFn s_exteriorCellGetRefs = nullptr;
static LandGetDataFn                 s_landGetData                = nullptr;
static WorldspaceGetTerrainBulkFn    s_worldspaceGetTerrainBulk   = nullptr;
static LastErrorFn                   s_pluginLastError            = nullptr;

// ── Path helpers ──────────────────────────────────────────────────────────────

// Find the highest-versioned hostfxr.dll under <dotnetRoot>\host\fxr\.
static std::wstring FindHostfxrUnder(const std::wstring& dotnetRoot)
{
    auto fxrDir = dotnetRoot + L"\\host\\fxr";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((fxrDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};

    std::tuple<int,int,int> best{0,0,0};
    std::wstring bestVer;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        int ma = 0, mi = 0, pa = 0;
        if (swscanf_s(fd.cFileName, L"%d.%d.%d", &ma, &mi, &pa) == 3) {
            auto v = std::make_tuple(ma, mi, pa);
            if (v > best) { best = v; bestVer = fd.cFileName; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (bestVer.empty()) return {};
    auto path = fxrDir + L"\\" + bestVer + L"\\hostfxr.dll";
    return (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) ? path : L"";
}

static std::wstring FindHostfxr()
{
    wchar_t buf[MAX_PATH];

    // 1. DOTNET_ROOT env var
    if (GetEnvironmentVariableW(L"DOTNET_ROOT", buf, MAX_PATH)) {
        auto p = FindHostfxrUnder(buf);
        if (!p.empty()) return p;
    }

    // 2. %ProgramFiles%\dotnet (typical system-wide install)
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) {
        auto p = FindHostfxrUnder(std::wstring(buf) + L"\\dotnet");
        if (!p.empty()) return p;
    }

    // 3. %LOCALAPPDATA%\Microsoft\dotnet (per-user install)
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) {
        auto p = FindHostfxrUnder(std::wstring(buf) + L"\\Microsoft\\dotnet");
        if (!p.empty()) return p;
    }

    return {};
}

// Find SctBridge.dll by probing relative to the running executable.
// Build layout: exe is in bin/Debug/ or bin/Release/, bridge is copied to
// the same directory by the CMake post-build step.
static std::wstring FindBridgeDll()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring dir = exePath;
    dir = dir.substr(0, dir.rfind(L'\\'));

    // Probe: same dir, then up to 2 parent dirs (covers build tree variations)
    for (int i = 0; i < 3; ++i) {
        auto p = dir + L"\\SctBridge.dll";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
        auto slash = dir.rfind(L'\\');
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    return {};
}

// ── Public API ────────────────────────────────────────────────────────────────

bool DotNetHost::Init(char* errOut, int errLen)
{
    // Find hostfxr.dll
    auto hostfxrPath = FindHostfxr();
    if (hostfxrPath.empty()) {
        std::snprintf(errOut, errLen,
            ".NET 10 runtime not found — HKX import unavailable. "
            "Install .NET 10 or set DOTNET_ROOT.");
        return false;
    }

    HMODULE hostfxr = LoadLibraryW(hostfxrPath.c_str());
    if (!hostfxr) {
        std::snprintf(errOut, errLen, "Failed to load hostfxr.dll");
        return false;
    }

    auto initFn = reinterpret_cast<hostfxr_initialize_fn>(
        GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config"));
    auto getDelegateFn = reinterpret_cast<hostfxr_get_delegate_fn>(
        GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate"));
    auto closeFn = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(hostfxr, "hostfxr_close"));

    if (!initFn || !getDelegateFn || !closeFn) {
        std::snprintf(errOut, errLen, "hostfxr API not found in loaded DLL");
        return false;
    }

    // Find SctBridge.dll
    auto bridgePath = FindBridgeDll();
    if (bridgePath.empty()) {
        std::snprintf(errOut, errLen,
            "SctBridge.dll not found — HKX import unavailable. "
            "Build the sct-bridge project and ensure the DLL is next to the exe.");
        return false;
    }

    // SctBridge.runtimeconfig.json is always next to SctBridge.dll
    auto rcPath = bridgePath.substr(0, bridgePath.rfind(L'.')) + L".runtimeconfig.json";

    // Initialize the .NET runtime
    hostfxr_handle ctx = nullptr;
    int rc = initFn(rcPath.c_str(), nullptr, &ctx);
    if (rc != 0 || !ctx) {
        std::snprintf(errOut, errLen,
            "hostfxr_initialize_for_runtime_config failed (0x%x). "
            "Ensure SctBridge.runtimeconfig.json is next to SctBridge.dll.", rc);
        return false;
    }

    load_assembly_fn loadFn = nullptr;
    rc = getDelegateFn(ctx, kHdtLoadAssembly, reinterpret_cast<void**>(&loadFn));
    closeFn(ctx);

    if (rc != 0 || !loadFn) {
        std::snprintf(errOut, errLen,
            "hostfxr_get_runtime_delegate failed (0x%x)", rc);
        return false;
    }

    // Load bridge entry points
    constexpr auto kType = L"SctBridge.Bridge, SctBridge";
    auto load = [&](const wchar_t* method, void** fn) -> bool {
        int r = loadFn(bridgePath.c_str(), kType, method,
                       kUnmanagedCallersOnly, nullptr, fn);
        if (r != 0 || !*fn) {
            fprintf(stderr, "[SCT] load_assembly_and_get_function_pointer('%ls') failed: 0x%x\n",
                    method, static_cast<unsigned>(r));
            fflush(stderr);
        }
        return r == 0 && *fn != nullptr;
    };

    if (!load(L"HkxToXml",   reinterpret_cast<void**>(&s_hkxToXml))  ||
        !load(L"FreeBuffer",  reinterpret_cast<void**>(&s_freeBuffer)) ||
        !load(L"LastError",   reinterpret_cast<void**>(&s_lastError)))
    {
        std::snprintf(errOut, errLen, "Failed to load one or more SctBridge entry points "
                      "(see stderr / Output window for HRESULT)");
        return false;
    }

    s_ready = true;

    // Attempt to load plugin bridge (Mutagen backend) — non-fatal if it fails.
    constexpr auto kPluginType = L"SctBridge.PluginBridge, SctBridge";
    auto loadPlugin = [&](const wchar_t* method, void** fn) -> bool {
        int r = loadFn(bridgePath.c_str(), kPluginType, method,
                       kUnmanagedCallersOnly, nullptr, fn);
        return r == 0 && *fn != nullptr;
    };

    s_pluginReady =
        loadPlugin(L"PluginLoad",      reinterpret_cast<void**>(&s_pluginLoad))      &&
        loadPlugin(L"PluginUnload",    reinterpret_cast<void**>(&s_pluginUnload))    &&
        loadPlugin(L"NpcSearch",       reinterpret_cast<void**>(&s_pluginNpcSearch)) &&
        loadPlugin(L"NpcGet",          reinterpret_cast<void**>(&s_pluginNpcGet))    &&
        loadPlugin(L"ProjectNew",      reinterpret_cast<void**>(&s_projectNew))      &&
        loadPlugin(L"ProjectLoad",     reinterpret_cast<void**>(&s_projectLoad))     &&
        loadPlugin(L"ProjectSave",     reinterpret_cast<void**>(&s_projectSave))     &&
        loadPlugin(L"NpcCreate",       reinterpret_cast<void**>(&s_pluginNpcCreate)) &&
        loadPlugin(L"LastError",       reinterpret_cast<void**>(&s_pluginLastError));

    // Optional — present in SctBridge builds that include load-order support.
    // Absence does not affect s_pluginReady; the wrapper returns -1 if null.
    loadPlugin(L"LoadOrderLoad", reinterpret_cast<void**>(&s_loadOrderLoad));

    // Optional — cell browsing support (added with previs work).
    loadPlugin(L"CellSearch",          reinterpret_cast<void**>(&s_cellSearch));
    loadPlugin(L"CellGetRefs",         reinterpret_cast<void**>(&s_cellGetRefs));
    loadPlugin(L"WorldspaceSearch",    reinterpret_cast<void**>(&s_worldspaceSearch));
    loadPlugin(L"ExteriorCellGetRefs", reinterpret_cast<void**>(&s_exteriorCellGetRefs));
    loadPlugin(L"LandGetData",                reinterpret_cast<void**>(&s_landGetData));
    loadPlugin(L"WorldspaceGetTerrainBulk",   reinterpret_cast<void**>(&s_worldspaceGetTerrainBulk));

    return true;
}

bool DotNetHost::Ready() { return s_ready; }

bool DotNetHost::HkxToXml(const char* hkxPath,
                            char** xmlOut, int* xmlLen,
                            char* errOut, int errLen)
{
    if (!s_ready || !s_hkxToXml) {
        std::snprintf(errOut, errLen, "DotNetHost not initialized");
        return false;
    }
    fprintf(stderr, "[SCT] HkxToXml('%s')...\n", hkxPath); fflush(stderr);
    int bridgeRet = s_hkxToXml(hkxPath, xmlOut, xmlLen);
    fprintf(stderr, "[SCT] HkxToXml → %d, len=%d\n", bridgeRet, (xmlLen ? *xmlLen : -1)); fflush(stderr);
    if (bridgeRet == 0)
        return true;

    // Retrieve the managed exception message
    if (s_lastError && errOut && errLen > 0)
        s_lastError(errOut, errLen);
    return false;
}

void DotNetHost::FreeBuffer(void* ptr)
{
    if (s_ready && s_freeBuffer && ptr)
        s_freeBuffer(ptr);
}

// ── Plugin bridge wrappers ────────────────────────────────────────────────────

bool DotNetHost::PluginReady() { return s_pluginReady; }

bool DotNetHost::PluginLoad(const char* pluginPath, const char* dataFolder,
                             char* errOut, int errLen)
{
    if (!s_pluginReady || !s_pluginLoad) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (s_pluginLoad(pluginPath, dataFolder) == 0) return true;
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

int DotNetHost::LoadOrderLoad(const char* dataFolder, char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return -1;
    }
    if (!s_loadOrderLoad) {
        std::snprintf(errOut, errLen,
            "LoadOrderLoad not available — rebuild SctBridge to get this feature");
        return -1;
    }
    int count = s_loadOrderLoad(dataFolder);
    if (count >= 0) return count;
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return -1;
}

void DotNetHost::PluginUnload()
{
    if (s_pluginUnload) s_pluginUnload();
}

bool DotNetHost::NpcSearch(const char* query, int maxResults,
                            std::string& outJson,
                            char* errOut, int errLen)
{
    if (!s_pluginReady || !s_pluginNpcSearch) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_pluginNpcSearch(query, maxResults, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::NpcGet(uint32_t formId, std::string& outJson,
                         char* errOut, int errLen)
{
    if (!s_pluginReady || !s_pluginNpcGet) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_pluginNpcGet(static_cast<unsigned int>(formId), &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::ProjectNew(const char* modName, char* errOut, int errLen)
{
    if (!s_pluginReady || !s_projectNew) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (s_projectNew(modName) == 0) return true;
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::ProjectLoad(const char* path, char* errOut, int errLen)
{
    if (!s_pluginReady || !s_projectLoad) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (s_projectLoad(path) == 0) return true;
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::ProjectSave(const char* path, char* errOut, int errLen)
{
    if (!s_pluginReady || !s_projectSave) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (s_projectSave(path) == 0) return true;
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::NpcCreate(const char* inJson, std::string& outJson,
                            char* errOut, int errLen)
{
    if (!s_pluginReady || !s_pluginNpcCreate) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_pluginNpcCreate(inJson, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::CellSearch(const char* query, int maxResults,
                             std::string& outJson,
                             char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_cellSearch) {
        std::snprintf(errOut, errLen,
            "CellSearch not available — rebuild SctBridge");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_cellSearch(query, maxResults, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::CellGetRefs(const char* formKey,
                              std::string& outJson,
                              char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_cellGetRefs) {
        std::snprintf(errOut, errLen,
            "CellGetRefs not available — rebuild SctBridge");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_cellGetRefs(formKey, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::WorldspaceSearch(const char* query, int maxResults,
                                   std::string& outJson,
                                   char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_worldspaceSearch) {
        std::snprintf(errOut, errLen,
            "WorldspaceSearch not available — rebuild SctBridge");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_worldspaceSearch(query, maxResults, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::ExteriorCellGetRefs(const char* worldspaceFormKey, int cellX, int cellY,
                                      std::string& outJson,
                                      char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_exteriorCellGetRefs) {
        std::snprintf(errOut, errLen,
            "ExteriorCellGetRefs not available — rebuild SctBridge");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_exteriorCellGetRefs(worldspaceFormKey, cellX, cellY, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::LandGetData(const char* worldspaceFormKey, int cellX, int cellY,
                              std::string& outJson,
                              char* errOut, int errLen)
{
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_landGetData) {
        std::snprintf(errOut, errLen,
            "LandGetData not available — rebuild SctBridge");
        return false;
    }
    char* buf = nullptr; int len = 0;
    if (s_landGetData(worldspaceFormKey, cellX, cellY, &buf, &len) == 0) {
        outJson.assign(buf, len);
        FreeBuffer(buf);
        return true;
    }
    if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
    return false;
}

bool DotNetHost::WorldspaceGetTerrainBulk(
    const char* wsFormKey,
    std::map<std::pair<int,int>, LandRecord>& outTiles,
    char* errOut, int errLen)
{
    outTiles.clear();
    if (!s_pluginReady) {
        std::snprintf(errOut, errLen, "plugin bridge not initialized");
        return false;
    }
    if (!s_worldspaceGetTerrainBulk) {
        std::snprintf(errOut, errLen,
            "WorldspaceGetTerrainBulk not available — rebuild SctBridge");
        return false;
    }

    uint8_t* buf = nullptr; int bLen = 0;
    if (s_worldspaceGetTerrainBulk(wsFormKey, &buf, &bLen) != 0) {
        if (s_pluginLastError && errOut && errLen > 0) s_pluginLastError(errOut, errLen);
        return false;
    }

    // ── Parse SLRT binary blob ─────────────────────────────────────────────────
    // magic(4) + version(4) + cellCount(4) + per-cell records
    bool ok = false;
    do {
        if (bLen < 12) break;
        const uint8_t* p   = buf;
        const uint8_t* end = buf + bLen;

        constexpr uint32_t kMagic   = 0x54524C53u; // "SLRT"
        constexpr uint32_t kVersion = 2u;
        uint32_t magic, version, cellCount;
        std::memcpy(&magic,     p,     4); p += 4;
        std::memcpy(&version,   p,     4); p += 4;
        std::memcpy(&cellCount, p,     4); p += 4;

        if (magic != kMagic) {
            std::snprintf(errOut, errLen, "WorldspaceGetTerrainBulk: bad magic 0x%08X", magic);
            break;
        }
        if (version != kVersion) {
            std::snprintf(errOut, errLen, "WorldspaceGetTerrainBulk: unsupported version %u", version);
            break;
        }

        constexpr int kHeightsBytes = 33 * 33 * sizeof(float); // 4356
        constexpr int kColorsBytes  = 33 * 33 * 3;             // 3267

        ok = true;
        for (uint32_t i = 0; i < cellCount; ++i) {
            // int16 cellX + int16 cellY
            if (p + 4 > end) { ok = false; break; }
            int16_t cx, cy;
            std::memcpy(&cx, p, 2); p += 2;
            std::memcpy(&cy, p, 2); p += 2;

            // float[1089] heights
            if (p + kHeightsBytes > end) { ok = false; break; }
            LandRecord land{};
            std::memcpy(land.heights, p, kHeightsBytes); p += kHeightsBytes;

            // uint8 hasColors
            if (p + 1 > end) { ok = false; break; }
            land.hasColors = (*p++ != 0);

            if (land.hasColors) {
                if (p + kColorsBytes > end) { ok = false; break; }
                std::memcpy(land.colors, p, kColorsBytes); p += kColorsBytes;
            }

            // Base texture: uint16 pathLen + [char[] path + float tileRate]
            if (p + 2 > end) { ok = false; break; }
            uint16_t pathLen = 0;
            std::memcpy(&pathLen, p, 2); p += 2;
            if (pathLen > 0) {
                if (p + pathLen + 4 > end) { ok = false; break; }
                land.baseTexPath.assign(reinterpret_cast<const char*>(p), pathLen);
                p += pathLen;
                std::memcpy(&land.texTileRate, p, 4); p += 4;
            }

            // Alpha layers: uint8 count + per-layer data
            if (p + 1 > end) { ok = false; break; }
            const uint8_t alphaCount = *p++;
            constexpr int kBlendBytes = 33 * 33 * 4; // float[1089]
            for (uint8_t ai = 0; ai < alphaCount && ok; ++ai) {
                if (p + 2 > end) { ok = false; break; }
                uint16_t aPathLen = 0;
                std::memcpy(&aPathLen, p, 2); p += 2;
                if (p + aPathLen + 4 + kBlendBytes > end) { ok = false; break; }
                TerrainAlphaLayer layer;
                layer.path.assign(reinterpret_cast<const char*>(p), aPathLen);
                p += aPathLen;
                std::memcpy(&layer.tileRate, p, 4); p += 4;
                std::memcpy(layer.blendMap.data(), p, kBlendBytes); p += kBlendBytes;
                land.alphaLayers.push_back(std::move(layer));
            }

            outTiles[{(int)cx, (int)cy}] = std::move(land);
        }
    } while (false);

    FreeBuffer(buf);
    return ok;
}
