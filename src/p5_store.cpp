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
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
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
    for (char ch : name)                       // faithful: keep the typed case
    {
        if (std::isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-' || ch == ' ')
            out.push_back(ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 24) out.resize(24);
    return out;
}

std::string patchPath(const std::string &name)
{
    return baseDir() + "/" + sanitize(name) + ".muhammad";
}

bool savePatch(const std::string &name, const uint8_t raw[134])
{
    if (!ensureDir()) return false;
    const std::string n = sanitize(name);
    if (n.empty()) return false;
    std::ofstream f(patchPath(n), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char *)&kMagic, sizeof kMagic);
    f.write((const char *)raw, 134);
    return (bool)f;
}

namespace { char foldc(char c){ return (char)std::tolower((unsigned char)c); }
bool foldEq(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (foldc(a[i]) != foldc(b[i])) return false;
    return true;
} }

std::string foldCollision(const std::string &name)
{
    const std::string n = sanitize(name);
    for (const auto &f : listPatches()) if (foldEq(f, n)) return f;
    return {};
}

bool loadPatch(const std::string &name, uint8_t raw[134])   // 134 even when the
//               file is 133: byte 133 gets its honest default
{
    std::ifstream f(patchPath(name), std::ios::binary);
    if (!f) f = std::ifstream(name, std::ios::binary);       // full path too
    if (!f) return false;
    uint64_t magic = 0;
    f.read((char *)&magic, sizeof magic);
    if (magic != kMagic) return false;
    f.read((char *)raw, 134);
    const std::streamsize n = f.gcount();
    if (n == 133) { raw[133] = 0; return true; }   // files predate the FEEDBACK byte
    return n == 134;
}

std::vector<std::string> listPatches()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(baseDir(), ec))
    {
        if (!e.is_regular_file()) continue;
        const std::string fn = e.path().filename().string();
        if (fn.size() > 9 && fn.compare(fn.size() - 9, 9, ".muhammad") == 0)
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

bool findSlotFor(const Source &needle, int &slotOut)
{
    for (const auto &[sl, s] : g_map)
        if (s.kind == needle.kind && s.factory == needle.factory && s.file == needle.file)
        { slotOut = sl; return true; }
    return false;
}

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
        if (sl < 0 || sl >= kSlotBanks * kSlotsPerBank) continue;   // corrupt map line
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

// ---- tunings (Surge tuning-library) ----------------------------------------
#include <Tunings.h>
#include <TuningsImpl.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace p5store {

std::string tuningsDir() { return baseDir() + "/Tunings"; }

std::vector<std::string> listTunings()
{
    std::vector<std::string> out;
    std::error_code ec;
    // std::filesystem, not dirent: mingw ships dirent.h but MSVC does not,
    // and this TU already carries the fs alias (listPatches above). Same
    // filter, same sort, same unique — the scan is the contract, not the API.
    for (const auto &e : fs::directory_iterator(tuningsDir(), ec))
    {
        if (!e.is_regular_file()) continue;
        const std::string n = e.path().filename().string();
        const size_t dot = n.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = n.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".scl" || ext == ".kbm") out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static std::string slurp(const std::string &path)
{
    // A tuning file is a few hundred lines of text. Cap it: an unbounded
    // slurp of a 500 MB neighbor is a memory bomb with zero upside.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff sz = f.tellg();
    if (sz <= 0 || sz > (1 << 20)) return {};
    f.seekg(0, std::ios::beg);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool buildScaleTones(const double *tones, int n, int16_t *dev16)
{
    if (!tones || n < 2 || n > kMaxTones) return false;
    try
    {
        Tunings::Scale s;
        s.count = n;
        s.name = s.description = "Muhammad wheel edit";
        for (int i = 0; i < n; i++)
        {
            Tunings::Tone to;
            to.type = Tunings::Tone::kToneCents;
            to.cents = tones[i];
            to.ratio_d = to.ratio_n = 1;
            to.floatValue = tones[i] / 1200.0 + 1.0;
            to.stringRep = std::to_string(tones[i]);
            to.lineno = -1;
            s.tones.push_back(to);
        }
        Tunings::Tuning t(s);
        for (int k = 0; k < 128; k++)
        {
            long d = lround(t.retuningFromEqualInSemitonesForMidiNote(k) * 16.0);
            if (d < -512) d = -512; if (d > 512) d = 512;
            dev16[k] = (int16_t)d;
        }
        return true;
    }
    catch (...) { return false; }
}

uint16_t deriveMask(const double *tones, int n)
{   // chromatic positions of scale degrees, same law as the stock generator:
    // round(cents/100) mod 12, unison owns key 0, collisions shift up, period skipped.
    if (n <= 0 || n > 12) return 0;
    uint16_t mask = 1;
    for (int i = 0; i + 1 < n; i++)                 // last entry is the closing period
    {
        int pos = (int)(tones[i] / 100.0 + 0.5) % 12;
        if (pos <= 0) pos = 1;                       // never collide with the root slot
        while (mask & (1u << pos)) pos = (pos + 1) % 12;
        mask |= (uint16_t)(1u << pos);
    }
    return mask;
}

bool loadTuningDev(const std::string &fileName, int16_t *dev16, char *nameOut, int nameCap,
                   double *tonesOut, int *tonesN, bool *kbmUsed)
{
    if (fileName.find('/') != std::string::npos || fileName.find('\\') != std::string::npos ||
        fileName.find("..") != std::string::npos)      // a NAME, never a path
        return false;
    const std::string path = tuningsDir() + "/" + fileName;
    const size_t dot = path.rfind('.');
    const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    std::string sclT, kbmT;
    if (ext == ".kbm") { kbmT = slurp(path); if (FILE *alt = fopen((stem + ".scl").c_str(), "r")) { fclose(alt); sclT = slurp(stem + ".scl"); } }
    else { sclT = slurp(path); if (FILE *alt = fopen((stem + ".kbm").c_str(), "r")) { fclose(alt); kbmT = slurp(stem + ".kbm"); } }
    if (sclT.empty() && kbmT.empty()) return false;
    if (kbmUsed) *kbmUsed = !kbmT.empty();
    try
    {
        using namespace Tunings;
        Tuning t = sclT.empty()
            ? Tuning(parseKBMData(kbmT))
            : (kbmT.empty() ? Tuning(parseSCLData(sclT)) : Tuning(parseSCLData(sclT), parseKBMData(kbmT)));
        if (tonesOut && tonesN)
        {   // hand the wheel an editable copy of the SCALE degrees (cumulative
            // cents, last = octave); KBM-mapped or huge files come back uneditable
            *tonesN = 0;
            const Scale *sc = nullptr;
            if (!sclT.empty())
            {
                static Scale hold;                      // main-thread only
                hold = parseSCLData(sclT);
                sc = &hold;
            }
            if (sc && sc->count >= 2 && sc->count <= kMaxTones)
            {
                for (int i = 0; i < sc->count; i++) tonesOut[i] = sc->tones[i].cents;
                *tonesN = sc->count;
            }
        }
        for (int k = 0; k < 128; k++)
        {
            const double semis = t.retuningFromEqualInSemitonesForMidiNote(k);
            long d = lround(semis * 16.0);
            if (d < -512) d = -512; if (d > 512) d = 512;
            dev16[k] = (int16_t)d;
        }
        // display name: SCL/KBM first comment line, else the file stem
        const std::string &src = !sclT.empty() ? sclT : kbmT;
        std::string first = src.substr(0, src.find('\n'));
        size_t b = first.find_first_not_of(" \t");
        if (b != std::string::npos) first = first.substr(b);
        if (first.empty()) { size_t sl = stem.rfind('/'); first = sl == std::string::npos ? stem : stem.substr(sl + 1); }
        std::strncpy(nameOut, first.c_str(), nameCap - 1);
        nameOut[nameCap - 1] = 0;
        return true;
    }
    catch (...) { return false; }
}

} // namespace p5store

// ---- quantizer & save --------------------------------------------------------
#include <sys/stat.h>
namespace p5store {

bool buildScaleDev(const double *p, int degrees, uint16_t mask, int root, int16_t *dev16)
{
    if (!p || degrees < 2 || degrees > 66 || root < -24 || root > 24) return false;
    const double period = p[degrees];
    root = ((root % 12) + 12) % 12;          // key position within the octave
    const bool quantize = mask != 0 && degrees <= 12;
    int pos[12];
    if (quantize)
    {   int k = 0;
        for (int b = 0; b < 12 && k < degrees; b++)
            if (mask >> b & 1) pos[k++] = b;
        if (k != degrees) return false;      // mask must carry exactly one slot per degree
    }
    for (int key = 0; key < 128; key++)
    {
        const int u = key - 60 - root;           // distance above the root at middle C (melodee's n-60)
        int d, o;
        double q;
        if (quantize)
        {
            o = (int)std::floor((double)u / 12.0);
            const int r = u - 12 * o;
            int best = 0, bd = 99;
            for (int i = 0; i < degrees; i++)
            {   const int dd = std::abs(pos[i] - r);
                if (dd < bd) { bd = dd; best = i; }    // ties: lower degree wins (loop order)
            }
            d = best;
        }
        else
        {
            const int n = degrees;
            o = (int)std::floor((double)u / n);
            d = u - n * o;
        }
        q = p[d] + period * o - 100.0 * u;
        if (q > 6400.0) q = 6400.0; if (q < -6400.0) q = -6400.0;
        long v = lround(q / 6.25);                     // engine units: 1/16 semitone
        if (v < -512) v = -512; if (v > 512) v = 512;
        dev16[key] = (int16_t)v;
    }
    return true;
}

static std::string sclSanitize(const std::string &in)
{
    std::string out;
    for (char c : in)
        out.push_back((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == ' ' || c == '.' || c == '_' || c == '-' ? c : '_');
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) out = "untitled";
    if (out.size() > 48) out.resize(48);
    return out;
}

bool tuningFileExists(const char *name)
{
    const std::string stem = tuningsDir() + "/" + sclSanitize(name);
    for (const char *ext : {".scl", ".kbm", ".SCL", ".KBM"})
    {   FILE *f = fopen((stem + ext).c_str(), "r");
        if (f) { fclose(f); return true; } }
    return false;
}

bool saveTuningScl(const char *name, const double *tones, int n)
{
    if (!tones || n < 2 || n > 64) return false;
    const std::string label = sclSanitize(name);
    std::string dir = tuningsDir();
    {   std::error_code ec; fs::create_directories(dir, ec); }   // portable; raw mkdir() is POSIX-only
    std::ofstream f(dir + "/" + label + ".scl", std::ios::trunc);
    if (!f) return false;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s\n", label.c_str());   f << buf;   // description line
    std::snprintf(buf, sizeof buf, " %d\n!\n", n);          f << buf;   // count = tones incl. period
    for (int i = 0; i < n; i++)
    {   std::snprintf(buf, sizeof buf, " %.4f\n", tones[i]); f << buf; }
    return (bool)f;
}

} // namespace p5store
