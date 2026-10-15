// SPDX-License-Identifier: GPL-3.0-only
#include "p5_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace p5store {

namespace {
constexpr uint64_t kMagic = 0x31444148554D7055ULL;   // "UpMUHAM1"
std::map<int, Source> g_map;
bool g_mapLoaded = false;

std::string rawDir()
{
    if (const char *over = std::getenv("P5_STORE_DIR")) return over;   // tests / overrides
    std::error_code ec;
    // XDG user-dirs first, then plain ~/Documents, then ~/Music, then home.
    if (const char *docs = std::getenv("XDG_DOCUMENTS_DIR")) if (*docs) return docs;
    const char *home = std::getenv("HOME");
    if (!home) home = ".";
    fs::path h(home);
    fs::path cand[] = {h / "Documents", h / "Music", h};
    for (const auto &p : cand)
        if (fs::is_directory(p, ec)) { h = p; break; }
    return (h / "Muhammad Synth").string();
}
} // namespace

bool ensureDir()
{
    std::error_code ec;
    fs::create_directories(baseDir(), ec);
    return !ec || fs::is_directory(baseDir());
}

std::string baseDir() { return rawDir(); }

std::string sanitize(const std::string &name)
{
    std::string out;
    for (char ch : name)
    {
        const char u = (char)std::toupper((unsigned char)ch);
        if (std::isalnum((unsigned char)u) || u == '.' || u == '_' || u == '-' || u == ' ')
            out.push_back(u);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 24) out.resize(24);
    return out;
}

std::string patchPath(const std::string &name)
{
    return baseDir() + "/" + sanitize(name) + ".muhammed";
}

bool savePatch(const std::string &name, const uint8_t raw[133])
{
    if (!ensureDir()) return false;
    const std::string n = sanitize(name);
    if (n.empty()) return false;
    std::ofstream f(patchPath(n), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char *)&kMagic, sizeof kMagic);
    f.write((const char *)raw, 133);
    return (bool)f;
}

bool loadPatch(const std::string &name, uint8_t raw[133])
{
    std::ifstream f(patchPath(name), std::ios::binary);
    if (!f) f = std::ifstream(name, std::ios::binary);       // full path too
    if (!f) return false;
    uint64_t magic = 0;
    f.read((char *)&magic, sizeof magic);
    if (magic != kMagic) return false;
    f.read((char *)raw, 133);
    return f.gcount() == 133;
}

std::vector<std::string> listPatches()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(baseDir(), ec))
    {
        if (!e.is_regular_file()) continue;
        const std::string fn = e.path().filename().string();
        if (fn.size() > 9 && fn.compare(fn.size() - 9, 9, ".muhammed") == 0)
            out.push_back(fn.substr(0, fn.size() - 9));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --------------------------------------------------------------- map ------

static std::string mapPath() { return baseDir() + "/bank-program.map"; }

void setSlot(int slot, const Source &src)
{
    if (slot < 0 || slot >= kSlotBanks * kSlotsPerBank) return;
    g_map[slot] = src;
    g_mapLoaded = true;
}
bool hasSlot(int slot) { return g_map.count(slot) > 0; }
Source slot(int s)
{
    auto it = g_map.find(s);
    if (it != g_map.end()) return it->second;
    Source d; d.kind = SourceKind::Factory; d.factory = s; return d;
}
void clearSlot(int s) { g_map.erase(s); }

bool loadMap()
{
    std::ifstream f(mapPath());
    if (!f) { g_map.clear(); g_mapLoaded = true; return false; }
    g_map.clear();
    int sl; char kind[8];
    std::string rest;
    while (f >> sl >> kind)
    {
        std::getline(f, rest);
        size_t b = rest.find_first_not_of(" \t");
        rest = b == std::string::npos ? "" : rest.substr(b);
        Source s;
        if (!std::strcmp(kind, "fac")) { s.kind = SourceKind::Factory; s.factory = std::atoi(rest.c_str()); }
        else if (!std::strcmp(kind, "fil")) { s.kind = SourceKind::File; s.file = rest; }
        else continue;
        g_map[sl] = s;
    }
    g_mapLoaded = true;
    return true;
}

bool saveMap()
{
    if (!ensureDir()) return false;
    std::ofstream f(mapPath(), std::ios::trunc);
    if (!f) return false;
    for (const auto &[sl, s] : g_map)
    {
        if (s.kind == SourceKind::Factory) f << sl << " fac " << s.factory << "\n";
        else f << sl << " fil " << s.file << "\n";
    }
    return (bool)f;
}

} // namespace p5store
