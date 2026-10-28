/*
 * Dirt1_Core_Bloom_Fix - Colin McRae: DiRT (EGO 1.0) fix for modern CPUs / Windows
 * https://github.com/ZUKMAN/Dirt1_Core_Bloom_Fix
 *
 * SPDX-License-Identifier: MIT
 *
 * Loaded as dinput8.dll proxy (DiRT.exe imports DINPUT8!DirectInput8Create).
 * Before the game's entry point runs, the import table of DiRT.exe is patched.
 *
 * What the original engine does (DiRT.exe 1.2):
 *   - number of CPUs      = GetSystemInfo().dwNumberOfProcessors
 *   - worker/main thread  = SetThreadAffinityMask(1 << (core % N)) + SetThreadIdealProcessor
 *     "core" comes from system/workerMap*.xml, i.e. logical CPU index, so on SMT/hybrid CPUs
 *     high priority spin-waiting workers end up on HT siblings and E-cores.
 *   - hardware_settings_info "cores" = N, halved for any Intel CPU with the CPUID HTT bit
 *     (set on every modern Intel CPU), hardware_settings_restrictions.xml then picks
 *     workerMap.xml / 2Core / 4Core / 8Core by that number.
 *   - workerMap8Core.xml references undefined workers "view_manager_2/3" and is the only map
 *     that puts workers into the "solver"/"cloth" groups -> NULL deref at 0x962470 on race start.
 *   - VRAM comes from IDirectDraw7::GetAvailableVidMem (DWORD) and is divided as signed int,
 *     so >= 2 GB VRAM becomes a negative value.
 *
 * What DiRT 2 (EGO 1.5) does differently and what is ported here:
 *   - CPU topology detection: physical cores and their SMT siblings, limited to the
 *     process affinity mask (DiRT 2 enumerates APIC IDs, we use
 *     GetLogicalProcessorInformationEx which also knows P/E cores).
 *   - A table that maps the engine's "core N" index onto real logical CPUs
 *     instead of using 1 << N.
 *   - The main thread only gets an ideal processor, no hard affinity.
 *
 * Additionally:
 *   - The engine is limited to 4 worker cores: every system/workerMap*.xml request is
 *     redirected to the game's own workerMap4Core.xml (2Core / workerMap.xml on smaller CPUs),
 *     so hardware_settings_restrictions.xml / config edits are not needed.
 *   - VRAM reported by DirectDraw is clamped (default 1024 MB).
 *   - postprocess/effects.xml and tracks/<stage>/effects.xml are rewritten on load (bloom, orange
 *     tint, vignette, brightness and any <Param> from the ini), the way the
 *     "Colin McRae DIRT - Graphics Mod" does it by hand, without touching game files.
 *   - D3D9 display mode list is filtered. Every mode becomes a node in
 *     hardware_settings_info.xml, which lives in the fixed size HARDWARE_SETTINGS pool
 *     (system/applicationMemoryMap.xml). Modern Windows reports dozens of modes
 *     (DSR/VSR/scaled resolutions x 60..240 Hz), the pool runs out, the allocator
 *     returns NULL and the game crashes at startup at 0x5999BE (vector copy in the
 *     resolution matching code at 0x748280).
 */

#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>

#if defined(_MSC_VER)
#define RETURN_ADDRESS() _ReturnAddress()
#else
#define RETURN_ADDRESS() __builtin_return_address(0)
#endif

// --------------------------------------------------------------------------------------------
// Settings (Dirt1_Core_Bloom_Fix.ini next to the dll)
// --------------------------------------------------------------------------------------------

static struct {
    int affinity = 1;        // 0 = no pinning (OS decides), 1 = remap to best cores, 2 = original
    int workerMap = 1;       // 1 = redirect to the 4/2/1 core map, 0 = use game's selection
    int mainThreadSoft = 1;  // 1 = main thread: ideal processor only (as DiRT 2)
    int maxVideoMemoryMB = 1024; // 0 = no clamp
    int filterModes = 1;     // 1 = filter D3D9 display modes
    int maxResolutions = 12; // max distinct resolutions reported
    int minWidth = 800;      // smaller resolutions are dropped
    int dpiAware = 1;
    int log = 1;
    int postProcess = 1;     // 1 = rewrite postprocess/tracks effects.xml on load
} g_cfg;

// Post process parameter overrides: <Param name="X"> inside <Effect chain="TypeN">.
struct ParamOverride {
    char name[48];
    char value[48];
    int chain;   // 2 = tracks/<stage>/effects.xml, chain Type2; 5 = postprocess/effects.xml, chain Type5
};
static ParamOverride g_pp[64];
static int g_ppCount;
static char g_iniPath[MAX_PATH];

static void add_override(const char* name, const char* value, int chain)
{
    if (!value[0] || _stricmp(value, "keep") == 0 || g_ppCount >= 64) {
        return;
    }
    ParamOverride& o = g_pp[g_ppCount++];
    lstrcpynA(o.name, name, sizeof(o.name));
    lstrcpynA(o.value, value, sizeof(o.value));
    o.chain = chain;
}

static void add_amount(const char* key, const char* def, const char* param, int chain)
{
    char v[48];
    GetPrivateProfileStringA("PostProcess", key, def, v, sizeof(v), g_iniPath);
    if (!v[0] || _stricmp(v, "keep") == 0) {
        return;
    }
    char f[48];
    snprintf(f, sizeof(f), "%f", atof(v));
    add_override(param, f, chain);
}

static void load_postprocess_config()
{
    g_ppCount = 0;
    if (!g_cfg.postProcess) {
        return;
    }
    // In-game (tracks/*/effects.xml, chain Type2). Defaults follow "Colin McRae DIRT - Graphics Mod":
    // less bloom, no orange tint, no vignette, neutral brightness.
    add_amount("Bloom", "0.30", "bloomAmount", 2);
    add_amount("Vignette", "0", "vignetteAmount", 2);
    add_amount("Brightness", "0", "brighness", 2); // sic, that is the engine's name
    add_amount("Contrast", "keep", "contrast", 2);
    add_amount("Saturation", "keep", "colourAmount", 2);
    add_amount("Gamma", "keep", "gamma", 2);
    add_amount("MotionBlur", "keep", "blurLength", 2);

    char v[48];
    GetPrivateProfileStringA("PostProcess", "Tint", "0", v, sizeof(v), g_iniPath);
    if (v[0] && _stricmp(v, "keep") != 0 && atof(v) == 0.0) {
        add_override("tintColour", "255, 255, 255, 255", 2);
        add_override("tintAmount", "0.000000", 2);
    }

    // Front end (postprocess/effects.xml, FE_* effects, chain Type5).
    add_amount("MenuBloom", "0", "bloomAmount", 5);
    GetPrivateProfileStringA("PostProcess", "MenuTint", "0", v, sizeof(v), g_iniPath);
    if (v[0] && _stricmp(v, "keep") != 0 && atof(v) == 0.0) {
        add_override("tintAmount", "0.000000", 5);
    }
    add_amount("MenuVignette", "0", "vignetteAmount", 5);

    // Free form overrides for in-game effects: [PostProcessOverride] paramName=value
    char keys[4096];
    DWORD len = GetPrivateProfileSectionA("PostProcessOverride", keys, sizeof(keys), g_iniPath);
    for (char* line = keys; len && *line; line += strlen(line) + 1) {
        if (*line == ';') {
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        // later entries win, so remove an earlier rule for the same param/chain
        for (int i = 0; i < g_ppCount; ++i) {
            if (g_pp[i].chain == 2 && _stricmp(g_pp[i].name, line) == 0) {
                g_pp[i] = g_pp[--g_ppCount];
                --i;
            }
        }
        add_override(line, eq + 1, 2);
    }
}

static char g_dllDir[MAX_PATH];
static FILE* g_log;

static void logf(const char* fmt, ...)
{
    if (!g_log) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static void load_config()
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\Dirt1_Core_Bloom_Fix.ini", g_dllDir);
    lstrcpynA(g_iniPath, ini, MAX_PATH);
    g_cfg.affinity = GetPrivateProfileIntA("Threads", "Affinity", g_cfg.affinity, ini);
    g_cfg.workerMap = GetPrivateProfileIntA("Threads", "WorkerMap", g_cfg.workerMap, ini);
    g_cfg.mainThreadSoft = GetPrivateProfileIntA("Threads", "MainThreadSoftAffinity", g_cfg.mainThreadSoft, ini);
    g_cfg.maxVideoMemoryMB = GetPrivateProfileIntA("Video", "MaxVideoMemoryMB", g_cfg.maxVideoMemoryMB, ini);
    g_cfg.filterModes = GetPrivateProfileIntA("Video", "FilterModes", g_cfg.filterModes, ini);
    g_cfg.maxResolutions = GetPrivateProfileIntA("Video", "MaxResolutions", g_cfg.maxResolutions, ini);
    g_cfg.minWidth = GetPrivateProfileIntA("Video", "MinWidth", g_cfg.minWidth, ini);
    g_cfg.dpiAware = GetPrivateProfileIntA("Misc", "DPIAware", g_cfg.dpiAware, ini);
    g_cfg.log = GetPrivateProfileIntA("Misc", "Log", g_cfg.log, ini);
    g_cfg.postProcess = GetPrivateProfileIntA("PostProcess", "Enable", g_cfg.postProcess, ini);
}

static void open_log()
{
    if (!g_cfg.log) {
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\Dirt1_Core_Bloom_Fix.log", g_dllDir);
    g_log = fopen(path, "w");
    if (!g_log) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(path, sizeof(path), "%sDirt1_Core_Bloom_Fix.log", tmp);
        g_log = fopen(path, "w");
    }
}

// --------------------------------------------------------------------------------------------
// CPU topology
// --------------------------------------------------------------------------------------------

struct Core {
    BYTE efficiency;   // higher = faster (P-core)
    int logical[4];    // logical CPU indices (< 32) of this core, first = primary
    int count;
};

static Core g_cores[32];
static int g_coreCount;

static int g_map[32];        // engine core index -> logical CPU
static int g_mapCount;       // number of CPUs reported to the engine
static int g_realLogical;    // real logical CPU count (in process mask)
static DWORD g_processMask;

static int popcount32(DWORD v)
{
    int n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

static void detect_topology()
{
    DWORD_PTR processMask = 0, systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    g_processMask = DWORD(processMask);
    if (!g_processMask) {
        g_processMask = 1;
    }
    g_realLogical = popcount32(g_processMask);

    typedef BOOL(WINAPI * GLPIEX)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
    auto glpi = GLPIEX(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetLogicalProcessorInformationEx"));

    g_coreCount = 0;

    if (glpi) {
        DWORD size = 0;
        glpi(RelationProcessorCore, nullptr, &size);
        auto buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
        if (buffer && glpi(RelationProcessorCore, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX(buffer), &size)) {
            for (DWORD offset = 0; offset < size && g_coreCount < 32;) {
                auto info = PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX(buffer + offset);
                offset += info->Size;

                if (info->Relationship != RelationProcessorCore) {
                    continue;
                }

                // A 32-bit process only sees processor group 0 and CPUs 0..31.
                for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                    if (info->Processor.GroupMask[g].Group != 0) {
                        continue;
                    }
                    DWORD mask = DWORD(info->Processor.GroupMask[g].Mask) & g_processMask;
                    if (!mask) {
                        continue;
                    }
                    Core& core = g_cores[g_coreCount++];
                    core.efficiency = info->Processor.EfficiencyClass;
                    core.count = 0;
                    for (int bit = 0; bit < 32 && core.count < 4; ++bit) {
                        if (mask & (1u << bit)) {
                            core.logical[core.count++] = bit;
                        }
                    }
                }
            }
        }
        if (buffer) {
            HeapFree(GetProcessHeap(), 0, buffer);
        }
    }

    if (g_coreCount == 0) {
        // Fallback: every logical CPU is a core.
        for (int bit = 0; bit < 32; ++bit) {
            if (g_processMask & (1u << bit)) {
                Core& core = g_cores[g_coreCount++];
                core.efficiency = 0;
                core.logical[0] = bit;
                core.count = 1;
            }
        }
    }

    // Stable sort: fastest cores first (P-cores before E-cores), keep OS order otherwise.
    for (int i = 1; i < g_coreCount; ++i) {
        Core c = g_cores[i];
        int j = i - 1;
        while (j >= 0 && g_cores[j].efficiency < c.efficiency) {
            g_cores[j + 1] = g_cores[j];
            --j;
        }
        g_cores[j + 1] = c;
    }

    BYTE best = g_cores[0].efficiency;
    int fastCores = 0;
    for (int i = 0; i < g_coreCount; ++i) {
        if (g_cores[i].efficiency == best) {
            ++fastCores;
        }
    }

    // Number of worker cores: the 4 core map at most. The 8 core map is the only one that
    // puts workers into the physics "solver"/"cloth" groups, which the engine does not
    // support (crash on race start), and 4 workers are plenty for this game.
    int k = fastCores >= 4 ? 4 : fastCores >= 2 ? 2 : 1;
    if (k == 1 && g_realLogical >= 2) {
        k = 2; // one fast core plus SMT/E-cores
    }

    // Mapping table: primary thread of each core (fast cores first), then SMT siblings.
    // Unlike DiRT 2's Win7 path (which pairs siblings: 0,0',1,1'...) we fill physical cores
    // first, because the 2/4/8 core maps assume one high priority worker per core.
    int order[32];
    int n = 0;
    for (int i = 0; i < g_coreCount && n < 32; ++i) {
        order[n++] = g_cores[i].logical[0];
    }
    for (int s = 1; s < 4; ++s) {
        for (int i = 0; i < g_coreCount && n < 32; ++i) {
            if (g_cores[i].count > s) {
                order[n++] = g_cores[i].logical[s];
            }
        }
    }

    g_mapCount = k;
    for (int i = 0; i < k; ++i) {
        g_map[i] = order[i % n];
    }

    logf("CPU: %d logical CPUs in process mask %08lX, %d cores (%d fastest, efficiency class %d)",
         g_realLogical, g_processMask, g_coreCount, fastCores, best);
    for (int i = 0; i < g_coreCount; ++i) {
        char list[64] = {};
        for (int s = 0; s < g_cores[i].count; ++s) {
            char item[8];
            snprintf(item, sizeof(item), s ? ",%d" : "%d", g_cores[i].logical[s]);
            strcat_s(list, item);
        }
        logf("  core %2d: efficiency=%d logical CPUs=%s", i, g_cores[i].efficiency, list);
    }
    logf("Engine sees %d CPU(s), workerMap for %d core(s)", g_mapCount, g_mapCount);
    for (int i = 0; i < g_mapCount; ++i) {
        logf("  engine core %d -> logical CPU %d", i, g_map[i]);
    }
}

static DWORD map_mask(DWORD_PTR mask)
{
    DWORD out = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if (mask & (DWORD_PTR(1) << bit)) {
            out |= 1u << g_map[bit % g_mapCount];
        }
    }
    return out ? out : g_processMask;
}

// --------------------------------------------------------------------------------------------
// Hooks
// --------------------------------------------------------------------------------------------

static decltype(&GetSystemInfo) real_GetSystemInfo;
static decltype(&SetThreadAffinityMask) real_SetThreadAffinityMask;
static decltype(&SetThreadIdealProcessor) real_SetThreadIdealProcessor;
static decltype(&CreateFileA) real_CreateFileA;
static decltype(&CreateFileW) real_CreateFileW;
static decltype(&GetProcAddress) real_GetProcAddress;

static bool g_isKnownExe;       // DiRT.exe 1.2 (TimeDateStamp 0x483580D2)
static char g_workerMapPath[MAX_PATH];

static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO info)
{
    real_GetSystemInfo(info);
    if (g_cfg.affinity != 2) {
        info->dwNumberOfProcessors = g_mapCount;
        info->dwActiveProcessorMask = (DWORD_PTR(1) << g_mapCount) - 1;
    }
}

static DWORD_PTR WINAPI hook_SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask)
{
    // DiRT.exe 1.2: main thread affinity from "applicationProcessor" (call at 0x4044AA).
    // DiRT 2 only sets the ideal processor for its main thread, do the same.
    if (g_isKnownExe && g_cfg.mainThreadSoft && RETURN_ADDRESS() == (void*)0x4044B0) {
        logf("SetThreadAffinityMask(main thread, %08lX) -> skipped (ideal processor only)", DWORD(mask));
        return g_processMask;
    }

    switch (g_cfg.affinity) {
    case 0:
        logf("SetThreadAffinityMask(%p, %08lX) -> not pinned", thread, DWORD(mask));
        return g_processMask;
    case 2:
        return real_SetThreadAffinityMask(thread, mask);
    default: {
        DWORD mapped = map_mask(mask);
        DWORD_PTR result = real_SetThreadAffinityMask(thread, mapped);
        logf("SetThreadAffinityMask(%p, %08lX) -> %08lX%s", thread, DWORD(mask), mapped, result ? "" : " FAILED");
        return result ? result : g_processMask;
    }
    }
}

static DWORD WINAPI hook_SetThreadIdealProcessor(HANDLE thread, DWORD ideal)
{
    switch (g_cfg.affinity) {
    case 0:
        return 0;
    case 2:
        return real_SetThreadIdealProcessor(thread, ideal);
    default: {
        DWORD mapped = DWORD(g_map[ideal % g_mapCount]);
        DWORD result = real_SetThreadIdealProcessor(thread, mapped);
        return result == DWORD(-1) ? 0 : result;
    }
    }
}

static const char* base_name(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    return name;
}

static bool is_worker_map(const char* path)
{
    const char* name = base_name(path);
    size_t len = strlen(name);
    return len > 13 && _strnicmp(name, "workerMap", 9) == 0 && _stricmp(name + len - 4, ".xml") == 0;
}

// postprocess/effects.xml (front end) or tracks/<country>/<stage>/effects.xml (in-game)
static bool is_effects_xml(const char* path)
{
    if (_stricmp(base_name(path), "effects.xml") != 0) {
        return false;
    }
    char lower[MAX_PATH * 2];
    lstrcpynA(lower, path, sizeof(lower));
    CharLowerA(lower);
    for (char* p = lower; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
    return strstr(lower, "postprocess/") || strstr(lower, "tracks/");
}

// Replace the text of <Param name="X">...</Param> for matching overrides inside <Effect> blocks.
static int rewrite_effects(char* in, size_t inLen, char* out, size_t outCap, size_t* outLen, bool trackFile)
{
    size_t o = 0;
    int changes = 0;
    int chain = 0;
    auto emit = [&](const char* p, size_t n) {
        if (o + n <= outCap) {
            memcpy(out + o, p, n);
        }
        o += n;
    };

    const char* p = in;
    const char* end = in + inLen;
    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', end - p);
        if (!lt) {
            emit(p, end - p);
            break;
        }
        emit(p, lt - p);
        p = lt;
        const char* gt = (const char*)memchr(p, '>', end - p);
        if (!gt) {
            emit(p, end - p);
            break;
        }
        size_t tagLen = gt - p + 1;

        if (tagLen > 8 && strncmp(p, "<Effect ", 8) == 0) {
            chain = 0;
            const char* c = strstr(p, "chain=\"Type");
            if (c && c < gt) {
                chain = atoi(c + 11);
            }
            if (gt[-1] == '/') {
                chain = 0;
            }
            emit(p, tagLen);
            p = gt + 1;
            continue;
        }
        if (tagLen >= 9 && strncmp(p, "</Effect>", 9) == 0) {
            chain = 0;
        }
        if (chain && tagLen > 13 && strncmp(p, "<Param name=\"", 13) == 0) {
            const char* nameStart = p + 13;
            const char* nameEnd = (const char*)memchr(nameStart, '"', gt - nameStart);
            const char* close = nameEnd ? strstr(gt, "</Param>") : nullptr;
            const ParamOverride* rule = nullptr;
            if (nameEnd && close) {
                size_t nameLen = nameEnd - nameStart;
                for (int i = 0; i < g_ppCount; ++i) {
                    // track rules only touch track files, menu rules only the global file
                    // (it also holds in-game Type2 effects like damage flashes - left alone)
                    bool scope = trackFile ? g_pp[i].chain == 2 : g_pp[i].chain == 5;
                    if (scope && g_pp[i].chain == chain && strlen(g_pp[i].name) == nameLen &&
                        _strnicmp(g_pp[i].name, nameStart, nameLen) == 0) {
                        rule = &g_pp[i];
                    }
                }
            }
            if (rule) {
                // keep the whitespace around the value
                const char* v = gt + 1;
                while (v < close && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) ++v;
                const char* ve = close;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n')) --ve;
                emit(p, v - p);
                emit(rule->value, strlen(rule->value));
                emit(ve, close - ve);
                p = close;
                ++changes;
                continue;
            }
        }
        emit(p, tagLen);
        p = gt + 1;
    }
    *outLen = o;
    return o <= outCap ? changes : -1;
}

struct Redirect {
    char from[MAX_PATH];   // lower case, '/' separators
    char to[MAX_PATH];
};
static Redirect g_redirects[512];
static int g_redirectCount;
static CRITICAL_SECTION g_redirectLock;

static void normalize(const char* path, char* out, size_t cap)
{
    lstrcpynA(out, path, int(cap));
    CharLowerA(out);
    for (char* p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
}

static bool make_effects_copy(const char* path, char* out)
{
    HANDLE file = real_CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    char* in = (char*)HeapAlloc(GetProcessHeap(), 0, size + 1);
    size_t cap = size_t(size) * 2 + 4096;
    char* result = (char*)HeapAlloc(GetProcessHeap(), 0, cap);
    DWORD read = 0;
    bool ok = in && result && ReadFile(file, in, size, &read, nullptr) && read == size;
    CloseHandle(file);

    int changes = -1;
    size_t outLen = 0;
    if (ok) {
        in[size] = 0;
        char lower[MAX_PATH];
        normalize(path, lower, sizeof(lower));
        changes = rewrite_effects(in, size, result, cap, &outLen, strstr(lower, "tracks/") != nullptr);
    }

    bool redirected = false;
    if (changes > 0) {
        char dir[MAX_PATH];
        GetTempPathA(MAX_PATH, dir);
        strcat_s(dir, "Dirt1_Core_Bloom_Fix");
        CreateDirectoryA(dir, nullptr);

        // tracks/uk/kielder_01/effects.xml -> %TEMP%\Dirt1_Core_Bloom_Fix\tracks_uk_kielder_01_effects.xml
        char flat[MAX_PATH];
        normalize(path, flat, sizeof(flat));
        const char* rel = strstr(flat, "tracks/");
        if (!rel) rel = strstr(flat, "postprocess/");
        if (!rel) rel = flat;
        char name[MAX_PATH];
        lstrcpynA(name, rel, sizeof(name));
        for (char* q = name; *q; ++q) {
            if (*q == '/' || *q == ':') *q = '_';
        }
        snprintf(out, MAX_PATH, "%s\\%s", dir, name);

        HANDLE w = real_CreateFileA(out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            redirected = WriteFile(w, result, DWORD(outLen), &written, nullptr) && written == outLen;
            CloseHandle(w);
        }
        logf("effects: %s -> %s (%d params, %lu -> %u bytes)", path, redirected ? out : "WRITE FAILED", changes, size,
             unsigned(outLen));
    } else {
        logf("effects: %s unchanged (%d)", path, changes);
    }

    if (in) HeapFree(GetProcessHeap(), 0, in);
    if (result) HeapFree(GetProcessHeap(), 0, result);
    return redirected;
}

// Returns the replacement path for a file the game reads, or nullptr.
static const char* find_redirect(const char* path)
{
    bool workerMap = g_workerMapPath[0] && is_worker_map(path);
    bool effects = g_ppCount > 0 && is_effects_xml(path);
    if (!workerMap && !effects) {
        return nullptr;
    }
    if (workerMap) {
        return g_workerMapPath;
    }

    char key[MAX_PATH];
    normalize(path, key, sizeof(key));

    EnterCriticalSection(&g_redirectLock);
    const char* result = nullptr;
    for (int i = 0; i < g_redirectCount; ++i) {
        if (strcmp(g_redirects[i].from, key) == 0) {
            result = g_redirects[i].to[0] ? g_redirects[i].to : nullptr;
            LeaveCriticalSection(&g_redirectLock);
            return result;
        }
    }
    if (g_redirectCount < 512) {
        Redirect& r = g_redirects[g_redirectCount++];
        lstrcpynA(r.from, key, MAX_PATH);
        r.to[0] = 0;
        if (make_effects_copy(path, r.to)) {
            result = r.to;
        } else {
            r.to[0] = 0;
        }
    }
    LeaveCriticalSection(&g_redirectLock);
    return result;
}

static bool to_narrow(const WCHAR* path, char* out, int cap)
{
    return WideCharToMultiByte(CP_ACP, 0, path, -1, out, cap, nullptr, nullptr) != 0;
}

static decltype(&GetFileAttributesExA) real_GetFileAttributesExA;
static decltype(&FindFirstFileA) real_FindFirstFileA;

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE templ)
{
    if (name && !(access & GENERIC_WRITE)) {
        if (const char* to = find_redirect(name)) {
            logf("CreateFileA(%s) -> %s", name, to);
            name = to;
        }
    }
    return real_CreateFileA(name, access, share, sa, disposition, flags, templ);
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE templ)
{
    char narrow[MAX_PATH * 2];
    if (name && !(access & GENERIC_WRITE) && to_narrow(name, narrow, sizeof(narrow))) {
        if (const char* to = find_redirect(narrow)) {
            logf("CreateFileW(%s) -> %s", narrow, to);
            return real_CreateFileA(to, access, share, sa, disposition, flags, templ);
        }
    }
    return real_CreateFileW(name, access, share, sa, disposition, flags, templ);
}

// The engine sometimes takes the file size before opening it, keep both consistent.
static BOOL WINAPI hook_GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    if (name) {
        if (const char* to = find_redirect(name)) {
            name = to;
        }
    }
    return real_GetFileAttributesExA(name, level, info);
}

static HANDLE WINAPI hook_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
{
    const char* to = name && !strpbrk(name, "*?") ? find_redirect(name) : nullptr;
    if (!to) {
        return real_FindFirstFileA(name, data);
    }
    HANDLE h = real_FindFirstFileA(to, data);
    if (h != INVALID_HANDLE_VALUE && data) {
        lstrcpynA(data->cFileName, base_name(name), MAX_PATH);
        data->cAlternateFileName[0] = 0;
    }
    return h;
}

// IDirectDraw7::GetAvailableVidMem (vtable slot 23) clamp.
typedef HRESULT(WINAPI* GetAvailableVidMem_t)(void*, LPDDSCAPS2, LPDWORD, LPDWORD);
static GetAvailableVidMem_t real_GetAvailableVidMem;

static HRESULT WINAPI hook_GetAvailableVidMem(void* self, LPDDSCAPS2 caps, LPDWORD total, LPDWORD free)
{
    HRESULT hr = real_GetAvailableVidMem(self, caps, total, free);
    if (SUCCEEDED(hr) && g_cfg.maxVideoMemoryMB > 0) {
        DWORD limit = DWORD(g_cfg.maxVideoMemoryMB) << 20;
        DWORD t = total ? *total : 0, f = free ? *free : 0;
        if (total && *total > limit) {
            *total = limit;
        }
        if (free && *free > limit) {
            *free = limit;
        }
        logf("GetAvailableVidMem: total %lu MB free %lu MB -> %lu / %lu MB", t >> 20, f >> 20,
             total ? *total >> 20 : 0, free ? *free >> 20 : 0);
    }
    return hr;
}

typedef HRESULT(WINAPI* DirectDrawCreateEx_t)(GUID*, LPVOID*, REFIID, IUnknown*);
static DirectDrawCreateEx_t real_DirectDrawCreateEx;

static HRESULT WINAPI hook_DirectDrawCreateEx(GUID* guid, LPVOID* dd, REFIID iid, IUnknown* outer)
{
    HRESULT hr = real_DirectDrawCreateEx(guid, dd, iid, outer);
    if (SUCCEEDED(hr) && dd && *dd && !real_GetAvailableVidMem) {
        void** vtable = *(void***)*dd;
        DWORD old;
        if (VirtualProtect(&vtable[23], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            real_GetAvailableVidMem = GetAvailableVidMem_t(vtable[23]);
            vtable[23] = (void*)&hook_GetAvailableVidMem;
            VirtualProtect(&vtable[23], sizeof(void*), old, &old);
        }
    }
    return hr;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC proc = real_GetProcAddress(module, name);
    if (proc && HIWORD(name) && g_cfg.maxVideoMemoryMB > 0 && strcmp(name, "DirectDrawCreateEx") == 0) {
        real_DirectDrawCreateEx = DirectDrawCreateEx_t(proc);
        return FARPROC(&hook_DirectDrawCreateEx);
    }
    return proc;
}

// --------------------------------------------------------------------------------------------
// D3D9 display mode filter
// --------------------------------------------------------------------------------------------

typedef UINT(WINAPI* GetAdapterModeCount_t)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(WINAPI* EnumAdapterModes_t)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT(WINAPI* GetAdapterDisplayMode_t)(IDirect3D9*, UINT, D3DDISPLAYMODE*);

static GetAdapterModeCount_t real_GetAdapterModeCount;
static EnumAdapterModes_t real_EnumAdapterModes;
static GetAdapterDisplayMode_t real_GetAdapterDisplayMode;

struct ModeList {
    bool used;
    UINT adapter;
    D3DFORMAT format;
    UINT count;
    D3DDISPLAYMODE modes[128];
};
static ModeList g_modeLists[8];
static CRITICAL_SECTION g_modeLock;

static bool same_res(const D3DDISPLAYMODE& a, const D3DDISPLAYMODE& b)
{
    return a.Width == b.Width && a.Height == b.Height;
}

static void build_mode_list(IDirect3D9* d3d, ModeList& list)
{
    D3DDISPLAYMODE desktop = {};
    real_GetAdapterDisplayMode(d3d, list.adapter, &desktop);

    UINT total = real_GetAdapterModeCount(d3d, list.adapter, list.format);
    static D3DDISPLAYMODE all[1024];
    UINT n = 0;
    for (UINT i = 0; i < total && n < 1024; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(real_EnumAdapterModes(d3d, list.adapter, list.format, i, &m))) {
            continue;
        }
        if (int(m.Width) < g_cfg.minWidth || m.Height < 600 || m.Height > m.Width) {
            continue;
        }
        all[n++] = m;
    }

    // Distinct resolutions, largest first; the desktop resolution is always kept.
    static D3DDISPLAYMODE res[1024];
    UINT resCount = 0;
    for (UINT i = 0; i < n; ++i) {
        bool found = false;
        for (UINT j = 0; j < resCount && !found; ++j) {
            found = same_res(res[j], all[i]);
        }
        if (!found) {
            res[resCount++] = all[i];
        }
    }
    for (UINT i = 1; i < resCount; ++i) {
        D3DDISPLAYMODE m = res[i];
        UINT j = i;
        while (j > 0 && UINT64(res[j - 1].Width) * res[j - 1].Height < UINT64(m.Width) * m.Height) {
            res[j] = res[j - 1];
            --j;
        }
        res[j] = m;
    }
    UINT maxRes = g_cfg.maxResolutions > 0 ? UINT(g_cfg.maxResolutions) : 1024;
    if (resCount > maxRes) {
        bool desktopKept = false;
        for (UINT i = 0; i < maxRes; ++i) {
            desktopKept |= same_res(res[i], desktop);
        }
        if (!desktopKept) {
            for (UINT i = maxRes; i < resCount; ++i) {
                if (same_res(res[i], desktop)) {
                    res[maxRes - 1] = res[i];
                    break;
                }
            }
        }
        resCount = maxRes;
    }

    // Per resolution: desktop refresh rate and 60 Hz, otherwise the highest one.
    list.count = 0;
    for (int r = int(resCount) - 1; r >= 0; --r) { // ascending, like D3D9 does
        UINT best = 0;
        bool haveDesktop = false, have60 = false;
        for (UINT i = 0; i < n; ++i) {
            if (!same_res(all[i], res[r])) {
                continue;
            }
            best = all[i].RefreshRate > best ? all[i].RefreshRate : best;
            haveDesktop |= all[i].RefreshRate == desktop.RefreshRate;
            have60 |= all[i].RefreshRate == 60;
        }
        UINT rates[2];
        UINT rateCount = 0;
        if (have60) {
            rates[rateCount++] = 60;
        }
        if (haveDesktop && desktop.RefreshRate != 60) {
            rates[rateCount++] = desktop.RefreshRate;
        }
        if (rateCount == 0) {
            rates[rateCount++] = best;
        }
        for (UINT k = 0; k < rateCount && list.count < 128; ++k) {
            D3DDISPLAYMODE m = res[r];
            m.RefreshRate = rates[k];
            m.Format = list.format;
            list.modes[list.count++] = m;
        }
    }

    logf("D3D9 modes (adapter %u, format %d): %u reported, %u kept (desktop %ux%u@%u)", list.adapter, int(list.format),
         total, list.count, desktop.Width, desktop.Height, desktop.RefreshRate);
    for (UINT i = 0; i < list.count; ++i) {
        logf("  %ux%u @ %u Hz", list.modes[i].Width, list.modes[i].Height, list.modes[i].RefreshRate);
    }
}

static ModeList* get_mode_list(IDirect3D9* d3d, UINT adapter, D3DFORMAT format)
{
    EnterCriticalSection(&g_modeLock);
    ModeList* result = nullptr;
    for (auto& list : g_modeLists) {
        if (list.used && list.adapter == adapter && list.format == format) {
            result = &list;
            break;
        }
    }
    if (!result) {
        for (auto& list : g_modeLists) {
            if (!list.used) {
                list.used = true;
                list.adapter = adapter;
                list.format = format;
                build_mode_list(d3d, list);
                result = &list;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_modeLock);
    return result;
}

static UINT WINAPI hook_GetAdapterModeCount(IDirect3D9* d3d, UINT adapter, D3DFORMAT format)
{
    ModeList* list = get_mode_list(d3d, adapter, format);
    return list && list->count ? list->count : real_GetAdapterModeCount(d3d, adapter, format);
}

static HRESULT WINAPI hook_EnumAdapterModes(IDirect3D9* d3d, UINT adapter, D3DFORMAT format, UINT index, D3DDISPLAYMODE* mode)
{
    ModeList* list = get_mode_list(d3d, adapter, format);
    if (!list || !list->count) {
        return real_EnumAdapterModes(d3d, adapter, format, index, mode);
    }
    if (!mode) {
        return D3DERR_INVALIDCALL;
    }
    if (index >= list->count) {
        return D3DERR_INVALIDCALL;
    }
    *mode = list->modes[index];
    return D3D_OK;
}

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t real_Direct3DCreate9;

static IDirect3D9* WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9* d3d = real_Direct3DCreate9(sdk);
    if (d3d && !real_EnumAdapterModes) {
        void** vtable = *(void***)d3d;
        DWORD old;
        if (VirtualProtect(&vtable[6], 3 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            real_GetAdapterModeCount = GetAdapterModeCount_t(vtable[6]);
            real_EnumAdapterModes = EnumAdapterModes_t(vtable[7]);
            real_GetAdapterDisplayMode = GetAdapterDisplayMode_t(vtable[8]);
            vtable[6] = (void*)&hook_GetAdapterModeCount;
            vtable[7] = (void*)&hook_EnumAdapterModes;
            VirtualProtect(&vtable[6], 3 * sizeof(void*), old, &old);
            logf("IDirect3D9 mode enumeration hooked");
        }
    }
    return d3d;
}

// --------------------------------------------------------------------------------------------
// IAT patching
// --------------------------------------------------------------------------------------------

static bool patch_iat(HMODULE module, const char* dll, const char* func, void* hook, void** original)
{
    auto base = (BYTE*)module;
    auto dos = PIMAGE_DOS_HEADER(base);
    auto nt = PIMAGE_NT_HEADERS(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return false;
    }

    for (auto imp = PIMAGE_IMPORT_DESCRIPTOR(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) {
            continue;
        }
        auto names = PIMAGE_THUNK_DATA(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = PIMAGE_THUNK_DATA(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto ibn = PIMAGE_IMPORT_BY_NAME(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) {
                continue;
            }
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *original = (void*)iat->u1.Function;
            iat->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// The game's own system/workerMap4Core.xml / workerMap2Core.xml / workerMap.xml is used for
// whatever map hardware_settings_restrictions.xml / hardware_settings_config.xml ask for.
// workerMap8Core.xml is never used (undefined workers + multithreaded physics solver).
static void select_worker_map()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) {
        *slash = 0;
    }
    const char* name = g_mapCount >= 4 ? "workerMap4Core.xml" : g_mapCount >= 2 ? "workerMap2Core.xml" : "workerMap.xml";
    snprintf(g_workerMapPath, sizeof(g_workerMapPath), "%s\\system\\%s", dir, name);
    if (GetFileAttributesA(g_workerMapPath) == INVALID_FILE_ATTRIBUTES) {
        logf("%s not found, workerMap redirect disabled", g_workerMapPath);
        g_workerMapPath[0] = 0;
        return;
    }
    logf("Worker map: %s", g_workerMapPath);
}

static void install()
{
    load_config();
    open_log();
    logf("Dirt1_Core_Bloom_Fix loaded");

    HMODULE exe = GetModuleHandleA(nullptr);
    auto nt = PIMAGE_NT_HEADERS((BYTE*)exe + PIMAGE_DOS_HEADER(exe)->e_lfanew);
    g_isKnownExe = nt->FileHeader.TimeDateStamp == 0x483580D2;
    logf("Executable timestamp %08lX (%s), LargeAddressAware=%s", nt->FileHeader.TimeDateStamp,
         g_isKnownExe ? "DiRT 1.2" : "unknown version", (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "yes" : "no");

    OSVERSIONINFOW os = {sizeof(os)};
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    if (auto rtl = RtlGetVersion_t(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"))) {
        rtl(&os);
    }
    logf("Windows %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);

    detect_topology();

    InitializeCriticalSection(&g_redirectLock);
    load_postprocess_config();
    logf("Post process: %s, %d parameter rules", g_cfg.postProcess ? "on" : "off", g_ppCount);
    for (int i = 0; i < g_ppCount; ++i) {
        logf("  %s %s = %s", g_pp[i].chain == 5 ? "menu " : "track", g_pp[i].name, g_pp[i].value);
    }

    if (g_cfg.workerMap) {
        select_worker_map();
    }

    struct {
        const char* name;
        void* hook;
        void** original;
    } hooks[] = {
        {"GetSystemInfo", (void*)&hook_GetSystemInfo, (void**)&real_GetSystemInfo},
        {"SetThreadAffinityMask", (void*)&hook_SetThreadAffinityMask, (void**)&real_SetThreadAffinityMask},
        {"SetThreadIdealProcessor", (void*)&hook_SetThreadIdealProcessor, (void**)&real_SetThreadIdealProcessor},
        {"CreateFileA", (void*)&hook_CreateFileA, (void**)&real_CreateFileA},
        {"CreateFileW", (void*)&hook_CreateFileW, (void**)&real_CreateFileW},
        {"GetFileAttributesExA", (void*)&hook_GetFileAttributesExA, (void**)&real_GetFileAttributesExA},
        {"FindFirstFileA", (void*)&hook_FindFirstFileA, (void**)&real_FindFirstFileA},
        {"GetProcAddress", (void*)&hook_GetProcAddress, (void**)&real_GetProcAddress},
    };

    for (auto& h : hooks) {
        bool ok = patch_iat(exe, "KERNEL32.dll", h.name, h.hook, h.original);
        logf("hook %-24s %s", h.name, ok ? "ok" : "not found");
    }

    if (g_cfg.filterModes) {
        InitializeCriticalSection(&g_modeLock);
        bool ok = patch_iat(exe, "d3d9.dll", "Direct3DCreate9", (void*)&hook_Direct3DCreate9, (void**)&real_Direct3DCreate9);
        logf("hook %-24s %s", "Direct3DCreate9", ok ? "ok" : "not found");
    }

    // Fallbacks so that hooks never call a null pointer.
    if (!real_GetSystemInfo) real_GetSystemInfo = &GetSystemInfo;
    if (!real_SetThreadAffinityMask) real_SetThreadAffinityMask = &SetThreadAffinityMask;
    if (!real_SetThreadIdealProcessor) real_SetThreadIdealProcessor = &SetThreadIdealProcessor;
    if (!real_CreateFileA) real_CreateFileA = &CreateFileA;
    if (!real_CreateFileW) real_CreateFileW = &CreateFileW;
    if (!real_GetProcAddress) real_GetProcAddress = &GetProcAddress;
    if (!real_GetFileAttributesExA) real_GetFileAttributesExA = &GetFileAttributesExA;
    if (!real_FindFirstFileA) real_FindFirstFileA = &FindFirstFileA;

    if (g_cfg.dpiAware) {
        typedef BOOL(WINAPI * SetProcessDPIAware_t)();
        if (auto f = SetProcessDPIAware_t(GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware"))) {
            f();
        }
    }
}

// --------------------------------------------------------------------------------------------
// dinput8 proxy
// --------------------------------------------------------------------------------------------

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t real_DirectInput8Create;

static void load_real_dinput8()
{
    if (real_DirectInput8Create) {
        return;
    }
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 for a 32-bit process
    strcat_s(path, "\\dinput8.dll");
    HMODULE dll = LoadLibraryA(path);
    if (dll) {
        real_DirectInput8Create = DirectInput8Create_t(GetProcAddress(dll, "DirectInput8Create"));
    }
    logf("system dinput8: %s", real_DirectInput8Create ? path : "NOT FOUND");
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    load_real_dinput8();
    if (!real_DirectInput8Create) {
        return E_FAIL;
    }
    return real_DirectInput8Create(inst, version, iid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dllDir, MAX_PATH);
        if (char* slash = strrchr(g_dllDir, '\\')) {
            *slash = 0;
        }
        install();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            fclose(g_log);
        }
    }
    return TRUE;
}
