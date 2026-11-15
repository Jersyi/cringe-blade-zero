// Grab state leases on a scripted weapons component (the production CaptureWith / VerifyOrRestoreWith with a fake IO):
// a Main index the player's layout lacks (沐小葵 01 asks for 2; the native SetCurrentWeapon refuses it and changes nothing,
// 11-15 probe), a layout that has it, a foreign change, and DuYan's armed switch.
#include "../src/grab/grab_state_leases.h"
#include <cstdio>
#include <string>

using namespace GrabStateLeases;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }

unsigned char actor_bytes[16], weapons_bytes[16], config_bytes[8][16];
struct FakeIO {
    int count = 2, committed = 1; bool armed = false, dead = false;
    int set_index_calls = 0, set_armed_calls = 0;
    ue::Object* actor() { return actor_bytes; }
    Identity Identify(ue::Object* p) { Identity id; id.object = p; id.name.index = 7; id.weak = {1, 1}; return id; }
    bool Same(const Identity& id) { return id.object != nullptr; }
    bool IsDead(ue::Object*, bool& value) { value = dead; return true; }
    bool Capsule(ue::Object*, ue::Object*& capsule) { capsule = nullptr; return false; }
    bool Collision(ue::Object*, uint8_t&) { return false; }
    bool SetCollision(ue::Object*, uint8_t) { return false; }
    bool Weapons(ue::Object*, ue::Object*& weapons) { weapons = weapons_bytes; return true; }
    bool Main(ue::Object*, MainState& out) {
        out = {}; out.storage = config_bytes; out.count = count; out.capacity = count; out.committed = out.pending = committed; out.armed = armed;
        out.tag.index = 3;
        for (int i = 0; i < count; ++i) { out.configs.push_back(config_bytes[i]); out.interfaces.push_back(0); }
        return true;
    }
    bool Armed(ue::Object*, ue::Name, bool& value) { value = armed; return true; }
    // The native SetCurrentWeapon: an index past the layout, or the current one, returns false and changes nothing.
    bool SetIndex(ue::Object*, ue::Name, int index) { ++set_index_calls; if (index < 0 || index >= count || index == committed) return false; committed = index; return true; }
    bool SetArmed(ue::Object*, ue::Name, bool value) { ++set_armed_calls; armed = value; return true; }
    // GAA_SwitchWeapon on this side: Begin caches and switches (arming only when the switch returned true and Armed), End
    // switches back to the cached index (ResetOnEnd).
    int cached = -1;
    void Begin(int index, bool arm) { cached = committed; const bool ok = index >= 0 && index < count && index != committed; if (ok) committed = index; if (ok && arm) armed = true; }
    void End() { if (cached >= 0 && cached < count) committed = cached; }
};
// Policy rows as generated (motian_grab_data.generated.h): collision, index, armed, required, collision mask, index mask, armed, armed mask.
const Policy kMuXiaoKui01{false, true, false, 2, 0, 4, false, 0};
const Policy kDuYan01{false, true, true, 1, 0, 2, true, 0};
const Policy kNone{};
}

int main() {
    std::wstring reason;
    {   // 沐小葵 01 on the player's two-weapon slot: accepted, the native switch is refused, nothing to restore.
        FakeIO io; Ledger ledger;
        Check(CaptureWith(io, io.actor(), nullptr, kMuXiaoKui01, kNone, ledger, reason), "MuXiaoKui 01 captured on a 2-weapon slot");
        Check(ledger.source.index_absent, "index 2 noted as absent");
        Check(ledger.source.index_pending && !ledger.source.armed_pending, "index leased, armed not");
        io.Begin(2, false);
        Check(io.committed == 1 && !io.armed, "native switch to 2 changes nothing");
        io.End();
        Check(VerifyOrRestoreWith(io, ledger, reason, true), "restore completes");
        Check(!ledger.HasPending() && !ledger.preserved, "nothing pending or preserved");
        Check(io.set_index_calls == 0 && io.set_armed_calls == 0, "the lease made no native writes");
    }
    {   // Index masks past the layout (a required index far beyond it) are accepted the same way.
        FakeIO io; Ledger ledger; Policy beyond = kMuXiaoKui01; beyond.required_main_index = 9; beyond.allowed_index_mask = 1u << 9;
        Check(CaptureWith(io, io.actor(), nullptr, beyond, kNone, ledger, reason), "index 9 captured");
        Check(ledger.source.index_absent, "index 9 absent");
        Check(VerifyOrRestoreWith(io, ledger, reason, true) && !ledger.preserved, "index 9 restore clean");
    }
    {   // A three-weapon slot that has index 2: the switch happens and the lease puts the player's index back.
        FakeIO io; io.count = 3; io.committed = 0; Ledger ledger;
        Check(CaptureWith(io, io.actor(), nullptr, kMuXiaoKui01, kNone, ledger, reason), "MuXiaoKui 01 captured on a 3-weapon slot");
        Check(!ledger.source.index_absent, "index 2 present");
        io.Begin(2, false);
        Check(io.committed == 2, "native switch to 2 applied");
        // The ability was cut before its End ran: the lease restores.
        Check(VerifyOrRestoreWith(io, ledger, reason, true), "restore after a cut");
        Check(io.committed == 0 && io.set_index_calls == 1 && !ledger.preserved, "index 0 restored once");
    }
    {   // The player changed weapon during the grab (an index outside the move's own): preserved, not undone.
        FakeIO io; Ledger ledger;
        Check(CaptureWith(io, io.actor(), nullptr, kMuXiaoKui01, kNone, ledger, reason), "captured before a foreign change");
        io.committed = 0;
        Check(VerifyOrRestoreWith(io, ledger, reason, true), "foreign change retires the lease");
        Check(io.committed == 0 && io.set_index_calls == 0 && (ledger.preserved & ForeignValue), "foreign index kept");
    }
    {   // DuYan 01 (bow at 1, drawn) on the two-weapon slot from index 0, sheathed: index and armed state come back.
        FakeIO io; io.committed = 0; io.armed = false; Ledger ledger;
        Check(CaptureWith(io, io.actor(), nullptr, kDuYan01, kNone, ledger, reason), "DuYan 01 captured");
        Check(!ledger.source.index_absent, "index 1 present");
        io.Begin(1, true);
        Check(io.committed == 1 && io.armed, "DuYan switch drew index 1");
        Check(VerifyOrRestoreWith(io, ledger, reason, true), "DuYan restore");
        Check(io.committed == 0 && !io.armed && !ledger.preserved, "index 0 sheathed again");
    }
    {   // An ability still running is never restored under it.
        FakeIO io; Ledger ledger;
        Check(CaptureWith(io, io.actor(), nullptr, kMuXiaoKui01, kNone, ledger, reason), "captured while running");
        Check(!VerifyOrRestoreWith(io, ledger, reason, false) && ledger.HasPending(), "waits for the native end");
        Check(VerifyOrRestoreWith(io, ledger, reason, true), "then restores");
    }
    {   // Invalid policies are still refused.
        FakeIO io; Ledger ledger; Policy bad = kMuXiaoKui01; bad.allowed_index_mask = 0;
        Check(!CaptureWith(io, io.actor(), nullptr, bad, kNone, ledger, reason), "index lease without a mask refused");
        bad = kMuXiaoKui01; bad.required_main_index = 32;
        Check(!CaptureWith(io, io.actor(), nullptr, bad, kNone, ledger, reason), "index 32 refused");
        io.dead = true;
        Check(!CaptureWith(io, io.actor(), nullptr, kMuXiaoKui01, kNone, ledger, reason), "dead actor refused");
    }
    std::printf("grab_state_leases_test checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
