// Patch store round-trips: save/list/load + bank/program retrieval map.
#include "p5_store.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>            // mkdtemp (POSIX; libc++ does not leak it via stdlib)
#include <filesystem>

int main()
{
    char tmp[] = "/tmp/muh-store-XXXXXX";
    if (!mkdtemp(tmp)) { std::printf("mkdtemp failed\n"); return 1; }
    setenv("P5_STORE_DIR", tmp, 1);
    int fails = 0;
    auto check = [&](const char *what, bool ok) {
        std::printf("%-40s %s\n", what, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    };
    uint8_t raw[134]; std::memset(raw, 7, sizeof raw);
    std::memcpy(raw + 65, "TEST PATCH", 10);
    check("save", p5store::savePatch("TEST PATCH", raw));
    check("list contains it", [&]{ auto v = p5store::listPatches();
        for (auto &s : v) if (s == "TEST PATCH") return true; return false; }());
    uint8_t back[134] = {};
    check("load roundtrip", p5store::loadPatch("TEST PATCH", back) && !std::memcmp(raw, back, 134));
    check("sanitize keeps case", p5store::sanitize("a/b\\c*?") == "abc");
    check("sanitize faithful mixed", p5store::sanitize("Peace Be 5") == "Peace Be 5");
    check("foldCollision finds PEACE vs peace", [&]{
        uint8_t r2[134]; std::memset(r2, 3, sizeof r2);
        if (!p5store::savePatch("peace", r2)) return false;
        const std::string hit = p5store::foldCollision("PEACE");
        return hit == "peace" && p5store::foldCollision("brave").empty();
    }());
    check("empty name refused", !p5store::savePatch("///", raw));
    p5store::clearSlot(9);
    check("default slot = factory", p5store::slot(9).kind == p5store::SourceKind::Factory && p5store::slot(9).factory == 9);
    p5store::Source s; s.kind = p5store::SourceKind::File; s.file = "TEST PATCH";
    p5store::setSlot(9, s);
    check("assigned slot resolves file", p5store::slot(9).kind == p5store::SourceKind::File && p5store::slot(9).file == "TEST PATCH");
    check("map saved+reloaded", p5store::saveMap() && p5store::loadMap() && p5store::slot(9).file == "TEST PATCH");
    {   p5store::Source q; q.kind = p5store::SourceKind::File; q.file = "TEST PATCH";
        int sl = -1;
        check("findSlotFor reverse lookup", p5store::findSlotFor(q, sl) && sl == 9);
        p5store::Source none; none.file = "GHOST";
        check("findSlotFor misses unbound", !p5store::findSlotFor(none, sl));
    }
    std::filesystem::remove_all(tmp);
    {   // baseDir contract (2026-10-20 regression): XDG_DOCUMENTS_DIR names a
        // PARENT CANDIDATE, never the store itself — desktop sessions export
        // it, and a raw return lost the /Muhammad Synth level: real users saw
        // an empty USER page with patches on disk. Test WITHOUT P5_STORE_DIR.
        unsetenv("P5_STORE_DIR");
        char fh[] = "/tmp/muh-fakehome-XXXXXX";
        if (mkdtemp(fh))
        {   setenv("HOME", fh, 1);
            const std::string docs = std::string(fh) + "/Documents";
            std::filesystem::create_directories(docs);
            setenv("XDG_DOCUMENTS_DIR", docs.c_str(), 1);
            check("XDG parent + suffix", p5store::baseDir() == docs + "/Muhammad Synth");
            setenv("XDG_DOCUMENTS_DIR", "$HOME/Documents", 1);   // unexpanded shape
            check("$HOME form expands", p5store::baseDir() == docs + "/Muhammad Synth");
            unsetenv("XDG_DOCUMENTS_DIR");
            check("plain ~/Documents fallback", p5store::baseDir() == docs + "/Muhammad Synth");
            std::filesystem::remove_all(fh);
        }
        setenv("P5_STORE_DIR", tmp, 1);
    }
    std::printf(fails ? "store: FAILED\n" : "store: all ok\n");
    return fails ? 1 : 0;
}
