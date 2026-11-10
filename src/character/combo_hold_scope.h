#pragma once
#include <cstdint>

namespace ComboPlayback {
// A pending hold belongs to one visit of one unchanged chain. Node indices alone
// cannot distinguish a loop back to the same node or an edited configuration.
struct HoldScope {
    int owner=-2;
    uint64_t generation=0,visit=0;
    bool Matches(int current_owner,uint64_t current_generation,uint64_t current_visit)const {
        return owner!=-2&&owner==current_owner&&generation==current_generation&&visit==current_visit;
    }
};
}
