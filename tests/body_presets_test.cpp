// 身材 / 骨骼: every bundled preset and animation (samples/UUEPBS Presets) loads with the UUEPBS editor's own readers.
#include "presets.hpp"
#include "anim.hpp"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    const std::filesystem::path folder = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("samples/UUEPBS Presets");
    int failures = 0;
    uuepbs::PresetShelf shelf;
    shelf.set_folder(folder);
    const std::vector<std::string> names = shelf.list();
    size_t bones = 0;
    for (const std::string& name : names) {
        uuepbs::EditBook book;
        uuepbs::LocoBookMap loco;
        uuepbs::MorphBook morphs;
        std::string message;
        if (!shelf.load(name, book, &loco, message, &morphs)) { ++failures; std::printf("preset %s: %s\n", name.c_str(), message.c_str()); continue; }
        bones += book.size();
    }
    std::vector<std::string> errors;
    const std::vector<uuepbs::Animation> animations = uuepbs::anim_load_all(folder, &errors);
    for (const std::string& e : errors) { ++failures; std::printf("animation: %s\n", e.c_str()); }
    size_t tracks = 0;
    for (const uuepbs::Animation& a : animations) tracks += a.tracks.size();
    // The package ships only the three example presets of github.com/XTGMods/UUEPBS (presets/, commit 7c54929) and no animations
    // (user 11-13: the contributor's own presets and animations are not shipped).
    if (names.size() != 3 || !animations.empty()) { ++failures; std::printf("expected the 3 UUEPBS example presets and no animations\n"); }
    std::printf("{\"presets\":%zu,\"edited_bones\":%zu,\"animations\":%zu,\"tracks\":%zu,\"failures\":%d}\n", names.size(), bones, animations.size(), tracks, failures);
    return failures ? 1 : 0;
}
