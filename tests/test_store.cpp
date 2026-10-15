// Patch store round-trips: save/list/load + bank/program retrieval map.
#include "p5_store.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    uint8_t raw[133]; std::memset(raw, 7, sizeof raw);
    std::memcpy(raw + 65, "TEST PATCH", 10);
    check("save", p5store::savePatch("TEST PATCH", raw));
    check("list contains it", [&]{ auto v = p5store::listPatches();
        for (auto &s : v) if (s == "TEST PATCH") return true; return false; }());
    uint8_t back[133] = {};
    check("load roundtrip", p5store::loadPatch("TEST PATCH", back) && !std::memcmp(raw, back, 133));
    check("sanitize keeps case", p5store::sanitize("a/b\\c*?") == "abc");
    check("sanitize faithful mixed", p5store::sanitize("Peace Be 5") == "Peace Be 5");
    check("foldCollision finds PEACE vs peace", [&]{
        uint8_t r2[133]; std::memset(r2, 3, sizeof r2);
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
    std::filesystem::remove_all(tmp);
    std::printf(fails ? "store: FAILED\n" : "store: all ok\n");
    return fails ? 1 : 0;
}
