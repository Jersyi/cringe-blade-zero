#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace SupplementSequence {
struct Key { std::wstring row; int actor = -1, node = -1; };
inline bool Parse(std::wstring_view text, Key& out) {
    out = {};
    const auto first = text.find(L'#'), last = text.rfind(L'#');
    if (first == text.npos || first == 0 || first == last) return false;
    auto number = [](std::wstring_view part, int& value) {
        if (part.empty()) return false;
        value = 0;
        for (wchar_t c : part) {
            if (c < L'0' || c > L'9' || value > ((std::numeric_limits<int>::max)() - (c-L'0'))/10) return false;
            value = value*10 + c-L'0';
        }
        return true;
    };
    if (!number(text.substr(first+1,last-first-1),out.actor) || !number(text.substr(last+1),out.node)) return false;
    out.row = text.substr(0,first);
    return true;
}
// The Blueprint waits for a world tick before travel/spawn. Never dispatch it while paused,
// nor replace its persistent SeqID while the previous latent action is still pending.
struct Request {
    enum class Step { Wait, Invoke, Started, Timeout };
    int item = -1;
    uint64_t since = 0;
    bool invoked = false;
    bool Begin(int index, uint64_t now) {
        if (item >= 0 || index < 0) return false;
        item=index; since=now; invoked=false; return true;
    }
    Step Poll(uint64_t now, bool world_ready, bool paused, bool playing) {
        if (item < 0) return Step::Wait;
        if (invoked && playing) return Step::Started;
        if (now-since >= 45000) return Step::Timeout;
        if (!invoked && world_ready && !paused) { invoked=true; return Step::Invoke; }
        return Step::Wait;
    }
    void Clear() { *this={}; }
};
}
