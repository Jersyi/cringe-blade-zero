// Offline checks for the two-handed grip contact in npc_pose_transfer.h: a hero with long
// arms holds both palms close together; the NPC's shorter arms are retargeted by rotation,
// then the left arm IK must put the NPC's left palm at the hero's palm offset from the
// right palm, keep bone lengths and the hand's rotation, and fade out between 60 and 90 cm.
#include "npc_pose_transfer.h"
#include "npc_body_collision.h"
#include "npc_grip_routes.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using NpcPoseTransfer::Bone;
using NpcPoseTransfer::Transform;
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }

enum : int32_t { Root, Spine, UpperL, LowerL, HandL, PalmL, UpperR, LowerR, HandR, PalmR, Count };
constexpr int32_t kParents[Count] = {-1, Root, Spine, UpperL, LowerL, HandL, Spine, UpperR, LowerR, HandR};

Transform At(double x, double y, double z) { Transform t; t.translation[0] = x; t.translation[1] = y; t.translation[2] = z; return t; }
// Arms lie along +X (left) and -X (right) at rest; upper/lower/hand are the segment lengths.
std::vector<Bone> Skeleton(double spine, double upper, double lower, double hand) {
    const Transform locals[Count] = {At(0, 0, 0), At(0, 0, spine),
        At(20, 0, 40), At(upper, 0, 0), At(lower, 0, 0), At(hand, 0, 0),
        At(-20, 0, 40), At(-upper, 0, 0), At(-lower, 0, 0), At(-hand, 0, 0)};
    std::vector<Bone> bones(Count);
    for (int32_t i = 0; i < Count; ++i) bones[static_cast<size_t>(i)] = {static_cast<uint64_t>(1000 + i), kParents[i], locals[i]};
    return bones;
}
void Yaw(Transform& t, double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360;
    t.rotation[0] = t.rotation[1] = 0; t.rotation[2] = std::sin(half); t.rotation[3] = std::cos(half);
}
// Hero pose in component space: both arms swing forward by `shoulder` and bend by `elbow`
// in the horizontal plane, mirrored, so the palms meet in front of the chest.
std::vector<Transform> HeroPose(const std::vector<Bone>& hero, double shoulder, double elbow) {
    std::vector<Transform> local(Count), cs(Count);
    for (size_t i = 0; i < Count; ++i) local[i] = hero[i].ref;
    Yaw(local[UpperL], shoulder); Yaw(local[LowerL], elbow);
    Yaw(local[UpperR], -shoulder); Yaw(local[LowerR], -elbow);
    for (size_t i = 0; i < Count; ++i)
        if (kParents[i] < 0) cs[i] = local[i]; else NpcGripMath::Compose(local[i], cs[static_cast<size_t>(kParents[i])], cs[i]);
    return cs;
}
double Distance(const double* a, const double* b) { const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }
bool SameRotation(const Transform& a, const Transform& b) {
    const double dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
    return std::abs(std::abs(dot) - 1) < 1e-9;
}
bool SameTransform(const Transform& a, const Transform& b) { return Distance(a.translation, b.translation) < 1e-9 && SameRotation(a, b); }

struct Run { NpcPoseTransfer::Cache cache; bool ok = false; std::string why; };
Run Evaluate(NpcPoseTransfer::Plan plan, const std::vector<Transform>& pose) {
    Run run; NpcPoseTransfer::Prepare(plan, run.cache); const char* why = "";
    run.ok = NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), run.cache, why); run.why = why;
    return run;
}

// Same bone indices on both skeletons; bases default to identity (hands shaped alike).
NpcPoseTransfer::HandContact Request(const double* basis_l = nullptr, const double* basis_r = nullptr) {
    NpcPoseTransfer::HandContact k;
    k.source_hand_l = HandL; k.source_hand_r = HandR; k.source_palm_l = PalmL; k.source_palm_r = PalmR;
    k.upper = UpperL; k.lower = LowerL; k.hand = HandL; k.hand_r = HandR; k.palm_l = PalmL; k.palm_r = PalmR;
    for (size_t i = 0; i < 4; ++i) { if (basis_l) k.basis_l[i] = basis_l[i]; if (basis_r) k.basis_r[i] = basis_r[i]; }
    return k;
}
void Axis(double* q, double x, double y, double z, double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360, s = std::sin(half);
    q[0] = x * s; q[1] = y * s; q[2] = z * s; q[3] = std::cos(half);
}

void TestContact() {
    const auto hero = Skeleton(100, 30, 28, 8), npc = Skeleton(90, 25, 22, 6);
    NpcPoseTransfer::Plan plain; std::string why;
    Check(NpcPoseTransfer::Build(plain, hero, npc, {}, why), "plan builds");
    NpcPoseTransfer::Plan contact = plain;
    NpcPoseTransfer::HandContact swapped = Request(); swapped.lower = HandL; swapped.hand = LowerL;
    Check(!NpcPoseTransfer::SetHandContact(contact, swapped), "contact refuses a chain out of order");
    Check(NpcPoseTransfer::SetHandContact(contact, Request()), "contact accepts upper -> lower -> hand");

    // Close grip: palms well under 75 cm apart, full weight.
    const auto grip = HeroPose(hero, 60, 70);
    const double* hl = grip[PalmL].translation, *hr = grip[PalmR].translation;
    const double hero_gap = Distance(hl, hr), offset[3] = {hl[0] - hr[0], hl[1] - hr[1], hl[2] - hr[2]};
    Check(hero_gap < 75, "close pose has the palms under 75 cm apart");
    Run base = Evaluate(plain, grip), solved = Evaluate(contact, grip);
    Check(base.ok && solved.ok, "both plans evaluate");
    if (!base.ok || !solved.ok) { std::printf("  why=%s / %s\n", base.why.c_str(), solved.why.c_str()); return; }
    const auto& before = base.cache.target_cs, &after = solved.cache.target_cs;
    Check(std::abs(Distance(before[PalmL].translation, before[PalmR].translation) - hero_gap) > 1, "rotation-only retarget misses the hero's palm gap");
    Check(solved.cache.contacts == 1 && solved.cache.contact_weight == 1, "contact runs at full weight");
    double want[3]; for (size_t i = 0; i < 3; ++i) want[i] = after[PalmR].translation[i] + offset[i];
    std::printf("  palm gap: hero %.3f cm, NPC %.3f -> %.3f cm, contact error %.2e cm\n", hero_gap,
        Distance(before[PalmL].translation, before[PalmR].translation), Distance(after[PalmL].translation, after[PalmR].translation),
        Distance(after[PalmL].translation, want));
    Check(Distance(after[PalmL].translation, want) < 1e-6, "left palm lands at the hero's offset from the right palm");
    Check(std::abs(Distance(after[UpperL].translation, after[LowerL].translation) - 25) < 1e-9, "upper arm keeps its length");
    Check(std::abs(Distance(after[LowerL].translation, after[HandL].translation) - 22) < 1e-9, "forearm keeps its length");
    Check(std::abs(Distance(after[HandL].translation, after[PalmL].translation) - 6) < 1e-9, "hand keeps its length");
    Check(!SameTransform(after[UpperL], before[UpperL]) && SameRotation(after[HandL], before[HandL]), "arm bends while the hand keeps its rotation");
    bool untouched = true;
    for (int32_t b : {Root, Spine, UpperR, LowerR, HandR, PalmR}) untouched = untouched && SameTransform(after[static_cast<size_t>(b)], before[static_cast<size_t>(b)]);
    Check(untouched && Distance(after[UpperL].translation, before[UpperL].translation) < 1e-9, "spine, right arm and shoulder stay put");
    bool synthetic = true;
    for (size_t s = 0; s < Count; ++s) synthetic = synthetic && SameTransform(solved.cache.synthetic_source[s], after[s]);
    Check(synthetic, "synthetic source carries the solved arm");
    const char* again = ""; NpcPoseTransfer::Evaluate(contact, grip.data(), grip.size(), solved.cache, again);
    Check(solved.cache.hits == 1 && solved.cache.contacts == 1, "unchanged source pose is a cache hit");

    // Out of reach: the arm stops short and the cache reports the residual.
    Run far = Evaluate(contact, HeroPose(hero, 60, 20));
    std::printf("  out of reach: residual %.2f cm\n", far.cache.contact_error_max);
    Check(far.ok && far.cache.contacts == 1 && far.cache.contact_error_max > 0.5, "unreachable contact reports its residual");

    // Mid grip: between 75 and 105 cm the palm moves part of the way (smoothstep).
    const auto mid = HeroPose(hero, 30, 60);
    const double mid_gap = Distance(mid[PalmL].translation, mid[PalmR].translation);
    Check(mid_gap > 75 && mid_gap < 105, "mid pose has the palms between 75 and 105 cm apart");
    Run mid_base = Evaluate(plain, mid), mid_solved = Evaluate(contact, mid);
    if (mid_base.ok && mid_solved.ok) {
        const double x = (105 - mid_gap) / 30, weight = x * x * (3 - 2 * x);
        double mid_offset[3], expect[3];
        for (size_t i = 0; i < 3; ++i) mid_offset[i] = mid[PalmL].translation[i] - mid[PalmR].translation[i];
        for (size_t i = 0; i < 3; ++i) {
            const double palm = mid_base.cache.target_cs[PalmL].translation[i];
            expect[i] = palm + (mid_solved.cache.target_cs[PalmR].translation[i] + mid_offset[i] - palm) * weight;
        }
        std::printf("  mid grip: hero gap %.3f cm, weight %.3f, error %.2e cm\n", mid_gap, mid_solved.cache.contact_weight,
            Distance(mid_solved.cache.target_cs[PalmL].translation, expect));
        Check(std::abs(mid_solved.cache.contact_weight - weight) < 1e-12 && weight > 0.1 && weight < 0.9, "mid grip uses the smoothstep weight");
        Check(Distance(mid_solved.cache.target_cs[PalmL].translation, expect) < 1e-6, "mid grip moves the palm part of the way");
    } else Check(false, "mid pose evaluates");

    // Open arms: palms 105 cm or more apart, the contact leaves the pose alone.
    const auto open = HeroPose(hero, 45, 0);
    Check(Distance(open[PalmL].translation, open[PalmR].translation) >= 105, "open pose has the palms 105 cm or more apart");
    Run open_base = Evaluate(plain, open), open_solved = Evaluate(contact, open);
    bool same = open_base.ok && open_solved.ok;
    for (size_t t = 0; same && t < Count; ++t) same = SameTransform(open_base.cache.target_cs[t], open_solved.cache.target_cs[t]);
    Check(same && open_solved.cache.contacts == 0 && open_solved.cache.contact_weight == 0, "open arms are not touched");
}

// NPC hands shaped differently from the hero's: the weapon adapter turns the weapon
// by R about the right palm. The left hand must sit on that turned weapon exactly as
// the hero's left hand sits on the hero's weapon.
void TestWeaponFrame() {
    const auto hero = Skeleton(100, 30, 28, 8), npc = Skeleton(90, 25, 22, 6);
    NpcPoseTransfer::Plan contact; std::string why;
    double basis_l[4], basis_r[4]; Axis(basis_r, 0, 0, 1, 15); Axis(basis_l, 0, 1, 0, -12);
    Check(NpcPoseTransfer::Build(contact, hero, npc, {}, why) && NpcPoseTransfer::SetHandContact(contact, Request(basis_l, basis_r)), "turned-hand plan builds");
    const auto grip = HeroPose(hero, 60, 70);
    Run solved = Evaluate(contact, grip);
    Check(solved.ok && solved.cache.contacts == 1, "turned-hand contact runs");
    if (!solved.ok) return;
    const auto& after = solved.cache.target_cs;
    // A weapon point held in the hero's left palm, mapped the way the adapter maps the weapon.
    Transform mount, identity, mapped; std::memcpy(mount.rotation, grip[HandL].rotation, sizeof(mount.rotation));
    std::memcpy(mount.translation, grip[PalmL].translation, sizeof(mount.translation));
    Check(NpcGripMath::MapSourceMountPose(grip.data(), after.data(), HandR, HandR, PalmR, PalmR, mount, basis_r, identity, mapped), "weapon adapter maps the left grip point");
    Transform held; NpcGripMath::Mul(after[HandL].rotation, basis_l, held.rotation);
    std::printf("  turned hands: left grip error %.2e cm, max %.2e cm\n", Distance(after[PalmL].translation, mapped.translation), solved.cache.contact_error_max);
    Check(Distance(after[PalmL].translation, mapped.translation) < 1e-6, "left palm lands on the turned weapon");
    Check(SameRotation(held, mapped), "left hand turns with the weapon");
    Check(solved.cache.contact_error_max < 1e-6, "contact reports no residual when reachable");
}
// A boss skeleton can hang a same-name bone under another parent (HuaShan eye_l / eye_r,
// BladeMan bns_skirt_3_01). That bone stays at the NPC reference pose; the rest still maps.
void TestParentMismatch() {
    const auto hero = Skeleton(100, 30, 28, 8);
    auto npc = Skeleton(90, 25, 22, 6);
    npc[PalmR].parent = LowerR; npc[PalmR].ref = At(-28, 0, 0);
    NpcPoseTransfer::Plan plan; std::string why;
    Check(NpcPoseTransfer::Build(plan, hero, npc, {}, why), "plan builds with one re-parented bone");
    Check(plan.parent_mismatch == 1 && plan.mapped == Count - 1, "only the re-parented bone is left unmapped");
    Check(plan.target_to_source[PalmR] == -1 && plan.source_to_target[PalmR] == -1, "re-parented bone is not mapped either way");
    const auto pose = HeroPose(hero, 45, 30);
    Run run = Evaluate(plan, pose);
    Check(run.ok, "evaluate with a re-parented bone");
    Transform expected; NpcGripMath::Compose(npc[PalmR].ref, run.cache.target_cs[LowerR], expected);
    Check(run.ok && SameTransform(run.cache.target_cs[PalmR], expected), "re-parented bone keeps its NPC reference local");
    Check(run.ok && SameTransform(run.cache.synthetic_source[PalmR], pose[PalmR]), "unmapped source bone passes through unchanged");
    std::printf("parent mismatch: kept=%zu mapped=%zu\n", plan.parent_mismatch, plan.mapped);
}
double Angle(const double* a0, const double* a1, const double* b0, const double* b1) {
    const double a[3] = {a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2]}, b[3] = {b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2]};
    const double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
    return std::acos((std::max)(-1.0, (std::min)(1.0, c))) * 57.29577951308232;
}
// An NPC whose rest-pose left arm is raised 30 deg, its forearm bent 15 deg and its hand
// rolled 25 deg (梦华 phase 2 / 花千笑). Without a retarget pose those angles stay in every
// frame. Matched to the hero's rest pose, the NPC arm points where the hero's does, a sleeve
// bone the hero lacks rides along, and the weapon adapter's hand turn is identity.
void TestRestTurn() {
    const auto hero = Skeleton(100, 30, 28, 8);
    auto npc = Skeleton(90, 25, 22, 6);
    Axis(npc[UpperL].ref.rotation, 0, 1, 0, -30); Axis(npc[LowerL].ref.rotation, 0, 0, 1, 15); Axis(npc[HandL].ref.rotation, 1, 0, 0, 25);
    npc.push_back({5000, UpperL, At(10, 0, -3)});
    const size_t sleeve = npc.size() - 1;
    NpcPoseTransfer::Plan plain, matched; std::string why;
    Check(NpcPoseTransfer::Build(plain, hero, npc, {}, why) && NpcPoseTransfer::Build(matched, hero, npc, {}, why), "rest-turn plans build");
    double upper_deg = 0, fore_deg = 0, basis[4], inverse[4], tmp[4], turn[4];
    Check(NpcPoseTransfer::AlignRestDirection(matched, UpperL, LowerL, &upper_deg) && NpcPoseTransfer::AlignRestDirection(matched, LowerL, HandL, &fore_deg), "arm directions align");
    Axis(basis, 0, 0, 1, 15);  // hand shaped differently: the finger-geometry basis is not identity
    NpcGripMath::Inverse(basis, inverse); NpcGripMath::Mul(matched.source_ref_cs[HandL].rotation, inverse, tmp);
    NpcGripMath::Inverse(matched.target_ref_cs[HandL].rotation, inverse); NpcGripMath::Mul(tmp, inverse, turn);
    Check(NpcPoseTransfer::SetRestTurn(matched, HandL, turn), "hand frame turn accepted");
    Check(!NpcPoseTransfer::SetRestTurn(matched, static_cast<int32_t>(sleeve), turn), "unmapped bone refuses a turn");
    Check(NpcPoseTransfer::FinishRestTurns(matched) && NpcPoseTransfer::FinishRestTurns(plain), "rest turns resolve");
    std::printf("  rest turn: upper %.2f deg, fore %.2f deg\n", upper_deg, fore_deg);
    Check(std::abs(upper_deg - 30) < 1e-6 && std::abs(fore_deg - 15) < 1e-6, "each bone turns only by its own missing arc");
    const auto& rest = matched.target_rest_cs; const auto& hero_rest = matched.source_ref_cs;
    Check(Angle(rest[UpperL].translation, rest[LowerL].translation, hero_rest[UpperL].translation, hero_rest[LowerL].translation) < 1e-4 &&
          Angle(rest[LowerL].translation, rest[HandL].translation, hero_rest[LowerL].translation, hero_rest[HandL].translation) < 1e-4, "matched rest arm points along the hero's");
    Check(std::abs(Distance(rest[UpperL].translation, rest[LowerL].translation) - 25) < 1e-9 && std::abs(Distance(rest[LowerL].translation, rest[HandL].translation) - 22) < 1e-9, "matched rest keeps NPC bone lengths");
    Check(plain.rest_mode[UpperL] == 0 && matched.rest_mode[UpperL] == 1 && matched.rest_mode[PalmL] == 1 && matched.rest_mode[sleeve] == 2 && matched.rest_mode[UpperR] == 0, "rest modes: turned, inherited, follower, untouched");

    const auto pose = HeroPose(hero, 45, 30);
    Run before = Evaluate(plain, pose), after = Evaluate(matched, pose);
    Check(before.ok && after.ok, "rest-turn plans evaluate");
    if (!before.ok || !after.ok) { std::printf("  why=%s / %s\n", before.why.c_str(), after.why.c_str()); return; }
    const auto& b = before.cache.target_cs, &a = after.cache.target_cs;
    const double old_upper = Angle(b[UpperL].translation, b[LowerL].translation, pose[UpperL].translation, pose[LowerL].translation);
    const double new_upper = Angle(a[UpperL].translation, a[LowerL].translation, pose[UpperL].translation, pose[LowerL].translation);
    const double new_fore = Angle(a[LowerL].translation, a[HandL].translation, pose[LowerL].translation, pose[HandL].translation);
    NpcGripMath::Inverse(pose[HandL].rotation, inverse); NpcGripMath::Mul(a[HandL].rotation, basis, tmp); NpcGripMath::Mul(tmp, inverse, turn);
    const double weapon_deg = NpcPoseTransfer::detail::Degrees(turn);
    std::printf("  posed arm off the hero's: upper %.2f -> %.2e deg, fore %.2e deg, weapon turn %.2e deg\n", old_upper, new_upper, new_fore, weapon_deg);
    Check(old_upper > 20, "without a retarget pose the NPC arm keeps its rest offset");
    Check(new_upper < 1e-4 && new_fore < 1e-4, "matched NPC arm follows the hero's arm directions");
    Check(weapon_deg < 1e-4, "weapon adapter's hand turn is identity");
    Transform follower; NpcGripMath::ComposeMount(npc[sleeve].ref, a[UpperL], follower);
    Check(SameTransform(a[sleeve], follower) && SameTransform(after.cache.target_local[sleeve], npc[sleeve].ref), "sleeve bone rides the turned upper arm");
    bool right = true;
    for (int32_t r : {Root, Spine, UpperR, LowerR, HandR, PalmR}) right = right && SameTransform(a[static_cast<size_t>(r)], b[static_cast<size_t>(r)]);
    Check(right, "unturned bones are unchanged");
    Check(Distance(a[UpperL].translation, b[UpperL].translation) < 1e-9, "shoulder stays put");
    Transform palm; NpcGripMath::ComposeMount(after.cache.target_local[PalmL], a[HandL], palm);
    Check(SameTransform(a[PalmL], palm), "target_local agrees with target_cs after the turn");
}
std::vector<Transform> ComposeAll(const std::vector<Bone>& bones, const std::vector<Transform>& local) {
    std::vector<Transform> cs(bones.size());
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].parent < 0) cs[i] = local[i]; else NpcGripMath::Compose(local[i], cs[static_cast<size_t>(bones[i].parent)], cs[i]);
    return cs;
}
// Palms 24 cm apart do not make a two-handed grip. The contact runs only while the hero's
// left fist (hand_l * the rest weapon_l offset) sits on the weapon_r handle axis (local Z)
// with the two handle axes parallel; a free left hand at the same spot is left alone.
void TestGripGate() {
    auto hero = Skeleton(100, 30, 28, 8);
    const auto npc = Skeleton(90, 25, 22, 6);
    const int32_t WeaponR = Count, WeaponL = Count + 1;
    hero.push_back({2000, HandR, At(0, 4, 0)}); hero.push_back({2001, HandL, At(0, 4, 0)});
    std::vector<Transform> local(hero.size());
    for (size_t i = 0; i < hero.size(); ++i) local[i] = hero[i].ref;
    Yaw(local[UpperL], 60); Yaw(local[LowerL], 70); Yaw(local[UpperR], -60); Yaw(local[LowerR], -70);
    // Put the hero's left fist 12 cm up the right weapon's handle by giving weapon_l that rest offset.
    auto cs = ComposeAll(hero, local);
    Transform on_handle = cs[WeaponR], grip_rest; double axis[3]; const double up[3] = {0, 0, 1};
    NpcGripMath::Rotate(cs[WeaponR].rotation, up, axis); for (size_t i = 0; i < 3; ++i) on_handle.translation[i] += axis[i] * 12;
    Check(NpcGripMath::Relative(on_handle, cs[HandL], grip_rest), "grip offset solves");
    hero[WeaponL].ref = local[WeaponL] = grip_rest;
    const auto gripped = ComposeAll(hero, local);
    auto free_local = local; Axis(free_local[HandL].rotation, 1, 0, 0, 90);  // same palm, fist rolled off the handle
    const auto free_hand = ComposeAll(hero, free_local);
    Check(Distance(free_hand[PalmL].translation, free_hand[PalmR].translation) < 75, "free hand is inside the old 75 cm palm gate");

    NpcPoseTransfer::Plan plain, gated; std::string why;
    NpcPoseTransfer::HandContact request = Request(); request.source_weapon_r = WeaponR; request.grip_l = hero[WeaponL].ref;
    Check(NpcPoseTransfer::Build(plain, hero, npc, {}, why) && NpcPoseTransfer::Build(gated, hero, npc, {}, why) && NpcPoseTransfer::SetHandContact(gated, request), "gated plan builds");
    NpcPoseTransfer::HandContact bad = request; bad.grip_none_cm = bad.grip_full_cm;
    NpcPoseTransfer::Plan refused = plain; Check(!NpcPoseTransfer::SetHandContact(refused, bad), "gate refuses an empty fade band");
    Run held = Evaluate(gated, gripped), loose = Evaluate(gated, free_hand), loose_plain = Evaluate(plain, free_hand);
    Check(held.ok && loose.ok && loose_plain.ok, "gated plans evaluate");
    if (!held.ok || !loose.ok || !loose_plain.ok) return;
    const double* hl = gripped[PalmL].translation, *hr = gripped[PalmR].translation; double want[3];
    for (size_t i = 0; i < 3; ++i) want[i] = held.cache.target_cs[PalmR].translation[i] + hl[i] - hr[i];
    std::printf("  grip gate: held weight %.3f error %.2e cm; free hand contacts %llu rejected %llu\n", held.cache.contact_weight,
        Distance(held.cache.target_cs[PalmL].translation, want), static_cast<unsigned long long>(loose.cache.contacts), static_cast<unsigned long long>(loose.cache.grip_rejected));
    Check(held.cache.contacts == 1 && held.cache.contact_weight == 1 && Distance(held.cache.target_cs[PalmL].translation, want) < 1e-6, "fist on the handle: contact at full weight");
    bool same = loose.cache.contacts == 0 && loose.cache.grip_rejected == 1;
    for (size_t t = 0; same && t < Count; ++t) same = SameTransform(loose.cache.target_cs[t], loose_plain.cache.target_cs[t]);
    Check(same, "free left hand near the weapon is not pulled");
}
std::vector<Bone> Scaled(std::vector<Bone> bones, double k) {
    for (Bone& b : bones) for (size_t i = 0; i < 3; ++i) b.ref.translation[i] *= k;
    return bones;
}
// The fixture has no head or fingers: the spine top stands in for the head, and the palm bone for
// middle_01 and index_01 with the hand for pinky_01 (knuckle width = hand length).
NpcPoseTransfer::SizeBones FixtureSize() {
    NpcPoseTransfer::SizeBones b; b.head = Spine;
    const int32_t arms[2][4] = {{UpperL, LowerL, HandL, PalmL}, {UpperR, LowerR, HandR, PalmR}};
    for (size_t s = 0; s < 2; ++s) { b.upper[s] = arms[s][0]; b.lower[s] = arms[s][1]; b.hand[s] = arms[s][2]; b.middle[s] = b.index[s] = arms[s][3]; b.pinky[s] = arms[s][2]; }
    return b;
}
// A body that is the hero scaled by 1.6 must hold the hero's weapon scaled by 1.6 at the same place
// relative to its hand, and its left hand must stay on that weapon's handle (the two-hand contact
// scales the hero's palm offset too). Unequal proportions average geometrically; a clamp and
// missing bones fall back as documented.
void TestWeaponScale() {
    const auto hero = Skeleton(100, 30, 28, 8), big = Scaled(hero, 1.6), odd = Skeleton(90, 25, 22, 6);
    const auto size = FixtureSize();
    NpcPoseTransfer::Plan plan, clamped, mixed, missing; std::string why; NpcPoseTransfer::BodyScale scale;
    Check(NpcPoseTransfer::Build(plan, hero, big, {}, why) && NpcPoseTransfer::Build(mixed, hero, odd, {}, why), "scaled plans build");
    clamped = plan; missing = plan;
    Check(NpcPoseTransfer::WeaponScale(plan, size, size, 0.5, 5, scale) && scale.used == 3 && std::abs(scale.raw - 1.6) < 1e-12 && plan.weapon_scale == scale.scale, "uniform body: all three ratios give 1.6");
    std::printf("  weapon scale: uniform %.6f (stature %.3f reach %.3f fist %.3f)\n", plan.weapon_scale, scale.ratio[0], scale.ratio[1], scale.ratio[2]);
    Check(NpcPoseTransfer::WeaponScale(clamped, size, size, 0.5, 1.5, scale) && scale.scale == 1.5 && std::abs(scale.raw - 1.6) < 1e-12 && clamped.weapon_scale == 1.5, "clamp limits the applied scale only");
    Check(NpcPoseTransfer::WeaponScale(mixed, size, size, 0.5, 5, scale), "unequal proportions measure");
    const double expect = std::cbrt(0.9 * (25.0 + 22.0) / (30.0 + 28.0) * 6.0 / 8.0);
    std::printf("  weapon scale: unequal %.6f expect %.6f\n", mixed.weapon_scale, expect);
    Check(std::abs(mixed.weapon_scale - expect) < 1e-12, "unequal proportions: geometric mean of stature, reach and fist");
    NpcPoseTransfer::SizeBones head_only; head_only.head = Spine; missing.weapon_scale = 2;
    Check(!NpcPoseTransfer::WeaponScale(missing, head_only, head_only, 0.5, 5, scale) && missing.weapon_scale == 1 && scale.used == 1, "one ratio is not enough: scale stays 1");

    NpcPoseTransfer::Plan contact = plan, unscaled = plan; unscaled.weapon_scale = 1;
    Check(NpcPoseTransfer::SetHandContact(contact, Request()) && NpcPoseTransfer::SetHandContact(unscaled, Request()), "scaled contact plans build");
    const auto grip = HeroPose(hero, 60, 70);
    Run base = Evaluate(plan, grip), held = Evaluate(contact, grip), wrong = Evaluate(unscaled, grip);
    Check(base.ok && held.ok && wrong.ok, "scaled plans evaluate");
    if (!base.ok || !held.ok || !wrong.ok) return;
    bool uniform = true;
    for (size_t t = 0; t < Count; ++t) { double want[3]; for (size_t i = 0; i < 3; ++i) want[i] = grip[t].translation[i] * 1.6; uniform = uniform && Distance(base.cache.target_cs[t].translation, want) < 1e-9; }
    Check(uniform, "fixture: the scaled body's pose is the hero's pose times 1.6");
    // A sword gripped 4 cm past the right palm, 90 cm long along hand Z, authored with a zero X scale.
    Transform local = At(0, 4, 0), mount, identity, mapped; Axis(local.rotation, 1, 0, 0, 80); local.scale[0] = 0;
    Check(NpcGripMath::ComposeMount(local, grip[PalmR], mount), "mount composes");
    Check(NpcGripMath::MapSourceMountPose(grip.data(), held.cache.target_cs.data(), HandR, HandR, PalmR, PalmR, mount, identity.rotation, identity, mapped, contact.weapon_scale), "weapon maps with the body scale");
    double want[3], tip[3], hero_tip[3]; const double blade[3] = {0, 0, 90};
    for (size_t i = 0; i < 3; ++i) want[i] = mount.translation[i] * 1.6;
    NpcGripMath::Rotate(mapped.rotation, blade, tip); NpcGripMath::Rotate(mount.rotation, blade, hero_tip);
    for (size_t i = 0; i < 3; ++i) { tip[i] = mapped.translation[i] + tip[i] * mapped.scale[2]; hero_tip[i] = (mount.translation[i] + hero_tip[i]) * 1.6; }
    std::printf("  scaled weapon: grip error %.2e cm, tip error %.2e cm, scale %.3f/%.3f/%.3f\n", Distance(mapped.translation, want), Distance(tip, hero_tip), mapped.scale[0], mapped.scale[1], mapped.scale[2]);
    Check(Distance(mapped.translation, want) < 1e-9 && Distance(tip, hero_tip) < 1e-9 && SameRotation(mapped, mount), "scaled body holds the scaled weapon where the hero holds its own");
    Check(mapped.scale[0] == 0 && std::abs(mapped.scale[1] - 1.6) < 1e-12 && std::abs(mapped.scale[2] - 1.6) < 1e-12, "weapon scale multiplies the authored scale, zero kept");
    Transform refused; Check(!NpcGripMath::MapSourceMountPose(grip.data(), held.cache.target_cs.data(), HandR, HandR, PalmR, PalmR, mount, identity.rotation, identity, refused, 0.0), "zero weapon scale is refused");

    double left[3]; for (size_t i = 0; i < 3; ++i) left[i] = grip[PalmL].translation[i] * 1.6;
    std::printf("  scaled contact: left palm error %.2e cm (unscaled offset would pull it %.2f cm)\n", Distance(held.cache.target_cs[PalmL].translation, left), Distance(wrong.cache.target_cs[PalmL].translation, left));
    Check(held.cache.contacts == 1 && Distance(held.cache.target_cs[PalmL].translation, left) < 1e-6, "scaled contact keeps the left palm on the scaled weapon");
    Check(wrong.cache.contacts == 1 && Distance(wrong.cache.target_cs[PalmL].translation, left) > 1, "unscaled palm offset would pull the left hand off it");
}
// Hun's ik_weapon_r hangs off the root but rests on weapon_r, and the animation keeps it there. A
// weapon mounted on it must follow the NPC's hand: HandProxies finds the hand-chain bone it rests
// on, and the adapter anchors to that hand instead of the root.
void TestHandProxy() {
    auto hero = Skeleton(100, 30, 28, 8);
    const auto npc = Skeleton(90, 25, 22, 6);
    std::vector<Transform> rest(hero.size());
    for (size_t i = 0; i < hero.size(); ++i) if (hero[i].parent < 0) rest[i] = hero[i].ref; else NpcGripMath::Compose(hero[i].ref, rest[static_cast<size_t>(hero[i].parent)], rest[i]);
    const int32_t Ik = static_cast<int32_t>(hero.size()), Near = Ik + 1, Far = Ik + 2;
    Transform on_hand = rest[HandR], beside = rest[HandR], away = At(0, 0, 50);
    beside.translation[1] += 0.4;  // 0.4 cm off the hand
    hero.push_back({3000, Root, on_hand}); hero.push_back({3001, Root, beside}); hero.push_back({3002, Root, away});
    NpcPoseTransfer::Plan plan; std::string why;
    Check(NpcPoseTransfer::Build(plan, hero, npc, {}, why), "proxy plan builds");
    const auto proxy = NpcPoseTransfer::HandProxies(plan.source, plan.source_ref_cs, {HandL, HandR}, 0.5);
    std::printf("  hand proxies: ik %d near %d far %d hand %d palm %d\n", proxy[Ik], proxy[Near], proxy[Far], proxy[HandR], proxy[PalmR]);
    Check(proxy[Ik] == HandR && proxy[Near] == HandR && proxy[Far] == -1, "bones resting on a hand are proxies; others are not");
    Check(proxy[HandR] == -1 && proxy[PalmR] == -1 && proxy[HandL] == -1 && proxy[Root] == -1, "hand-chain bones and the root are not proxies");
    Check(NpcPoseTransfer::HandProxies(plan.source, plan.source_ref_cs, {HandL, HandR}, 0.3)[Near] == -1, "tolerance bounds the match");
    // A bare bone route (WEAPON_R) resolves by name, since the socket lookup returns no bone for it.
    Check(NpcPoseTransfer::BoneNamed(plan.source, 1000 + HandR) == HandR && NpcPoseTransfer::BoneNamed(plan.source, 3000) == Ik && NpcPoseTransfer::BoneNamed(plan.source, 999) == -1, "bone routes resolve by name");

    // Posed: the animation copies hand_r into the IK bone. Anchored to the hand, the mount lands
    // at the NPC hand (palm offset scaled); anchored to the root it would stay at the hero's place.
    NpcPoseTransfer::SizeBones size; size.head = Spine;
    for (size_t s = 0; s < 2; ++s) { const int32_t u[2] = {UpperL, UpperR}, l[2] = {LowerL, LowerR}, h[2] = {HandL, HandR}, p[2] = {PalmL, PalmR};
        size.upper[s] = u[s]; size.lower[s] = l[s]; size.hand[s] = h[s]; size.middle[s] = size.index[s] = p[s]; size.pinky[s] = h[s]; }
    NpcPoseTransfer::BodyScale scale; Check(NpcPoseTransfer::WeaponScale(plan, size, size, 0.5, 5, scale), "proxy plan measures");
    std::vector<Transform> local(hero.size());
    for (size_t i = 0; i < hero.size(); ++i) local[i] = hero[i].ref;
    Yaw(local[UpperR], -60); Yaw(local[LowerR], -70);
    auto pose = ComposeAll(hero, local); pose[static_cast<size_t>(Ik)] = pose[HandR];
    Run run = Evaluate(plan, pose);
    Check(run.ok, "proxy plan evaluates");
    if (!run.ok) return;
    Transform identity, by_hand, by_root;
    Check(NpcGripMath::MapSourceMountPose(pose.data(), run.cache.target_cs.data(), HandR, HandR, PalmR, PalmR, pose[static_cast<size_t>(Ik)], identity.rotation, identity, by_hand, plan.weapon_scale) &&
          NpcGripMath::MapSourceMountPose(pose.data(), run.cache.target_cs.data(), Root, Root, -1, -1, pose[static_cast<size_t>(Ik)], identity.rotation, identity, by_root, plan.weapon_scale), "proxy mount maps both ways");
    const double hand_cm = Distance(by_hand.translation, run.cache.target_cs[HandR].translation), root_cm = Distance(by_root.translation, run.cache.target_cs[HandR].translation);
    std::printf("  ik mount: hand-anchored %.2f cm from the NPC hand, root-anchored %.2f cm (scale %.3f)\n", hand_cm, root_cm, plan.weapon_scale);
    Check(hand_cm < 1 && root_cm > 10, "an IK-proxy mount follows the NPC hand, not the root");
}
// The NPC's own ik_weapon_r (under the root, resting on its palm) is mapped from the hero's root-relative
// IK bone, so it drifts off the NPC hand by the body difference. Its post-process weapon IK would then pull
// the visible hand off the weapon. As a follower it lands on the palm every frame, keeping its rest offset
// (rotation included), and a bone under it moves with it.
void TestIkFollowers() {
    auto hero = Skeleton(100, 30, 28, 8), npc = Skeleton(90, 25, 22, 6);
    const auto rest_of = [](const std::vector<Bone>& bones) { std::vector<Transform> cs(bones.size());
        for (size_t i = 0; i < bones.size(); ++i) if (bones[i].parent < 0) cs[i] = bones[i].ref; else NpcGripMath::Compose(bones[i].ref, cs[static_cast<size_t>(bones[i].parent)], cs[i]); return cs; };
    const auto hero_rest = rest_of(hero), npc_rest = rest_of(npc);
    const int32_t Ik = static_cast<int32_t>(hero.size()), Child = Ik + 1;
    Transform hero_ik = hero_rest[PalmR], npc_ik = npc_rest[PalmR]; Axis(hero_ik.rotation, 0, 0, 1, 20); Axis(npc_ik.rotation, 0, 0, 1, 20);
    hero.push_back({3000, Root, hero_ik}); hero.push_back({3001, Ik, At(0, 0, 5)});
    npc.push_back({3000, Root, npc_ik}); npc.push_back({3001, Ik, At(0, 0, 5)});
    NpcPoseTransfer::Plan plain, followed; std::string why;
    Check(NpcPoseTransfer::Build(plain, hero, npc, {}, why) && NpcPoseTransfer::Build(followed, hero, npc, {}, why), "follower plans build");
    std::vector<int32_t> follow = NpcPoseTransfer::HandProxies(followed.target, followed.target_ref_cs, {HandL, HandR}, 0.5);
    Check(follow[static_cast<size_t>(Ik)] == PalmR && follow[static_cast<size_t>(Child)] == -1, "the NPC ik bone rests on its palm");
    std::vector<int32_t> bad(follow.size(), -1); bad[PalmR] = Ik; bad[static_cast<size_t>(Ik)] = PalmR;  // two bones following each other
    NpcPoseTransfer::Plan refused = plain;
    Check(!NpcPoseTransfer::SetFollowers(refused, bad), "a follower of a follower is refused");
    Check(NpcPoseTransfer::SetFollowers(followed, follow), "followers accepted");
    std::vector<Transform> local(hero.size());
    for (size_t i = 0; i < hero.size(); ++i) local[i] = hero[i].ref;
    Yaw(local[UpperR], -60); Yaw(local[LowerR], -70);
    auto pose = ComposeAll(hero, local);
    // The hero animation keeps its IK copy on the palm, as authored.
    pose[static_cast<size_t>(Ik)] = pose[PalmR]; std::memcpy(pose[static_cast<size_t>(Ik)].rotation, hero_ik.rotation, sizeof(hero_ik.rotation));
    { Transform hero_child; NpcGripMath::Compose(hero[static_cast<size_t>(Child)].ref, pose[static_cast<size_t>(Ik)], hero_child); pose[static_cast<size_t>(Child)] = hero_child; }
    Run before = Evaluate(plain, pose), after = Evaluate(followed, pose);
    Check(before.ok && after.ok, "follower plans evaluate");
    if (!before.ok || !after.ok) return;
    const auto& a = after.cache.target_cs;
    Transform want; NpcGripMath::ComposeMount(followed.follow_rel[static_cast<size_t>(Ik)], a[PalmR], want);
    Transform child_want; NpcGripMath::ComposeMount(npc[static_cast<size_t>(Child)].ref, a[static_cast<size_t>(Ik)], child_want);
    Transform local_check; NpcGripMath::ComposeMount(after.cache.target_local[static_cast<size_t>(Ik)], a[Root], local_check);
    const double drift = Distance(before.cache.target_cs[static_cast<size_t>(Ik)].translation, before.cache.target_cs[PalmR].translation);
    std::printf("  ik follower: unfollowed ik bone %.2f cm off the NPC palm, followed %.2e cm\n", drift, Distance(a[static_cast<size_t>(Ik)].translation, a[PalmR].translation));
    Check(drift > 5, "without a follower the NPC ik bone drifts off its palm");
    Check(SameTransform(a[static_cast<size_t>(Ik)], want) && Distance(a[static_cast<size_t>(Ik)].translation, a[PalmR].translation) < 1e-9, "followed ik bone sits on the palm with its rest offset");
    Check(SameTransform(a[static_cast<size_t>(Child)], child_want) && SameTransform(local_check, a[static_cast<size_t>(Ik)]), "its child moves with it; target_local agrees");
    bool fk = true;
    for (size_t t = 0; t < Count; ++t) fk = fk && SameTransform(a[t], before.cache.target_cs[t]);
    Check(fk, "FK bones are unchanged");
    Check(SameTransform(after.cache.synthetic_source[static_cast<size_t>(Ik)], a[static_cast<size_t>(Ik)]), "synthetic source carries the followed ik bone");
}
// A boss skill montage gives the hero's bones that boss's offsets: under a foreign montage the target
// keeps its own rest translations except root / pelvis, blended by the weight, and the cache rebuilds.
void TestForeignMontage() {
    const auto hero = Skeleton(100, 30, 28, 8), npc = Skeleton(90, 25, 22, 6);
    NpcPoseTransfer::Plan plan; std::string why;
    Check(NpcPoseTransfer::Build(plan, hero, npc, {}, why), "foreign plan builds");
    // Foreign offsets: every bone but the root 5 cm further along X in its parent.
    std::vector<Transform> local(Count), pose(Count);
    for (size_t i = 0; i < Count; ++i) { local[i] = hero[i].ref; if (i) local[i].translation[0] += 5; }
    for (size_t i = 0; i < Count; ++i)
        if (kParents[i] < 0) pose[i] = local[i]; else NpcGripMath::Compose(local[i], pose[static_cast<size_t>(kParents[i])], pose[i]);
    plan.foreign_keep.assign(plan.target.size(), 0); plan.foreign_keep[Spine] = 1;
    NpcPoseTransfer::Cache cache; NpcPoseTransfer::Prepare(plan, cache); const char* reason = "";
    Check(NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), cache, reason, 0.0), "own montage evaluates");
    const Transform moved = cache.target_local[UpperL];
    Check(std::abs(moved.translation[0] - (npc[UpperL].ref.translation[0] + 5)) < 1e-9, "own montage: translation motion is copied");
    Check(NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), cache, reason, 1.0), "foreign montage evaluates");
    bool rest = true; for (size_t t = UpperL; t < Count; ++t) rest = rest && Distance(cache.target_local[t].translation, npc[t].ref.translation) < 1e-9;
    Check(rest, "foreign montage: the target keeps its own rest translations");
    Check(std::abs(cache.target_local[Spine].translation[0] - (npc[Spine].ref.translation[0] + 5)) < 1e-9, "foreign montage: kept bones still move");
    const uint64_t builds = cache.builds;
    NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), cache, reason, 0.5);
    Check(cache.builds == builds + 1 && std::abs(cache.target_local[UpperL].translation[0] - (npc[UpperL].ref.translation[0] + 2.5)) < 1e-9, "weight change rebuilds and blends halfway");
    Check(NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), cache, reason, 7.0) && cache.foreign == 1, "weight clamped to 1");
    // The NPC's own skill: the hero carries the NPC's track remapped - rescaled to the hero's lengths for
    // scaled bones, as it is for Animation bones, the hero's rest for Skeleton bones. The target takes
    // its own track back (the boss weapon bone moves as in the boss's performance).
    using NpcPoseTransfer::kAnimation; using NpcPoseTransfer::kScaled; using NpcPoseTransfer::kSkeleton;
    plan.source_mode.assign(plan.target.size(), kScaled);
    plan.source_mode[HandR] = plan.source_mode[PalmR] = kAnimation; plan.source_mode[LowerL] = kSkeleton;
    const auto length = [](const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    std::vector<Transform> track(Count), carried(Count), own(Count);
    for (size_t i = 0; i < Count; ++i) {
        track[i] = npc[i].ref; if (i) track[i].translation[1] += 3;
        carried[i] = track[i];
        const double h = length(hero[i].ref.translation), n = length(npc[i].ref.translation);
        if (plan.source_mode[i] == kSkeleton) carried[i] = hero[i].ref;
        else if (plan.source_mode[i] == kScaled && n > 1e-6) for (double& v : carried[i].translation) v *= h / n;
    }
    for (size_t i = 0; i < Count; ++i)
        if (kParents[i] < 0) own[i] = carried[i]; else NpcGripMath::Compose(carried[i], own[static_cast<size_t>(kParents[i])], own[i]);
    Check(NpcPoseTransfer::Evaluate(plan, own.data(), own.size(), cache, reason, 1.0, true) && cache.own, "own montage evaluates");
    bool exact = true;
    for (size_t t = 1; t < Count; ++t) exact = exact && Distance(cache.target_local[t].translation, (t == LowerL ? npc[t].ref : track[t]).translation) < 1e-9;
    Check(exact, "own montage: the target takes its own track back, Skeleton bones rest");
    const Transform own_hand = cache.target_local[HandR];
    Check(NpcPoseTransfer::Evaluate(plan, own.data(), own.size(), cache, reason, 1.0, false) && !cache.own && Distance(cache.target_local[HandR].translation, npc[HandR].ref.translation) < 1e-9,
        "other skeleton: the same pose holds the rest translation");
    Check(NpcPoseTransfer::Evaluate(plan, own.data(), own.size(), cache, reason, 0.0, true) && !cache.own, "own needs a foreign weight");
    NpcPoseTransfer::Evaluate(plan, own.data(), own.size(), cache, reason, 0.5, true);
    const double plain = npc[HandR].ref.translation[1] + (carried[HandR].translation[1] - hero[HandR].ref.translation[1]);
    Check(std::abs(cache.target_local[HandR].translation[1] - (plain + own_hand.translation[1]) / 2) < 1e-9, "own montage blends with the plain transfer");
}
// The boss's own skill on a display mesh built unlike its animation Skeleton (MengHua: a 163 cm pelvis on
// a 102 cm Skeleton, every bone Animation). The engine scales the key to the hero by the Skeleton's length;
// the target shows it as the boss does - Animation the key, Scaled key * display / Skeleton, Skeleton the
// display rest - and a key the hero lost is the Skeleton's length along the display rest. Without the boss
// modes (or for an untracked bone: kScaled at the display length) the candidate 55 result stands.
void TestOwnMontageBuiltOffSkeleton() {
    using NpcPoseTransfer::kAnimation; using NpcPoseTransfer::kScaled; using NpcPoseTransfer::kSkeleton;
    const auto hero = Skeleton(100, 30, 28, 8), npc = Skeleton(160, 48, 44, 12), anim = Skeleton(100, 28, 26, 7);
    const auto length = [](const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    NpcPoseTransfer::Plan plan; std::string why;
    Check(NpcPoseTransfer::Build(plan, hero, npc, {}, why), "built-off plan builds");
    plan.source_mode.assign(plan.target.size(), kScaled);
    plan.source_mode[HandR] = plan.source_mode[PalmR] = kAnimation; plan.source_mode[LowerL] = kSkeleton;
    plan.own_mode.assign(plan.target.size(), kAnimation); plan.own_mode[UpperL] = plan.own_mode[PalmR] = kScaled; plan.own_mode[LowerR] = kSkeleton;
    plan.own_length.resize(plan.target.size());
    for (size_t i = 0; i < Count; ++i) plan.own_length[i] = length(anim[i].ref.translation);
    // The boss key: its Skeleton's rest, 3 cm further along Y; the engine carries it on the hero.
    std::vector<Transform> key(Count), carried(Count), pose(Count);
    for (size_t i = 0; i < Count; ++i) {
        key[i] = anim[i].ref; if (i) key[i].translation[1] += 3;
        carried[i] = key[i];
        const double h = length(hero[i].ref.translation), s = plan.own_length[i];
        if (plan.source_mode[i] == kSkeleton) carried[i] = hero[i].ref;
        else if (plan.source_mode[i] == kScaled && s > 1e-4) for (double& v : carried[i].translation) v *= h / s;
    }
    for (size_t i = 0; i < Count; ++i)
        if (kParents[i] < 0) pose[i] = carried[i]; else NpcGripMath::Compose(carried[i], pose[static_cast<size_t>(kParents[i])], pose[i]);
    NpcPoseTransfer::Cache cache; NpcPoseTransfer::Prepare(plan, cache); const char* reason = "";
    Check(NpcPoseTransfer::Evaluate(plan, pose.data(), pose.size(), cache, reason, 1.0, true) && cache.own, "built-off own montage evaluates");
    bool animation = true;
    for (size_t t : {size_t(Spine), size_t(UpperR), size_t(HandL), size_t(PalmL), size_t(HandR)}) animation = animation && Distance(cache.target_local[t].translation, key[t].translation) < 1e-9;
    Check(animation, "Animation bones take the boss key at its Skeleton length, not the display's");
    bool scaled = true;
    for (size_t t : {size_t(UpperL), size_t(PalmR)}) {
        double want[3]; const double k = length(npc[t].ref.translation) / plan.own_length[t];
        for (size_t i = 0; i < 3; ++i) want[i] = key[t].translation[i] * k;
        scaled = scaled && Distance(cache.target_local[t].translation, want) < 1e-9;
    }
    Check(scaled, "Scaled bones take key * display / Skeleton length, whatever the hero's mode");
    Check(Distance(cache.target_local[LowerR].translation, npc[LowerR].ref.translation) < 1e-9, "boss Skeleton-mode bones rest");
    double lost[3]; const double k = plan.own_length[LowerL] / length(npc[LowerL].ref.translation);
    for (size_t i = 0; i < 3; ++i) lost[i] = npc[LowerL].ref.translation[i] * k;
    Check(Distance(cache.target_local[LowerL].translation, lost) < 1e-9, "a key the hero lost is the Skeleton length along the display rest");
    // Candidate 55 (every boss bone Scaled at the display length) stretched the spine to the display build.
    const Transform fixed_spine = cache.target_local[Spine];
    NpcPoseTransfer::Plan old = plan; old.own_mode.clear(); old.own_length.clear();
    NpcPoseTransfer::Cache old_cache; NpcPoseTransfer::Prepare(old, old_cache);
    Check(NpcPoseTransfer::Evaluate(old, pose.data(), pose.size(), old_cache, reason, 1.0, true), "built-off plan without boss modes evaluates");
    Check(length(old_cache.target_local[Spine].translation) > 1.5 * length(fixed_spine.translation), "without the boss modes the spine stretches to the display length");
    // Untracked bones (kScaled at the display length) and a plan without boss modes agree exactly.
    NpcPoseTransfer::Plan untracked = plan;
    for (size_t t = 0; t < Count; ++t) { untracked.own_mode[t] = kScaled; untracked.own_length[t] = length(npc[t].ref.translation); }
    NpcPoseTransfer::Cache untracked_cache; NpcPoseTransfer::Prepare(untracked, untracked_cache);
    Check(NpcPoseTransfer::Evaluate(untracked, pose.data(), pose.size(), untracked_cache, reason, 1.0, true), "untracked plan evaluates");
    bool same = true;
    for (size_t t = 0; t < Count; ++t) same = same && Distance(untracked_cache.target_local[t].translation, old_cache.target_local[t].translation) < 1e-9;
    Check(same, "untracked bones keep the candidate 55 translation");
    // A display built like its Skeleton: the boss modes change nothing, whatever they are.
    NpcPoseTransfer::Plan alike; Check(NpcPoseTransfer::Build(alike, hero, anim, {}, why), "built-alike plan builds");
    alike.source_mode = plan.source_mode; alike.own_length.resize(alike.target.size());
    for (size_t t = 0; t < Count; ++t) alike.own_length[t] = length(anim[t].ref.translation);
    alike.own_mode.assign(alike.target.size(), kAnimation); alike.own_mode[UpperL] = kScaled;
    NpcPoseTransfer::Plan alike_old = alike; alike_old.own_mode.clear(); alike_old.own_length.clear();
    NpcPoseTransfer::Cache a, b; NpcPoseTransfer::Prepare(alike, a); NpcPoseTransfer::Prepare(alike_old, b);
    Check(NpcPoseTransfer::Evaluate(alike, pose.data(), pose.size(), a, reason, 1.0, true) && NpcPoseTransfer::Evaluate(alike_old, pose.data(), pose.size(), b, reason, 1.0, true), "built-alike plans evaluate");
    bool unchanged = true;
    for (size_t t = 0; t < Count; ++t) unchanged = unchanged && Distance(a.target_local[t].translation, b.target_local[t].translation) < 1e-9;
    Check(unchanged, "a display built like its Skeleton keeps the candidate 55 translations");
}
// The boss's own skill keeps the boss's own arms: its key already holds the boss weapon, so the retarget
// pose (rest turns, matched to the hero's arms) and the two-hand contact fade out by the own-montage weight
// and the arms come out exactly as without them; other montages keep both.
void TestOwnMontageNativeArms() {
    const auto hero = Skeleton(100, 30, 28, 8);
    auto npc = Skeleton(90, 25, 22, 6);
    Axis(npc[UpperL].ref.rotation, 0, 1, 0, -30); Axis(npc[LowerL].ref.rotation, 0, 0, 1, 15);
    NpcPoseTransfer::Plan plain, matched; std::string why;
    Check(NpcPoseTransfer::Build(plain, hero, npc, {}, why) && NpcPoseTransfer::Build(matched, hero, npc, {}, why), "native-arm plans build");
    Check(NpcPoseTransfer::AlignRestDirection(matched, UpperL, LowerL) && NpcPoseTransfer::AlignRestDirection(matched, LowerL, HandL) && NpcPoseTransfer::FinishRestTurns(matched) &&
          NpcPoseTransfer::SetHandContact(matched, Request()), "native-arm plan takes rest turns and the contact");
    const auto grip = HeroPose(hero, 60, 70);
    NpcPoseTransfer::Cache p, m; NpcPoseTransfer::Prepare(plain, p); NpcPoseTransfer::Prepare(matched, m); const char* reason = "";
    Check(NpcPoseTransfer::Evaluate(plain, grip.data(), grip.size(), p, reason, 1.0, true) && NpcPoseTransfer::Evaluate(matched, grip.data(), grip.size(), m, reason, 1.0, true), "own montage evaluates on both plans");
    bool same = true;
    for (size_t t = 0; t < Count; ++t) same = same && SameTransform(p.target_cs[t], m.target_cs[t]);
    Check(same && m.contacts == 0 && m.contact_weight == 0, "own montage: arms as the plain mapping, no two-hand contact");
    Check(NpcPoseTransfer::Evaluate(matched, grip.data(), grip.size(), m, reason, 1.0, false) && m.contacts == 1 && !SameTransform(m.target_cs[UpperL], p.target_cs[UpperL]),
          "foreign montage keeps the rest turns and the contact");
    const double full = m.contact_weight;
    Check(NpcPoseTransfer::Evaluate(matched, grip.data(), grip.size(), m, reason, 0.5, true) && std::abs(m.contact_weight - full * 0.5) < 1e-12, "half own weight halves the contact");
    std::printf("  own montage arms: plain match %d, contact %.2f -> %.2f at half weight\n", same ? 1 : 0, full, m.contact_weight);
}
// Mounted boss (长鸿 phase 2): the display mesh carries a horse the hero has no bones for. A pose puppet
// plays the boss's own animation; the horse and the rider's pelvis take its root-relative local pose, the
// rest follows the hero (all of it during the boss's own montage). The CopyPose map can only point at
// source slots, so each horse bone borrows a slot no target bone maps: its local is the slot relative to
// that slot's source parent. Arms matched to the hero's rest keep that turn relative to the turned body.
void TestMountPuppet() {
    auto hero = Skeleton(100, 30, 28, 8);
    hero.push_back({3000, Spine, At(0, 5, 10)}); hero.push_back({3001, Count, At(0, 0, 5)}); hero.push_back({3002, Root, At(10, 0, 0)});
    auto npc = Skeleton(90, 25, 22, 6);
    const size_t H0 = npc.size(), H1 = H0 + 1;
    npc.push_back({4000, Root, At(0, 0, -50)}); npc.push_back({4001, static_cast<int32_t>(H0), At(30, 0, 0)});
    Axis(npc[UpperL].ref.rotation, 0, 1, 0, -30);
    NpcPoseTransfer::Plan plan, turned; std::string why;
    Check(NpcPoseTransfer::Build(plan, hero, npc, {}, why) && NpcPoseTransfer::Build(turned, hero, npc, {}, why), "mount plans build");
    Check(NpcPoseTransfer::AlignRestDirection(turned, UpperL, LowerL) && NpcPoseTransfer::FinishRestTurns(turned), "mount plan arm matched to the hero");
    std::vector<uint8_t> always(npc.size(), 0); always[Spine] = always[H0] = always[H1] = 1;
    size_t assigned = 0, wanted = 0;
    Check(NpcPoseTransfer::SetPuppet(plan, always, assigned, wanted) && NpcPoseTransfer::SetPuppet(turned, always, assigned, wanted), "puppet set");
    Check(assigned == 2 && wanted == 2 && plan.extra_source[H0] >= Count && plan.extra_source[H1] >= Count && plan.extra_source[H0] != plan.extra_source[H1] &&
          plan.extra_source[Spine] == -1 && plan.extra_order.size() == 2, "horse bones borrow unmapped source slots");
    // Puppet: the boss's pose on its own mesh, its root carried 100 cm forward by root motion.
    std::vector<Transform> pl(npc.size());
    for (size_t i = 0; i < npc.size(); ++i) pl[i] = npc[i].ref;
    pl[Root].translation[0] = 100; Yaw(pl[Spine], 30); pl[Spine].translation[2] = 120; Axis(pl[H0].rotation, 0, 1, 0, 10); pl[H0].translation[2] = -45; Yaw(pl[H1], 20);
    Yaw(pl[UpperL], 40);
    const auto puppet_cs = ComposeAll(npc, pl);
    NpcPoseTransfer::PuppetPose puppet; puppet.cs = puppet_cs.data(); puppet.count = puppet_cs.size(); puppet.serial = 1; puppet.now_ms = 1000;
    std::vector<Transform> hero_pose(hero.size());
    { std::vector<Transform> local(hero.size()); for (size_t i = 0; i < hero.size(); ++i) local[i] = hero[i].ref; Yaw(local[Spine], -15); Yaw(local[UpperL], 25); Yaw(local[LowerL], 35); hero_pose = ComposeAll(hero, local); }
    NpcPoseTransfer::Cache cache, bare; NpcPoseTransfer::Prepare(plan, cache); NpcPoseTransfer::Prepare(plan, bare); const char* reason = "";
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 0, false, &puppet), "puppet evaluates");
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), bare, reason), "puppet plan without a puppet evaluates");
    bool held = true;
    for (size_t t : {size_t(Spine), H0, H1}) held = held && SameTransform(cache.target_local[t], pl[t]);
    Check(held, "horse and pelvis take the puppet's root-relative locals");
    bool hero_arms = true;
    for (size_t t = UpperL; t < Count; ++t) hero_arms = hero_arms && SameTransform(cache.target_local[t], bare.target_local[t]);
    Check(hero_arms && SameTransform(cache.target_local[Root], bare.target_local[Root]), "root and arms follow the hero");
    const auto slot_local = [&](const NpcPoseTransfer::Cache& c, size_t t) {
        const size_t s = static_cast<size_t>(plan.extra_source[t]); Transform local;
        NpcGripMath::RelativeNativeSafe(c.synthetic_source[s], c.synthetic_source[static_cast<size_t>(hero[s].parent)], local); return local; };
    Check(SameTransform(slot_local(cache, H0), pl[H0]) && SameTransform(slot_local(cache, H1), pl[H1]), "the CopyPose reads each horse bone's local off its borrowed slot");
    Check(SameTransform(slot_local(bare, H0), npc[H0].ref) && SameTransform(bare.synthetic_source[Count + 2], hero_pose[Count + 2]), "without a puppet the borrowed slots carry the rest; free slots pass through");
    const uint64_t builds = cache.builds;
    NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 0, false, &puppet);
    Check(cache.hits == 1 && cache.builds == builds, "same hero and puppet poses are a cache hit");
    // The boss's own montage: everything but the root from the puppet.
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 1.0, true, &puppet), "own montage with a puppet evaluates");
    bool all = true;
    for (size_t t = 1; t < npc.size(); ++t) all = all && SameTransform(cache.target_local[t], pl[t]);
    Check(all, "own montage: every bone but the root is the puppet's");
    // A new puppet animation fades in over kPuppetBlendMs from the last puppet pose.
    std::vector<Transform> next = pl; Yaw(next[H1], 60);
    const auto next_cs = ComposeAll(npc, next);
    NpcPoseTransfer::PuppetPose switched = puppet; switched.cs = next_cs.data(); switched.serial = 2; switched.now_ms = 2000;
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 0, false, &switched) && SameTransform(cache.target_local[H1], pl[H1]), "a switch starts from the last puppet pose");
    switched.now_ms = 2000 + NpcPoseTransfer::kPuppetBlendMs / 2;
    Transform half; NpcPoseTransfer::detail::Blend(pl[H1], next[H1], 0.5, half);
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 0, false, &switched) && SameTransform(cache.target_local[H1], half), "halfway through the fade");
    switched.now_ms = 2000 + NpcPoseTransfer::kPuppetBlendMs;
    Check(NpcPoseTransfer::Evaluate(plan, hero_pose.data(), hero_pose.size(), cache, reason, 0, false, &switched) && SameTransform(cache.target_local[H1], next[H1]), "fade done");
    // An arm matched to the hero's rest keeps its turn relative to the body the puppet turned.
    NpcPoseTransfer::Cache t_puppet, t_bare; NpcPoseTransfer::Prepare(turned, t_puppet); NpcPoseTransfer::Prepare(turned, t_bare);
    Check(NpcPoseTransfer::Evaluate(turned, hero_pose.data(), hero_pose.size(), t_puppet, reason, 0, false, &puppet) && NpcPoseTransfer::Evaluate(turned, hero_pose.data(), hero_pose.size(), t_bare, reason), "turned-arm mount plan evaluates");
    Transform with, without; NpcGripMath::RelativeNativeSafe(t_puppet.target_cs[UpperL], t_puppet.target_cs[Spine], with); NpcGripMath::RelativeNativeSafe(t_bare.target_cs[UpperL], t_bare.target_cs[Spine], without);
    Check(SameRotation(with, without) && !SameRotation(t_puppet.target_cs[UpperL], t_bare.target_cs[UpperL]), "matched arm rides the puppet's pelvis turn");
    std::printf("  mount puppet: borrowed slots %zu of %zu, builds %llu hits %llu\n", assigned, wanted, static_cast<unsigned long long>(cache.builds), static_cast<unsigned long long>(cache.hits));
}
// The skin's movement capsule: the combat blueprint's capsule when in range, else stature-scaled; the
// hero keeps only its radius. Other body shapes scale about the feet; skills cycle in random rounds.
void TestBodyCollision() {
    using namespace NpcBodyCollision;
    const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    const CapsulePlan big = PlanCapsule(34, 90, 60, 180, 1.3, 0.5, 4.0);
    Check(big.valid && near(big.radius, 60) && near(big.half, 180) && near(big.ratio, 2) && std::string(big.source) == "blueprint", "blueprint capsule taken as is");
    Check(near(KeptRadius(big, 90), 60), "the hero keeps the skin's radius at its own half-height");
    const CapsulePlan brute = PlanCapsule(45, 96, 110, 230, 2.4, 0.5, 4.0);
    Check(brute.valid && near(KeptRadius(brute, 96), 96), "a radius above the hero half-height is capped there (WanJun 110 -> 96)");
    Check(near(KeptRadius(CapsulePlan{}, 96), 0), "no plan: nothing kept");
    const CapsulePlan giant = PlanCapsule(34, 90, 200, 450, 1.3, 0.5, 4.0);
    Check(giant.valid && std::string(giant.source) == "stature" && near(giant.half, 117) && near(giant.radius, 34 * 1.3), "out-of-range blueprint falls back to stature");
    const CapsulePlan small = PlanCapsule(34, 90, 30, 70, 1, 0.5, 4.0);
    Check(small.valid && near(KeptRadius(small, 90), 30), "smaller skin narrows the capsule");
    Check(!PlanCapsule(34, 90, 0, 0, 0, 0.5, 4.0).valid, "no blueprint and no stature: unchanged");
    Check(!PlanCapsule(0, 90, 60, 180, 1, 0.5, 4.0).valid && !PlanCapsule(34, 20, 60, 180, 1, 0.5, 4.0).valid, "broken hero capsule: unchanged");
    const CapsulePlan clamped = PlanCapsule(34, 90, 50, 40, 10, 0.5, 4.0);
    Check(clamped.valid && std::string(clamped.source) == "stature" && near(clamped.ratio, 4), "half below radius is not a capsule; stature clamped to 4");
    const CapsulePlan wide = PlanCapsule(34, 90, 0, 0, 0.5, 0.5, 4.0);
    Check(wide.valid && wide.half >= wide.radius, "half-height never below radius");
    Shape shape; shape.location[2] = 40; shape.location[0] = 10;
    const Shape body = ScaleShape(shape, 1.5, false), socket = ScaleShape(shape, 1.5, true);
    Check(near(body.location[2], 60) && near(body.location[0], 15) && near(body.scale[1], 1.5), "body shape offset and size scale with the capsule");
    Check(near(socket.location[2], 40) && near(socket.scale[2], 1.5), "socket shape keeps its socket offset");
    // Under the capsule (centre 90 above the feet) a shape at the feet stays at the feet, one at the centre rises.
    const Shape feet = [] { Shape s; s.location[2] = -90; return s; }();
    Check(near(ScaleShape(feet, 2, false, -90).location[2], -90), "shape at the feet stays on the ground");
    Check(near(ScaleShape(Shape{}, 2, false, -90).location[2], 90), "shape at the hero centre moves to the taller body's centre");
    SkillRound round; round.Reset(12345);
    bool every_round_complete = true, no_repeat_across = true, order_varies = false;
    std::vector<size_t> first_order;
    for (int r = 0; r < 50; ++r) {
        std::vector<int> seen(12, 0); std::vector<size_t> order;
        for (size_t i = 0; i < 12; ++i) { const size_t pick = round.Next(12); if (pick < 12) ++seen[pick]; order.push_back(pick); }
        for (int count : seen) every_round_complete = every_round_complete && count == 1;
        if (r == 0) first_order = order; else order_varies = order_varies || order != first_order;
        Check(round.Left(12) == 0, "a finished round has nothing left");
        const size_t last = order.back(), next = round.Next(12);
        no_repeat_across = no_repeat_across && next != last;
        // Put the round back to its start so each loop checks one whole round.
        round.pos = 0; round.order.clear(); round.last = last;
        (void)next;
    }
    Check(every_round_complete, "every skill once per random round");
    Check(no_repeat_across, "a new round never starts with the skill just cast");
    Check(order_varies, "rounds are shuffled differently");
    SkillRound fresh; fresh.Reset(7); fresh.Next(5); fresh.Next(5);
    Check(fresh.Left(5) == 3 && fresh.Next(8) < 8 && fresh.Left(8) == 7, "a different skill count starts a new round");
    SkillRound single; single.Reset(1); Check(single.Next(1) == 0 && single.Next(1) == 0, "one skill repeats");
    SkillRound empty; Check(empty.Next(0) == 0 && empty.Left(0) == 0, "no skills: nothing picked");
    std::printf("  body collision: blueprint %.0f/%.0f kept radius %.0f, brute kept %.0f, stature fallback half %.1f\n", big.radius, big.half, KeptRadius(big, 90), KeptRadius(brute, 96), giant.half);
}
}  // namespace

// Weapon routes: only another actor's weapon on a named socket becomes one; effects (a pooled footstep burst on
// foot_r_Socket owned by WorldSettings, a weapon's own trail) never do; discovered routes go when the weapon goes.
void TestWeaponRoutes() {
    using NpcGripRoutes::Attached;
    constexpr uint64_t kFoot = 0x1111, kBack = 0x2222, kHand = 0x3333;
    Attached pooled; pooled.socket = kFoot; pooled.owner_alive = true; pooled.owner_is_world = true; pooled.effect = true;
    Attached pooled_mesh = pooled; pooled_mesh.effect = false;  // anything the pools own, even not an effect class
    Attached trail; trail.socket = kHand; trail.owner_alive = true; trail.effect = true;  // a weapon actor's Niagara
    Attached knife; knife.socket = kBack; knife.owner_alive = true; knife.weapon_actor = true; // a Shaqi weapon's mesh
    Attached own = knife; own.owner_is_pawn = true;                                       // the hero's own part
    Attached dead = knife; dead.owner_alive = false;
    Attached bare = knife; bare.socket = 0;
    Attached light = knife; light.weapon_actor = false;
    Check(!NpcGripRoutes::Weapon(light) && !NpcGripRoutes::TakesRoute(light, true, false), "cinematic lights and cameras never remap head sockets");
    Check(!NpcGripRoutes::Weapon(pooled) && !NpcGripRoutes::TakesRoute(pooled, true, false), "a pooled effect on foot_r_Socket (owner WorldSettings) is no route");
    Check(!NpcGripRoutes::TakesRoute(pooled_mesh, true, false), "nothing the effect pools own becomes a route");
    Check(!NpcGripRoutes::TakesRoute(trail, true, false), "a weapon actor's effect component is no route");
    Check(NpcGripRoutes::TakesRoute(knife, true, false), "another actor's weapon on a socket the mesh has becomes a route");
    Check(!NpcGripRoutes::TakesRoute(knife, true, true) && !NpcGripRoutes::TakesRoute(knife, false, false), "not twice, not on a socket the mesh lacks");
    Check(!NpcGripRoutes::TakesRoute(own, true, false) && !NpcGripRoutes::TakesRoute(dead, true, false) && !NpcGripRoutes::TakesRoute(bare, true, false),
          "the hero's own parts, a dead owner and an unnamed socket are no routes");
    const std::vector<NpcGripRoutes::Route> routes = {{kHand, false}, {kBack, true}, {kFoot, true}};
    const std::vector<size_t> with_knife = NpcGripRoutes::StaleRoutes(routes, {knife, pooled});
    Check(with_knife.size() == 1 && with_knife[0] == 2, "a discovered route with only an effect on its socket goes; the knife's stays; generated ones stay");
    const std::vector<size_t> nothing = NpcGripRoutes::StaleRoutes(routes, {});
    Check(nothing.size() == 2 && nothing[0] == 2 && nothing[1] == 1, "with nothing attached every discovered route goes, highest index first");
    Check(NpcGripRoutes::StaleRoutes(routes, {light}).size() == 2, "a light cannot keep an old weapon route alive");
}

int main() {
    TestContact();
    TestWeaponFrame();
    TestParentMismatch();
    TestRestTurn();
    TestGripGate();
    TestWeaponScale();
    TestHandProxy();
    TestIkFollowers();
    TestBodyCollision();
    TestForeignMontage();
    TestOwnMontageBuiltOffSkeleton();
    TestOwnMontageNativeArms();
    TestMountPuppet();
    TestWeaponRoutes();
    std::printf("npc_pose_transfer_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
