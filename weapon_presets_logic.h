#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace WeaponPresetsLogic {
// DualSense full USB/Bluetooth input layouts verified against SDL's PS5 driver.
// Never interpret the Bluetooth 10-byte simple report as a touch report.
struct Touch { bool down = false; uint8_t id = 0; int x = 0, y = 0; };
inline uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; ++i) { crc ^= p[i]; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
inline bool DecodeTouch(const uint8_t* report, size_t size, Touch& touch) {
    if (!report) return false;
    size_t base = 0;
    if (size == 64 && report[0] == 0x01) base = 1;
    else if (size == 78 && report[0] == 0x31) {
        const uint8_t seed = 0xa1;
        uint32_t stored = 0; for (int i = 0; i < 4; ++i) stored |= static_cast<uint32_t>(report[74 + i]) << (i * 8);
        if (stored != ~Crc(report, 74, Crc(&seed, 1, 0xffffffffu))) return false;
        base = 2;
    } else return false;
    const uint8_t* point = report + base + 32;
    // Use only one finger. A second contact cancels rather than accidentally
    // turning a two-finger/system gesture into an equipment request.
    if (!(point[4] & 0x80)) return false;
    touch.down = !(point[0] & 0x80); touch.id = point[0] & 0x7f;
    touch.x = point[1] | ((point[2] & 0x0f) << 8);
    touch.y = (point[2] >> 4) | (point[3] << 4);
    if (touch.down && (touch.x >= 1920 || touch.y >= 1080)) return false;
    return true;
}
struct Swipe {
    bool tracking = false, wait_release = true;
    uint8_t contact = 0;
    int start_x = 0, start_y = 0, last_x = 0, last_y = 0;
    uint64_t start_time = 0, last_report = 0, last_swipe = 0;
    void Reset() { tracking = false; wait_release = true; }
    // Fire only on release after a horizontal stroke; one touch = one switch.
    bool Feed(const Touch& t, uint64_t now, bool allowed) {
        if (!allowed || (last_report && now - last_report > 300)) Reset();
        last_report = now;
        if (!allowed) return false;
        if (!t.down) {
            const bool fire = tracking && now >= start_time && now - start_time <= 900 &&
                std::abs(last_x - start_x) >= 320 && std::abs(last_y - start_y) <= 220 &&
                std::abs(last_x - start_x) > std::abs(last_y - start_y) * 2 &&
                (!last_swipe || now - last_swipe >= 400);
            tracking = false; wait_release = false;
            if (fire) last_swipe = now;
            return fire;
        }
        if (wait_release) return false;
        if (!tracking) { tracking = true; contact = t.id; start_x = t.x; start_y = t.y; start_time = now; }
        else if (contact != t.id) { Reset(); return false; }
        last_x = t.x; last_y = t.y;
        return false;
    }
};

template<class ID> struct Step { bool main = true; int index = 0; ID id{}; };
template<class ID> bool ValidIds(const std::vector<ID>& ids) {
    if (ids.empty() || ids.size() > 2) return false;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i].None()) return false;
        for (size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return false;
    }
    return true;
}
template<class ID> bool Plan(std::vector<ID> current, const std::vector<ID>& target,
    const std::vector<ID>& owned, bool main, std::vector<Step<ID>>& steps) {
    if (!ValidIds(current) || !ValidIds(target) || current.size() != target.size()) return false;
    for (const auto& id : target) if (std::find(owned.begin(), owned.end(), id) == owned.end()) return false;
    for (size_t i = 0; i < target.size(); ++i) {
        if (current[i] == target[i]) continue;
        const auto found = std::find(current.begin(), current.end(), target[i]);
        if (found != current.end()) {
            const size_t other = static_cast<size_t>(found - current.begin());
            if (main) current[other] = current[i]; // Native normal-weapon replacement swaps duplicates.
            else {
                // Native Shaqi replacement rejects already-equipped IDs. Use an
                // existing unequipped item as a cycle breaker, never grant one.
                auto spare = std::find_if(owned.begin(), owned.end(), [&](const ID& id) {
                    return !id.None() && std::find(current.begin(), current.end(), id) == current.end() &&
                        std::find(target.begin(), target.end(), id) == target.end();
                });
                if (spare == owned.end()) return false;
                steps.push_back({false, static_cast<int>(other), *spare}); current[other] = *spare;
            }
        }
        steps.push_back({main, static_cast<int>(i), target[i]}); current[i] = target[i];
    }
    return current == target;
}
}
