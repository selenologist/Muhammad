// SPDX-License-Identifier: GPL-3.0-only
#include "p5_factory.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace p5fac {

static uint8_t gStore[200][133];                      // .bss, NOT .rodata:
static std::atomic<int> gCount{0};                    // zero proprietary bytes in the binary
static char gPathUsed[1024] = "";

bool loaded() { return gCount.load(std::memory_order_acquire) == 200; }
int  count()  { return gCount.load(std::memory_order_acquire); }
const uint8_t *raw(int i) { return gStore[i]; }
const char *pathUsed() { return gPathUsed; }

static bool tryFile(const std::string &p)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    uint8_t head[8];
    if (std::fread(head, 1, 8, f) != 8 || std::memcmp(head, "P5FC", 4) != 0)
    { std::fclose(f); return false; }
    uint32_t rec, n;
    std::memcpy(&rec, head + 4, 4);
    if (std::fread(&n, 4, 1, f) != 1) { std::fclose(f); return false; }
    if (rec != 133 || n != 200) { std::fclose(f); return false; }   // strict v1
    if (std::fread(gStore, 133, 200, f) != 200) { std::fclose(f); return false; }
    std::fclose(f);
    std::snprintf(gPathUsed, sizeof gPathUsed, "%s", p.c_str());
    gCount.store(200, std::memory_order_release);     // publish AFTER the bytes
    return true;
}

static std::string moduleDir()
{
#ifdef _WIN32
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&moduleDir, &hm)) return {};
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(hm, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(p, n);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : s.substr(0, slash);
#else
    Dl_info di;
    if (!dladdr((const void *)&moduleDir, &di) || !di.dli_fname) return {};
    std::string s(di.dli_fname);
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? std::string() : s.substr(0, slash);
#endif
}

// mirrors p5store::baseDir()'s location WITHOUT linking the store (this loader
// is engine-level and must stay self-contained); keep the two in sync if the
// user-patch folder ever moves.
static std::string userPatchDir()
{
    const char *home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return {};
    return std::string(home) + "/Documents/Muhammad Synth";
}

bool load()
{
    if (loaded()) return true;
    const char *env = std::getenv("P5_FACTORY");
    if (env && *env) { return tryFile(env); }        // authoritative: no fallback
    const std::string md = moduleDir();
    if (!md.empty())
    {   if (tryFile(md + "/muhammad-factory.p5fac")) return true;
        if (tryFile(md + "/../Resources/muhammad-factory.p5fac")) return true; }
    const std::string ud = userPatchDir();
    return !ud.empty() && tryFile(ud + "/muhammad-factory.p5fac");
}

} // namespace p5fac
