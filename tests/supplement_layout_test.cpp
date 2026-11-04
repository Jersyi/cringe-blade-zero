#include "debug_supplement_catalog.generated.h"
#include "supplement_layout.h"
#include <cstdio>
#include <cwchar>
#include <iterator>

// The supplement page shows every catalog item exactly once, and a card's options fit its layout.
int main() {
    constexpr size_t kCount = std::size(DebugSupplementCatalog::kItems);
    int seen[kCount]{}, failures = 0, cards = 0;
    for (const auto& section : SupplementLayout::kSections) {
        if (!section.title || !section.count) { ++failures; std::printf("FAIL empty section\n"); }
        for (size_t c = 0; c < section.count; ++c) {
            const auto& card = section.cards[c];
            ++cards;
            size_t options = 0;
            for (const auto& option : card.options) {
                if (!option.label) { if (option.item != -1) { ++failures; std::printf("FAIL unlabeled option %d\n", option.item); } continue; }
                if (options++ != static_cast<size_t>(&option - card.options.data())) { ++failures; std::printf("FAIL gap in %ls\n", card.title); }
                if (option.item < 0 || static_cast<size_t>(option.item) >= kCount) { ++failures; std::printf("FAIL item %d out of range\n", option.item); continue; }
                ++seen[option.item];
            }
            if (!options || !card.title || !card.note) { ++failures; std::printf("FAIL card %ls\n", card.title ? card.title : L"?"); }
            if (card.remembers && options < 2) { ++failures; std::printf("FAIL %ls remembers one option\n", card.title); }
        }
    }
    for (size_t i = 0; i < kCount; ++i)
        if (seen[i] != 1) { ++failures; std::printf("FAIL item %zu (%ls) shown %d times\n", i, DebugSupplementCatalog::kItems[i].label, seen[i]); }
    std::printf("%zu sections, %d cards, %zu items, %d failures\n", std::size(SupplementLayout::kSections), cards, kCount, failures);
    return failures ? 1 : 0;
}
