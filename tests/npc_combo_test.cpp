// 招式派生 model (npc_combo_model.h): move kinds from GA names, trigger nodes and the hero action tag families,
// where a press goes (Follow: straight wires, 按键分支 rows and their pauses, intervals), editing (InsertAfter /
// Remove / Link, branches and the 加速 / 武器属性 modifiers), the play rate, stick direction and the ini text, old
// texts included.
#include "npc_combo_model.h"
#include "grab_boss.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }
}

int main() {
    using namespace NpcCombo;
    // Kinds.
    Check(Classify("GA_GreatBladeMan_LightAttack_01_ForCa") == Kind::Light, "LightAttack ... ForCa is a light attack");
    Check(Classify("/PBZCombatAsset/ActorData/Mobs/062_MuXiaoKui/Ability/Abilities/GA_MuXiaoKui_Skill_01") == Kind::Skill, "a package path classifies by its name");
    Check(Classify("GA_ChiLian_HeavyAttack_02") == Kind::Heavy && Classify("GA_AHua_Heavy_01") == Kind::Heavy, "HeavyAttack and Heavy are heavy attacks");
    Check(Classify("GA_MuXiaoKui_Dodge_B") == Kind::Dodge && Classify("GA_ZuoShang_Dash_F") == Kind::Dodge && Classify("GA_ChangHong_BackStep") == Kind::Dodge,
          "Dodge / Dash / BackStep are dodges");
    Check(Classify("GA_AHua_SpecialMove_L") == Kind::Dodge, "SpecialMove is a dodge (AHua)");
    Check(Classify("GA_BiYuan_DodgeAttack_B_01") == Kind::DodgeAttack && Classify("GA_YuBi_Attack_SideStepAtk") == Kind::DodgeAttack,
          "dodges that attack are their own kind, wherever the word sits");
    Check(Classify("GA_HookMan_P2_SpecialAttack_03") == Kind::Special && Classify("GA_HookMan_CriticalArt_01_DirectJump") == Kind::Ultimate &&
          Classify("GA_ChangHong_P2_AutoCombo_JustRun") == Kind::Combo && Classify("GA_Foo_Bar_01") == Kind::Other, "special / ultimate / combo / other");
    Check(Classify("GA_ZuoShang_Attack_AutoCombo_Close") == Kind::Combo, "an auto combo is a combo even after Attack");
    Check(Classify("GA_Attack_Skill_01") == Kind::Skill, "the first token is the boss, never a kind");

    // Hero action families.
    Check(ActionBits("Ability.Action.Hun.Light.AAAA") == (1u << static_cast<int>(Trigger::Light)), "light attack combo hits are light attacks");
    Check(ActionBits("Ability.Action.Hun.Heavy.ComboAtk_N1H1") == (1u << static_cast<int>(Trigger::Heavy)), "heavy attack family");
    Check(ActionBits("Ability.Action.Hun.ShachiAbility.TigerCannon") == (1u << static_cast<int>(Trigger::Shachi)) && std::wstring(TriggerLabel(Trigger::Shachi)) == L"主角影之武装" &&
          std::wstring(ComboKeyLabel(static_cast<int>(Trigger::Shachi))) == L"影之武装" && static_cast<int>(Trigger::Shachi) == 3,
          "影之武装 = ShachiAbility (the old 杀气 label; saved as 3, so old files read it as 影之武装)");
    Check(ActionBits("Ability.Action.Stance") == (1u << static_cast<int>(Trigger::Stance)) && ActionBits("Ability.Action.Stance.Loop") == (1u << static_cast<int>(Trigger::Stance)) &&
          ActionBits("Ability.Action.StanceDash") == 0u && std::wstring(TriggerLabel(Trigger::Stance)) == L"主角格挡" && ValidRouteButton(static_cast<int>(Trigger::Stance)) &&
          static_cast<int>(Trigger::Stance) == 16,
          "格挡 = the stance family Ability.Action.Stance (saved as 16, after every older trigger); a branch row may take it");
    {
        Config stance; stance.chain.push_back(TriggerNode(Trigger::Stance, 0, 0)); EnsureKey(stance);
        const Config back = Decode(Encode(stance));
        Check(back.chain.size() == stance.chain.size() && back.chain[0].type == NodeType::Trigger && back.chain[0].trigger == static_cast<int>(Trigger::Stance), "a 格挡 trigger is saved and read back");
    }
    Check(kTriggerOrder[5] == Trigger::Stance && kTriggerOrder[2] == Trigger::Shachi, "the page lists 格挡 after 战技, 影之武装 after 重攻击");
    Check(ActionBits("Ability.Action.Hun.Special") == (1u << static_cast<int>(Trigger::Special)) && ActionBits("Ability.Action.Hun.SpecialDash") == (1u << static_cast<int>(Trigger::SpecialDash)),
          "Special and SpecialDash are different families");
    Check(ActionBits("Ability.Action.Hun.DashAttack") == 0u && ActionBits("Ability.Action.Hun.Weapon.Switch") == 0u, "siblings with the same prefix and other actions are none");
    Check(Fires(Trigger::AnyDodge, ActionBits("Ability.Action.Hun.PerfectDodge")) && !Fires(Trigger::Dash, ActionBits("Ability.Action.Hun.PerfectDodge")) &&
          !Fires(Trigger::AnyDodge, ActionBits("Ability.Action.Hun.Light")) && !Fires(Trigger::NpcKey, ~0u), "any dodge covers every dodge; the NPC key is never a hero action");

    // Follow: where a press goes. 轻轻轻 (a straight wire = the same button again), 轻轻重 and 轻轻 (pause 2 s) 轻 by a branch.
    const Press light{ActionBits("Ability.Action.Hun.Light.A"), false}, heavy{ActionBits("Ability.Action.Hun.Heavy"), false}, key{0, true};
    const int L = static_cast<int>(Trigger::Light), H = static_cast<int>(Trigger::Heavy), K = static_cast<int>(Trigger::NpcKey);
    Config tree;  // 0 light trigger -> 1 A -> 2 B -> 3 branch {light -> 4 C, heavy -> 5 D, light after 2 s -> 6 interval 300 -> 7 E}
    tree.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
    tree.chain.push_back({NodeType::Skill, "GA_A"});
    tree.chain.push_back({NodeType::Skill, "GA_B"});
    Node fork = BranchNode(0, 0); fork.routes = {{L, 0, 4}, {H, 0, 5}, {L, 2000, 6}};
    tree.chain.push_back(fork);
    tree.chain.push_back({NodeType::Skill, "GA_C"});
    tree.chain.push_back({NodeType::Skill, "GA_D"});
    tree.chain.push_back({NodeType::Interval, {}, 300});
    tree.chain.push_back({NodeType::Skill, "GA_E"});
    tree.chain[0].next = 1; tree.chain[1].next = 2; tree.chain[2].next = 3; tree.chain[6].next = 7;
    EnsureKey(tree);
    Check(TriggerFor(tree, light) == 0 && TriggerFor(tree, heavy) == -1 && TriggerFor(tree, key) == -1, "the light trigger starts on a light press only (the key node has no chain)");
    Hop hop = Follow(tree, 0, L, light, -1);
    Check(hop.move == 1 && hop.wait_ms == 0 && hop.button == L, "first press: A");
    Check(Follow(tree, 1, L, light, 300).move == 2, "轻轻: a straight wire takes the same button again");
    Check(Follow(tree, 1, L, heavy, 300).move == -1, "a straight wire does not take another button");
    hop = Follow(tree, 2, L, light, 400);
    Check(hop.move == 4 && hop.button == L, "轻轻轻: a quick light press after B takes the light row");
    hop = Follow(tree, 2, L, heavy, 400);
    Check(hop.move == 5 && hop.button == H, "轻轻重: a heavy press takes the heavy row; the chain is on heavy after it");
    hop = Follow(tree, 2, L, light, 2100);
    Check(hop.move == 7 && hop.wait_ms == 300, "轻轻 停 2 s 轻: the longest pause that has passed wins, with its row's interval");
    Check(Follow(tree, 2, L, light, 1999).move == 4 && Follow(tree, 2, L, light, -1).move == 4, "short of the pause, or no press before: the quick row");
    Check(Follow(tree, 2, L, key, 3000).move == -1 && Follow(tree, 4, L, light, 100).move == -1, "an unmatched press or the end of a chain leads nowhere");
    Check(MaxPause(tree, 2) == 2000 && MaxPause(tree, 1) == 0 && BranchAfter(tree, 2) == 3, "the branch after B keeps the chain up to 2 s");
    Check(Continues(tree, 2) && !Continues(tree, 4), "a move with nothing after it ends the chain");
    // 派生点: when the next move may cut a move (a press before is kept for then; a branch's pause counts from it).
    Check(CutMs(tree, 2) == 0 && CancelOf(tree, 2) == 0, "no 派生点 and no interval after it: a move is cut when it ends");
    tree.chain[2].ms = 400;
    Check(CancelOf(tree, 2) == 400 && CutMs(tree, 2) == 400, "a 派生点 of 0.4 s");
    tree.chain[1].ms = 99999;
    Check(CancelOf(tree, 1) == kCancelMax && kCancelMax == 10000 && CancelOf(tree, 3) == 0 && CancelOf(tree, 0) == 0, "a 派生点 is at most 10 s; only moves have one");
    tree.chain[1].ms = 0; tree.chain[2].ms = 0;
    Check(!TooEarly(tree, 2, L, light, -1) && !TooEarly(tree, 2, L, heavy, -1) && !TooEarly(tree, 2, L, key, -1),
          "a press with an at-once row is never too early, nor one no row takes");
    {
        Config late;  // 0 light trigger -> 1 A -> 2 branch {heavy after 1 s -> 3 B}
        late.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
        late.chain.push_back({NodeType::Skill, "GA_A"});
        Node delayed = BranchNode(0, 0); delayed.routes = {{H, 1000, 3}};
        late.chain.push_back(delayed);
        late.chain.push_back({NodeType::Skill, "GA_B"});
        late.chain[0].next = 1; late.chain[1].next = 2;
        EnsureKey(late);
        Check(TooEarly(late, 1, L, heavy, -1) && TooEarly(late, 1, L, heavy, 999) && !TooEarly(late, 1, L, heavy, 1000) && Follow(late, 1, L, heavy, 1000).move == 3,
              "a heavy press before A can be cut, or before its 1 s pause, is too early (the chain waits); from 1 s after the cut it plays B");
        Check(!TooEarly(late, 1, L, light, 500) && Follow(late, 1, L, light, kPauseMax).move == -1, "a press no row takes leads nowhere, it is not early");
    }
    {
        // 停顿从上一次按键算起; 预输入 keeps one press (the last one wins); a press too early for its pause row is kept.
        Config pt;  // 0 light trigger -> 1 A -> 2 branch {light -> 3 B, light after 1 s -> 4 C, heavy after 1 s -> 5 D}
        pt.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
        pt.chain.push_back({NodeType::Skill, "GA_A"});
        Node rows = BranchNode(0, 0); rows.routes = {{L, 0, 3}, {L, 1000, 4}, {H, 1000, 5}};
        pt.chain.push_back(rows);
        pt.chain.push_back({NodeType::Skill, "GA_B"});
        pt.chain.push_back({NodeType::Skill, "GA_C"});
        pt.chain.push_back({NodeType::Skill, "GA_D"});
        pt.chain[0].next = 1; pt.chain[1].next = 2;
        EnsureKey(pt);
        // The 11-06 14:31 case: A plays 1.5 s; a light press 1.1 s after the press that played A, while A still plays.
        Decision d = Decide(pt, 1, L, light, 1100);
        Check(d.hop.move == 4 && !d.later && d.hop.pause_ms == 1000, "a press during the move 1.1 s after the press before takes the 1 s row (kept for A's 派生点 or end)");
        d = Decide(pt, 1, L, light, 400);
        Check(d.hop.move == 3 && !d.later && d.hop.pause_ms == 0, "a press during the move 0.4 s after the press before takes the quick row");
        Check(Decide(pt, 1, L, light, 1000).hop.move == 4 && Decide(pt, 1, L, light, 999).hop.move == 3, "exactly at the pause: the pause row; 1 ms short of it: the quick row");
        d = Decide(pt, 1, L, heavy, 600);
        Check(d.hop.move == 5 && d.later && d.ready_in_ms == 400 && d.hop.pause_ms == 1000,
              "heavy has only a 1 s row: a heavy press 0.6 s after the press before is kept and plays D in 0.4 s");
        d = Decide(pt, 1, L, heavy, 1000);
        Check(d.hop.move == 5 && !d.later && d.ready_in_ms == 0, "a heavy press exactly at the pause plays D at once (or at A's 派生点 while A plays)");
        Check(Decide(pt, 1, L, key, 600).hop.move == -1 && !Decide(pt, 1, L, key, 600).later && Decide(pt, 3, L, light, 5000).hop.move == -1,
              "a press no row takes, or after the end of a chain, leads nowhere (not kept)");
        Check(NextPause(pt, 1, L, heavy, 600).pause_ms == 1000 && NextPause(pt, 1, L, heavy, 1000).hop.move == -1, "the next pause of a press is one it has not reached yet");
        // The press before: the light press at 10.0 s played A; mashing keeps the quick row, a pause takes the 1 s row.
        uint64_t before = 10000;
        d = Decide(pt, 1, L, light, 400); before = PressBefore(before, d, 10400);
        Check(d.hop.move == 3 && before == 10400, "a light press at 10.4 s keeps B and is the press before for the next one");
        d = Decide(pt, 1, L, light, 700); before = PressBefore(before, d, 11100);
        Check(d.hop.move == 3 && before == 11100, "mashing on (11.1 s, 0.7 s after the press before) replaces it with B again: the last press wins");
        d = Decide(pt, 1, L, light, 1200); before = PressBefore(before, d, 12300);
        Check(d.hop.move == 4 && before == 12300, "a light press 1.2 s after the one before (A still playing) replaces B with C");
        const uint64_t heavy_before = before;
        d = Decide(pt, 1, L, heavy, 300); before = PressBefore(before, d, 12600);
        Check(d.hop.move == 5 && d.later && d.ready_in_ms == 700 && before == heavy_before, "an early heavy press replaces C with D for 13.3 s and leaves the press before");
        // The window after the move: A pressed at 10.0 s, ended at 11.5 s, reset 0.4 s.
        const uint64_t press = 10000, ended = 11500;
        const uint64_t until = WaitUntil(ended, press, 400, MaxPause(pt, 1));
        Check(MaxPause(pt, 1) == 1000 && until == press + 1000 + kPauseGraceMs, "with a 1 s row the chain waits until 1 s + 1.5 s after the press before (12.5 s)");
        Check(WaitUntil(ended, press, 400, 0) == ended + 400, "without a pause row: the reset time after the move");
        Check(WaitUntil(10500, press, 2000, 1000) == press + 1000 + 2000 && WaitUntil(ended, 8000, 400, 1000) == ended + 400,
              "a reset time over 1.5 s is the grace; never shorter than the reset time after the move");
        Check(12400 <= until && 12600 > until && Decide(pt, 1, L, light, 2400).hop.move == 4,
              "a light press at 12.4 s is inside the window (C); one at 12.6 s is beyond it: the chain has reset (it starts at A again)");
    }
    tree.chain[3].routes[2].next = -1;
    Check(Follow(tree, 2, L, light, 2100).move == 4 && MaxPause(tree, 2) == 0, "an unwired row never matches and keeps nothing waiting");
    // Lead: intervals between the trigger and the first move are a wait after the press; a bad name passes on.
    Config row = Linked({{NodeType::Interval, {}, 200}, {NodeType::Skill, ""}, {NodeType::Skill, "GA_A"}, {NodeType::Interval, {}, 300}, {NodeType::Interval, {}, 250},
                         {NodeType::AutoDodge}, {NodeType::Interval, {}, 20}});
    const int key_node = KeyTrigger(row);
    hop = Follow(row, key_node, K, key, -1);
    Check(key_node == 7 && hop.move == 2 && hop.wait_ms == 200 && TriggerFor(row, key) == key_node, "the key: lead interval, past a bad name, to A");
    hop = Follow(row, 2, K, key, 100);
    Check(hop.move == 5 && hop.wait_ms == 550 && SharedWait(row, 2) == 550, "consecutive intervals add up; the stick's dodge is a move");
    Check(SharedWait(row, 5) == kIntervalMin, "intervals are clamped to the minimum");
    row.chain[2].ms = 300;
    Check(CutMs(row, 2) == 300, "a 派生点 before the end of the intervals after a move cuts it first");
    row.chain[2].ms = 900;
    Check(CutMs(row, 2) == 550 && CutMs(row, 5) == kIntervalMin, "intervals that end first cut first");
    row.chain[2].ms = 0;
    Check(Follow(row, 2, K, light, 100).move == -1 && Follow(Config{}, -1, 0, key, -1).move == -1, "the key's chain takes no hero press; nothing follows nothing");

    // Several triggers, each with its own chain; only what the wires reach plays; a loop ends at the first repeat.
    Config graph = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}, {NodeType::Skill, "GA_C"}, {NodeType::Skill, "GA_Free"}});
    const int k = KeyTrigger(graph);
    graph.chain[2].next = -1; graph.chain[static_cast<size_t>(k)].next = 2; graph.chain[2].next = 0; graph.chain[0].next = 1; graph.chain[1].next = 2;
    const std::vector<size_t> order = Order(graph, k);
    Check(order.size() == 3 && order[0] == 2 && order[1] == 0 && order[2] == 1, "order follows the wires, the loop back to C ends it, the free node is out");
    const int lt = InsertAfter(graph, -1, TriggerNode(Trigger::Light, 0, 300));
    Check(lt >= 0 && graph.chain[static_cast<size_t>(lt)].next == -1 && TriggerFor(graph, light) == -1,
          "a trigger with nothing wired does not fire");
    Link(graph, lt, 3);
    Check(TriggerFor(graph, light) == lt && Order(graph, lt).size() == 1 && TriggerOf(graph, 3) == lt,
          "wired to GA_Free: the hero's light attack now fires it");
    graph.chain[static_cast<size_t>(lt)].on = false;
    Check(TriggerFor(graph, light) == -1, "a trigger switched off does not fire");
    Link(graph, 3, lt);
    Check(graph.chain[3].next == -1, "nothing wires into a trigger");

    // Editing.
    Config edit = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}});
    const int ek = KeyTrigger(edit);
    const int mid = InsertAfter(edit, 0, {NodeType::Interval, {}, 400});
    Check(edit.chain[0].next == mid && edit.chain[static_cast<size_t>(mid)].next == 1 && Order(edit, ek).size() == 3, "insert after A: A -> interval -> B");
    const int head = InsertAfter(edit, ek, {NodeType::Skill, "GA_Z"});
    Check(edit.chain[static_cast<size_t>(ek)].next == head && edit.chain[static_cast<size_t>(head)].next == 0, "insert after the key node becomes the first");
    Remove(edit, 0);  // A goes: Z -> interval -> B, indices shift down
    std::vector<std::string> names; for (size_t i : Order(edit, KeyTrigger(edit))) names.push_back(edit.chain[i].type == NodeType::Interval ? "I" : edit.chain[i].ability);
    Check(names.size() == 3 && names[0] == "GA_Z" && names[1] == "I" && names[2] == "GA_B", "removing a node joins the wires around it");
    Check(Tail(edit, KeyTrigger(edit)) >= 0 && edit.chain[static_cast<size_t>(Tail(edit, KeyTrigger(edit)))].ability == "GA_B", "tail is the last wired node");
    Link(edit, 0, 0);
    Check(edit.chain[0].next != 0, "a node never wires to itself");

    // Branch and modifier editing: A0 B1 key2.
    Config br = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}});
    const int bk = KeyTrigger(br);
    const int b = InsertAfter(br, 0, BranchNode(0, 0));
    Check(br.chain[0].next == b && br.chain[static_cast<size_t>(b)].routes.size() == 1 && br.chain[static_cast<size_t>(b)].routes[0].next == 1 && Order(br, bk).size() == 3,
          "a branch inserted after A takes A's wire on its first row");
    br.chain[static_cast<size_t>(b)].routes.push_back({H, 0, -1});
    const int c = InsertAfter(br, b, {NodeType::Skill, "GA_C"}, 1);
    Check(br.chain[static_cast<size_t>(b)].routes[1].next == c && Order(br, bk).size() == 4 && Tail(br, bk) == 1, "a node after the second row is wired from it; the main line ends at B");
    Link(br, b, bk, 0);
    Check(br.chain[static_cast<size_t>(b)].routes[0].next == 1, "a row never wires into a trigger");
    // Arrows to modifiers: A0 B1 key2 branch3 C4, then 加速 5, 武器属性 6.
    const auto at = [&](int i) -> Node& { return br.chain[static_cast<size_t>(i)]; };
    const int speed = static_cast<int>(br.chain.size()); br.chain.push_back(ModifierNode(NodeType::Speed, 0, 0));
    const int weapon = static_cast<int>(br.chain.size()); br.chain.push_back(ModifierNode(NodeType::Weapon, 0, 0));
    Check(!LinkModifier(br, c, weapon) && at(c).mods[kModWeapon] == -1, "a move never points at 武器属性");
    Check(LinkModifier(br, bk, weapon) && LinkModifier(br, c, speed) && at(c).mods[kModSpeed] == speed && WeaponOn(br, bk) && !WeaponOn(br, -1),
          "the key trigger points at 武器属性, C at 加速");
    Link(br, 0, speed); Link(br, speed, 1);
    Check(at(0).next == b && at(speed).next == -1 && Order(br, bk).size() == 4, "no exec wire into or out of a modifier, and the walk never reaches one");
    Check(!LinkModifier(br, b, speed) && !LinkModifier(br, weapon, speed) && !LinkModifier(br, c, 1), "a branch or a modifier points at no modifier, and nothing points at a move this way");
    Check(SpeedFor(br, bk, c) == kSpeedNew && SpeedFor(br, bk, 0) == kSpeedDefault, "C at 加速: C at 150 %, A unchanged");
    const int chain_speed = static_cast<int>(br.chain.size()); br.chain.push_back(ModifierNode(NodeType::Speed, 0, 0));
    at(chain_speed).speed = 200; LinkModifier(br, bk, chain_speed);
    Check(SpeedFor(br, bk, 0) == 200 && SpeedFor(br, bk, c) == kSpeedNew, "the trigger at 加速 ×2: every move at 200 %, C keeps its own");
    LinkModifier(br, 0, chain_speed); LinkModifier(br, 1, chain_speed);
    Check(PointersTo(br, chain_speed).nodes == 3 && PointersTo(br, chain_speed).triggers == 1, "many nodes point at one 加速");
    LinkModifier(br, c, chain_speed);
    Check(at(c).mods[kModSpeed] == chain_speed && SpeedFor(br, bk, c) == 200 && PointersTo(br, speed).nodes == 0, "pointing at another 加速 replaces the first");
    LinkModifier(br, c, speed);
    const int armor = static_cast<int>(br.chain.size()); br.chain.push_back(ModifierNode(NodeType::Armor, 0, 0));
    Check(!ArmorFor(br, bk, 0) && !ArmorFor(br, bk, c), "a 霸体 nothing points at covers nothing");
    LinkModifier(br, c, armor);
    Check(at(c).mods[kModArmor] == armor && at(c).mods[kModSpeed] == speed && ArmorFor(br, bk, c) && !ArmorFor(br, bk, 0), "C at 霸体 too: only C, its 加速 stays");
    LinkModifier(br, bk, armor);
    Check(ArmorFor(br, bk, 0) && ArmorFor(br, bk, c) && !ArmorFor(br, -1, 0), "the trigger at 霸体: every move of its chain");
    UnlinkModifiers(br, bk, kModArmor);
    Check(!ArmorFor(br, bk, 0) && WeaponOn(br, bk) && SpeedFor(br, bk, 0) == 200, "breaking the trigger's 霸体 arrow keeps its others");
    UnlinkInto(br, armor);
    Check(!ArmorFor(br, bk, c) && at(c).mods[kModSpeed] == speed, "breaking every arrow into 霸体");
    {
        // 无敌: a modifier like 霸体, its own slot; a move or the trigger may point at it, the arrows keep the others.
        const int inv = static_cast<int>(br.chain.size()); br.chain.push_back(ModifierNode(NodeType::Invincible, 0, 0));
        Check(IsModifier(br.chain.back()) && SlotOf(NodeType::Invincible) == kModInvincible && !InvincibleFor(br, bk, c), "a 无敌 nothing points at covers nothing");
        Check(LinkModifier(br, c, inv) && at(c).mods[kModInvincible] == inv && at(c).mods[kModSpeed] == speed && InvincibleFor(br, bk, c) && !InvincibleFor(br, bk, 0),
              "C at 无敌: only C, its 加速 stays");
        Check(LinkModifier(br, bk, inv) && InvincibleFor(br, bk, 0) && !ArmorFor(br, bk, 0), "the trigger at 无敌: every move of its chain, no 霸体 from it");
        const Config round = Decode(Encode(br));
        Check(Encode(br).find(L"|N:") != std::wstring::npos && round.chain[static_cast<size_t>(inv)].type == NodeType::Invincible && InvincibleFor(round, bk, 0) &&
              InvincibleFor(round, bk, c) && Encode(round) == Encode(br), "无敌 saved as N and its arrows read back");
        Remove(br, static_cast<size_t>(inv));
        Check(!InvincibleFor(br, bk, c) && !InvincibleFor(br, bk, 0) && at(c).mods[kModSpeed] == speed, "removing 无敌 breaks its arrows only");
    }
    Remove(br, static_cast<size_t>(armor));
    Remove(br, static_cast<size_t>(c));  // 加速 5 -> 4, 武器属性 6 -> 5, the chain's 加速 7 -> 6
    Check(at(b).routes[1].next == -1 && at(4).type == NodeType::Speed && PointersTo(br, 4).nodes == 0 && at(bk).mods[kModWeapon] == 5 &&
          at(bk).mods[kModSpeed] == 6 && at(0).mods[kModSpeed] == 6 && at(1).mods[kModSpeed] == 6, "removing C unwires its row and its arrows; the other arrows follow their modifiers");
    Remove(br, 6);
    Check(at(bk).mods[kModSpeed] == -1 && at(0).mods[kModSpeed] == -1 && SpeedFor(br, bk, 0) == kSpeedDefault && WeaponOn(br, bk), "removing a 加速 breaks the arrows into it");
    Remove(br, static_cast<size_t>(b));
    Check(at(0).next == 1, "removing a branch joins A to its first row's move");

    // Play rate: 加速 percent of the rate now; a move shorter than its interval is slowed to fill it, a longer one keeps it.
    Check(Near(MoveRate(100, 2000, 1.0, 1.0, 1.0), 0.5), "1 s move, 2 s interval -> half speed");
    Check(Near(MoveRate(100, 500, 1.0, 1.0, 1.0), 1.0), "interval shorter than the move -> unchanged");
    Check(Near(MoveRate(100, 2000, 1.0, 1.0, 2.0), 0.25), "RateScale 2: the move lasts 0.5 s -> quarter speed");
    Check(Near(MoveRate(100, 3000, 1.2, 1.2, 1.0), 0.4), "already at 1.2x: 1 s -> 3 s is rate 0.4");
    Check(Near(MoveRate(100, 1010, 1.0, 1.0, 1.0), 1.0), "within 2 %: no change");
    Check(Near(MoveRate(100, 100000, 1.0, 1.0, 1.0), 0.05), "never below 0.05");
    Check(Near(MoveRate(100, 0, 1.0, 1.0, 1.0), 1.0) && Near(MoveRate(100, 2000, 0, 1.0, 1.0), 1.0), "no interval / unknown length -> unchanged");
    Check(Near(MoveRate(150, 0, 1.0, 1.0, 1.0), 1.5) && Near(MoveRate(200, 0, 2.0, 0.8, 1.0), 1.6), "加速 150 % / 200 % of the rate now");
    Check(Near(MoveRate(200, 400, 1.0, 1.0, 1.0), 2.0) && Near(MoveRate(200, 800, 1.0, 1.0, 1.0), 1.25), "sped up to 0.5 s: a 0.4 s interval keeps 2x, a 0.8 s one fills it at 1.25x");
    Check(Near(MoveRate(1000, 0, 1.0, 1.0, 1.0), 10.0) && Near(MoveRate(2000, 0, 1.0, 1.0, 1.0), 10.0) && Near(MoveRate(100, 0, 1.0, 0, 1.0), 0.0) && kSpeedMax == 1000,
          "加速 up to ×10 (1000 %); an unknown rate stays");
    Check(Near(Lasts(2.0, 1.0, 0.5), 4.0), "Lasts follows rate and RateScale");

    // Stick direction relative to facing.
    Check(Direction(0, 0) == 'B' && Direction(0.9, 0.1) == 'F' && Direction(-0.9, 0.2) == 'B' && Direction(0.1, 0.8) == 'R' && Direction(0.2, -0.9) == 'L',
          "centred stick -> back; otherwise the larger axis");

    // ini text.
    Config config; config.enabled = false;
    config.chain = {{NodeType::Skill, "GA_MuXiaoKui_Skill_01", 0, 200, 40, 1}, {NodeType::Interval, {}, 750, 400, 40, 2}, {NodeType::Skill, "GA_MuXiaoKui_Dodge_B", 0, 600, -30, -1},
                    TriggerNode(Trigger::NpcKey, 20, 40), TriggerNode(Trigger::Heavy, 20, 300), {NodeType::AutoDodge, {}, 0, 300, 300, -1}};
    config.chain[3].next = 0; config.chain[4].next = 5; config.chain[4].ms = 900; config.chain[4].on = false;
    const std::wstring text = Encode(config);
    Check(text == L"E0|S:GA_MuXiaoKui_Skill_01,200,40,1|I:750,400,40,2|S:GA_MuXiaoKui_Dodge_B,600,-30,-1|T:0,1200,1,20,40,0|T:2,900,0,20,300,5|A:300,300,-1", "encode");
    {
        Config cut = config; cut.chain[0].ms = 400; cut.chain[5].ms = 12000;
        const std::wstring ctext = Encode(cut);
        Check(ctext == L"E0|S:GA_MuXiaoKui_Skill_01,200,40,1,400|I:750,400,40,2|S:GA_MuXiaoKui_Dodge_B,600,-30,-1|T:0,1200,1,20,40,0|T:2,900,0,20,300,5|A:300,300,-1,10000",
              "encode a move's 派生点 as one more field (at most 10 s)");
        const Config cback = Decode(ctext);
        Check(cback.chain[0].ms == 400 && cback.chain[0].next == 1 && cback.chain[0].x == 200 && cback.chain[5].ms == kCancelMax && cback.chain[2].ms == 0 && Encode(cback) == ctext,
              "decode it back, the same text again");
    }
    Check(Decode(L"E1|T:0,50,1,0,0,-1").chain[0].ms == kResetMin && kResetMin == 100 && Decode(L"E1|T:0,100,1,0,0,-1").chain[0].ms == 100,
          "a replaced button's reset time is at least 0.1 s");
    const Config back = Decode(text);
    Check(!back.enabled && back.placed && back.chain.size() == 6 && back.chain[1].ms == 750 && back.chain[2].y == -30, "decode the nodes");
    Check(back.chain[3].type == NodeType::Trigger && back.chain[3].next == 0 && back.chain[4].trigger == static_cast<int>(Trigger::Heavy) && back.chain[4].ms == 900 &&
          !back.chain[4].on && back.chain[5].type == NodeType::AutoDodge, "decode triggers and the stick's dodge");
    Check(Decode(L"E1|T:0,1200,1,0,0,7|S:GA_A,0,0,0").chain[0].next == -1 && Decode(L"E1|T:0,1200,1,0,0,1|S:GA_A,0,0,0").chain[1].next == -1,
          "wires to missing nodes or into a trigger are dropped");
    Check(Decode(L"E1|S:GA_A,0,0,-1|T:3,1200,1,0,0,0").chain.size() == 3 && KeyTrigger(Decode(L"E1|S:GA_A,0,0,-1|T:3,1200,1,0,0,0")) == 2,
          "a config without an NPC key node gets one");
    Config kc; kc.chain = {{NodeType::Skill, "GA_A", 0, 10, 20, 1}};
    Node kb = BranchNode(100, 20); kb.routes = {{L, 0, -1}, {H, 2000, 0}, {K, 0, 3}};
    kc.chain.push_back(kb);
    kc.chain.push_back(TriggerNode(Trigger::NpcKey, 0, 0)); kc.chain[2].next = 0;
    kc.chain.push_back({NodeType::Skill, "GA_B", 0, 300, 20, -1});
    Node v = ModifierNode(NodeType::Speed, 5, 6); v.speed = 180; kc.chain.push_back(v);
    kc.chain.push_back(ModifierNode(NodeType::Weapon, 7, 8)); kc.chain.push_back(ModifierNode(NodeType::Armor, 9, 10));
    kc.chain[0].mods[kModSpeed] = 4; kc.chain[0].mods[kModArmor] = 6; kc.chain[2].mods[kModWeapon] = 5; kc.chain[3].mods[kModSpeed] = 4;
    const std::wstring ktext = Encode(kc);
    Check(ktext == L"E1|S:GA_A,10,20,1|K:100,20,1,0,-1,2,2000,0,0,0,3|T:0,1200,1,0,0,0|S:GA_B,300,20,-1|V:180,5,6|W:7,8|H:9,10|L:0,4|L:0,6|L:2,5|L:3,4",
          "encode a branch, the modifiers and the arrows to them");
    const Config kback = Decode(ktext);
    Check(kback.chain.size() == 7 && kback.chain[6].type == NodeType::Armor && kback.chain[6].x == 9 && ArmorFor(kback, 2, 0) && !ArmorFor(kback, 2, 3) &&
          SpeedFor(kback, 2, 0) == 180 && SpeedFor(kback, 2, 3) == 180 && WeaponOn(kback, 2) && kback.chain[4].speed == 180, "decode the arrows: 霸体 at A only, 加速 at A and B, 武器属性 at the key");
    Check(kback.chain[1].type == NodeType::Branch && kback.chain[1].routes.size() == 3 && kback.chain[1].routes[1].pause_ms == 2000 &&
          kback.chain[1].routes[2].button == K && kback.chain[1].routes[2].next == 3 && kback.chain[4].next == -1 && Encode(kback) == ktext, "decode them back, the same text again");
    {
        const Config old = Decode(L"E1|S:GA_A,10,20,-1|T:0,1200,1,0,0,0|V:180,5,6,0|W:7,8,1|H:9,10,0|W:1,1,0");
        Check(old.chain[0].mods[kModSpeed] == 2 && old.chain[0].mods[kModArmor] == 4 && old.chain[1].mods[kModWeapon] == 3 && old.chain[0].mods[kModWeapon] == -1 &&
              old.chain[2].x == 5 && old.chain[2].next == -1, "older modifiers that pointed at a node become that node's arrows (武器属性 at a move dropped)");
    }
    {
        const Config both = Decode(L"E1|S:GA_A,0,0,-1|V:150,0,0|H:0,0|L:0,1|L:0,2");
        const Config mixed = Decode(L"E1|S:GA_A,0,0,-1|S:GA_B,0,0,-1|I:500,0,0,-1|V:150,0,0|L:0,1|L:2,3|L:0,9|L:-3,0|L:3,3");
        Check(both.chain[0].mods[kModSpeed] == 1 && both.chain[0].mods[kModArmor] == 2 && mixed.chain[0].mods[kModSpeed] == -1 && mixed.chain[2].mods[kModSpeed] == -1 &&
              mixed.chain[3].mods[kModSpeed] == -1, "加速 / 霸体 arrows from a move stay; arrows to a move, from an interval, out of range or from a modifier are dropped");
    }
    Check(Decode(L"E1|K:0,0,1,0,1|T:0,1200,1,0,0,-1").chain[0].routes[0].next == -1 && Decode(L"E1|W:0,0|S:GA_A,0,0,-1|L:1,0").chain[1].mods[kModWeapon] == -1 &&
          Decode(L"E1|S:GA_A,0,0,1|V:150,0,0").chain[0].next == -1 && Decode(L"E1|K:0,0,99,0,-1,1,0,-1").chain[0].routes.size() == 1 &&
          Decode(L"E1|V:9999,0,0").chain[0].speed == kSpeedMax,
          "a row into a trigger, a move's arrow to 武器属性, an exec wire into a modifier and a bad button are dropped; speed clamped");

    // 允许中断: a trigger (its whole chain) or a move (that move alone) points at it; saved as C:x,y and its arrows.
    {
        Config ic = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}});  // A0 B1 key2
        const int ik = KeyTrigger(ic);
        ic.chain.push_back(ModifierNode(NodeType::Interrupt, 30, 40));
        const int cut = static_cast<int>(ic.chain.size()) - 1;
        Check(ik == 2 && IsModifier(ic.chain[static_cast<size_t>(cut)]) && SlotOf(NodeType::Interrupt) == kModInterrupt && ModifierFits(ic.chain, 0, cut) &&
              ModifierFits(ic.chain, ik, cut) && !ModifierFits(ic.chain, cut, 0), "允许中断 is a modifier a move or a trigger may point at");
        Check(!InterruptFor(ic, ik, 0) && LinkModifier(ic, 1, cut) && InterruptFor(ic, ik, 1) && !InterruptFor(ic, ik, 0), "a move's arrow: only that move may be cut at any time");
        LinkModifier(ic, ik, cut);
        Check(InterruptFor(ic, ik, 0) && InterruptFor(ic, ik, 1) && !InterruptFor(ic, -1, 0) && !ArmorFor(ic, ik, 0) && SpeedFor(ic, ik, 0) == kSpeedDefault,
              "the trigger's arrow: every move of its chain, and nothing else changes");
        const std::wstring itext = Encode(ic);
        Check(itext.find(L"|C:30,40|") != std::wstring::npos && itext.find(L"|L:1," + std::to_wstring(cut)) != std::wstring::npos &&
              itext.find(L"|L:2," + std::to_wstring(cut)) != std::wstring::npos, "saved as C:x,y and its arrows");
        const Config iback = Decode(itext);
        Check(iback.chain[static_cast<size_t>(cut)].type == NodeType::Interrupt && InterruptFor(iback, ik, 0) && Encode(iback) == itext, "read back, the same text again");
        Remove(ic, 0);
        Check(InterruptFor(ic, ik - 1, 0) && ic.chain[static_cast<size_t>(ik - 1)].mods[kModInterrupt] == cut - 1, "removing a node shifts the arrows into 允许中断");
        Remove(ic, static_cast<size_t>(cut - 1));
        Check(!InterruptFor(ic, ik - 1, 0) && ic.chain[0].mods[kModInterrupt] == -1, "removing 允许中断 breaks the arrows into it");
    }

    // 投技: a move of the chain once it names its grab (a press reaches it), with no modifiers, no 派生点 and no
    // 允许中断; saved as G:x,y,next[,id]. Without a grab picked it is passed over (never the 投技 page's pick).
    {
        Config gc = Linked({{NodeType::Skill, "GA_A"}});  // A0 key1
        Node grab; grab.type = NodeType::Grab; grab.x = 5; grab.y = 6;
        const int g = InsertAfter(gc, 0, grab), gk = KeyTrigger(gc);
        Check(!IsMove(gc.chain[static_cast<size_t>(g)]) && EmptyMove(gc.chain[static_cast<size_t>(g)]) && Follow(gc, 0, K, key, 100).move != g,
              "a grab node with no grab picked is passed over");
        gc.chain[static_cast<size_t>(g)].ability = "02";
        gc.chain.push_back(ModifierNode(NodeType::Interrupt, 0, 0)); const int gi = static_cast<int>(gc.chain.size()) - 1;
        gc.chain.push_back(ModifierNode(NodeType::Speed, 0, 0)); const int gs = static_cast<int>(gc.chain.size()) - 1;
        const Node& gn = gc.chain[static_cast<size_t>(g)];
        Check(IsMove(gn) && gc.chain[0].next == g && Follow(gc, 0, K, key, 100).move == g, "a press after A plays the grab node");
        Check(TakesModifiers(gn) && !LinkModifier(gc, g, gs) && !InterruptFor(gc, gk, g), "a grab points at no 加速 and is not cut without 允许中断");
        gc.chain.push_back(ModifierNode(NodeType::Armor, 0, 0)); const int ga = static_cast<int>(gc.chain.size()) - 1;
        gc.chain.push_back(ModifierNode(NodeType::Damage, 0, 0)); const int gd = static_cast<int>(gc.chain.size()) - 1;
        Check(!LinkModifier(gc, g, ga) && !LinkModifier(gc, g, gd) && LinkModifier(gc, g, gi) && InterruptFor(gc, gk, g) && !InterruptFor(gc, gk, 0),
              "a grab points at 允许中断 only (the boss body plays it out); its own 允许中断 is not the chain's");
        const std::wstring itext = Encode(gc);
        const Config iback = Decode(itext);
        Check(itext.find(L"|L:" + std::to_wstring(g) + L"," + std::to_wstring(gi)) != std::wstring::npos && InterruptFor(iback, KeyTrigger(iback), g) && Encode(iback) == itext,
              "a grab's 允许中断 is saved (L:grab,mod) and read back");
        UnlinkModifiers(gc, g);
        Check(!InterruptFor(gc, gk, g), "unlinked: the grab plays to its end again");
        gc.chain.pop_back(); gc.chain.pop_back();
        gc.chain[static_cast<size_t>(g)].ms = 500; LinkModifier(gc, gk, gi); LinkModifier(gc, gk, gs);
        Check(CancelOf(gc, g) == 0 && CutMs(gc, g) == 0 && InterruptFor(gc, gk, g) && InterruptFor(gc, gk, 0) && SpeedFor(gc, gk, g) == kSpeedDefault &&
              SpeedFor(gc, gk, 0) == kSpeedNew, "the chain's 允许中断 cuts the grab too; its 加速 skips it; it has no 派生点");
        const std::wstring gtext = Encode(gc);
        const Config gback = Decode(gtext);
        Check(gtext.find(L"|G:5,6,-1,02") != std::wstring::npos && gback.chain[static_cast<size_t>(g)].type == NodeType::Grab && gback.chain[0].next == g &&
              Encode(gback) == gtext, "saved as G:x,y,next,id and read back");
        // A node that names a boss grab keeps its id (letters, digits, _ : .); a bad id is dropped (no grab picked).
        gc.chain[static_cast<size_t>(g)].ability = "MoTian:GA_MoTian_GEx_01:BD_MoTian_GEx_01:BP_BulletEvent_Grab_C_0";
        const std::wstring ntext = Encode(gc);
        const Config nback = Decode(ntext);
        Check(ntext.find(L"|G:5,6,-1,MoTian:GA_MoTian_GEx_01:BD_MoTian_GEx_01:BP_BulletEvent_Grab_C_0") != std::wstring::npos &&
              nback.chain[static_cast<size_t>(g)].ability == gc.chain[static_cast<size_t>(g)].ability && Encode(nback) == ntext,
              "a grab node's move id is saved as G:x,y,next,id and read back");
        const Config bad = Decode(L"E1|G:1,2,-1,bad id!|G:3,4,-1,02|G:5,6");
        std::vector<std::string> grabs; for (const Node& n : bad.chain) if (n.type == NodeType::Grab) grabs.push_back(n.ability);
        Check(grabs.size() == 2 && grabs[0].empty() && grabs[1] == "02",
              "a bad grab id is dropped (no grab picked), a short one kept, a G: without next skipped");
    }

    // BOSS 技能: a move of another look, played in that look; it takes modifiers and a 派生点 like a move; saved as
    // X:look,GA,x,y,next[,cut]. Without its look or move it is passed over like a move with a bad name.
    {
        Config bc = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}});  // A0 B1 key2
        Node boss; boss.type = NodeType::BossSkill; boss.boss = "MuXiaoKui"; boss.ability = "GA_MuXiaoKui_Skill_01"; boss.x = 7; boss.y = 8; boss.ms = 450;
        const int x = InsertAfter(bc, 0, boss), xk = KeyTrigger(bc);
        bc.chain.push_back(ModifierNode(NodeType::Speed, 0, 0)); const int xs_ = static_cast<int>(bc.chain.size()) - 1;
        const Node bn = bc.chain[static_cast<size_t>(x)];
        Check(IsMove(bn) && !EmptyMove(bn) && bc.chain[0].next == x && bc.chain[static_cast<size_t>(x)].next == 1 && Follow(bc, 0, K, key, 100).move == x,
              "a press after A plays the boss move, then B");
        Check(TakesModifiers(bn) && LinkModifier(bc, x, xs_) && SpeedFor(bc, xk, x) == kSpeedNew && CancelOf(bc, x) == 450,
              "a boss move points at 加速 and keeps its 派生点");
        const std::wstring btext = Encode(bc);
        const Config bback = Decode(btext);
        const Node& bb = bback.chain[static_cast<size_t>(x)];
        Check(btext.find(L"|X:MuXiaoKui,GA_MuXiaoKui_Skill_01,7,8,1,450") != std::wstring::npos && bb.type == NodeType::BossSkill && bb.boss == "MuXiaoKui" &&
              bb.ability == "GA_MuXiaoKui_Skill_01" && bb.ms == 450 && bb.mods[kModSpeed] == xs_ && Encode(bback) == btext,
              "saved as X:look,GA,x,y,next,cut and read back (with its arrow)");
        Config empty = bc; empty.chain[static_cast<size_t>(x)].ability.clear();
        Check(!IsMove(empty.chain[static_cast<size_t>(x)]) && EmptyMove(empty.chain[static_cast<size_t>(x)]) && Follow(empty, 0, K, key, 100).move == 1,
              "a boss node without its move is passed over: the press plays B");
        const std::wstring etext = Encode(empty);
        Check(etext.find(L"|X:MuXiaoKui,_,7,8,1,450") != std::wstring::npos && Decode(etext).chain[static_cast<size_t>(x)].ability.empty() &&
              Decode(etext).chain[static_cast<size_t>(x)].boss == "MuXiaoKui", "an unpicked move is saved as _ and read back empty");
        const Config xbad = Decode(L"E1|X:bad look!,GA_X,1,2,-1|X:Ok,GA_Y,1,2|X:Ok,GA_Z,3,4,-1,99999");
        std::vector<const Node*> xs; for (const Node& n : xbad.chain) if (n.type == NodeType::BossSkill) xs.push_back(&n);
        Check(xs.size() == 2 && xs[0]->boss.empty() && xs[0]->ability == "GA_X" && xs[1]->boss == "Ok" && xs[1]->ms == ClampCancel(99999),
              "a bad look id is dropped, an X: without next skipped, a long 派生点 clamped");
    }

    // 武器派生: a hero weapon's ability (Y:weapon,GA,x,y,next[,cut]); a move with modifiers and a 派生点; passed over
    // without its weapon or move.
    {
        Config wc = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}});  // A0 B1 key2
        Node move; move.type = NodeType::WeaponMove; move.weapon = "Long"; move.ability = "GA_Hun_Long_AAA"; move.x = 3; move.y = 4; move.ms = 300;
        const int w = InsertAfter(wc, 0, move), wk = KeyTrigger(wc);
        wc.chain.push_back(ModifierNode(NodeType::Damage, 0, 0)); const int wd = static_cast<int>(wc.chain.size()) - 1;
        const Node wn = wc.chain[static_cast<size_t>(w)];
        Check(IsMove(wn) && !EmptyMove(wn) && TakesModifiers(wn) && Follow(wc, 0, K, key, 100).move == w && CancelOf(wc, w) == 300,
              "a weapon move is a move of the chain with its 派生点");
        Check(wc.chain[static_cast<size_t>(wd)].speed == kDamageNew && IsModifier(wc.chain[static_cast<size_t>(wd)]) && LinkModifier(wc, w, wd) &&
              DamageFor(wc, wk, w) == kDamageNew && DamageFor(wc, wk, 0) == kDamageDefault, "伤害倍率 (200% new) scales only the move pointing at it");
        Check(LinkModifier(wc, wk, wd) && DamageFor(wc, wk, 0) == kDamageNew, "a trigger pointing at it scales its whole chain");
        wc.chain[static_cast<size_t>(wd)].speed = 1500;
        Check(DamageFor(wc, wk, w) == kDamageMax, "at most 1000%");
        wc.chain[static_cast<size_t>(wd)].speed = 0;
        const std::wstring wtext = Encode(wc);
        const Config wback = Decode(wtext);
        Check(wtext.find(L"|Y:Long,GA_Hun_Long_AAA,3,4,1,300") != std::wstring::npos && wtext.find(L"|D:0,0,0") != std::wstring::npos &&
              wback.chain[static_cast<size_t>(w)].weapon == "Long" && wback.chain[static_cast<size_t>(w)].ability == "GA_Hun_Long_AAA" &&
              DamageFor(wback, wk, w) == 0 && Encode(wback) == wtext, "saved as Y:weapon,GA,... and D:percent,x,y and read back (0% stays 0)");
        Config bare = wc; bare.chain[static_cast<size_t>(w)].weapon.clear();
        Check(!IsMove(bare.chain[static_cast<size_t>(w)]) && Follow(bare, 0, K, key, 100).move == 1 && Encode(bare).find(L"|Y:_,GA_Hun_Long_AAA") != std::wstring::npos,
              "without its weapon the node is passed over and saved as _");
        Check(Decode(L"E1|Y:Long,GA_X,1,2|D:50,1|Y:bad id!,GA_Y,1,2,-1").chain.size() == 2 && Decode(L"E1|D:5000,1,2").chain[0].speed == kDamageMax,
              "a Y: without next and a short D: are skipped; a bad weapon id is dropped; a big percent is clamped");
    }

    // 处决: a trigger for the hero's executions (paired or simple), all of them or one weapon's (T:13,...,weapon).
    {
        const unsigned paired = ActionBits("Ability.Action.Execution.Channel.Execution"), simple = ActionBits("Ability.Action.Hun.SimpleExecution");
        Check(Fires(Trigger::Execution, paired) && Fires(Trigger::Execution, simple) && !Fires(Trigger::Light, simple) &&
              !Fires(Trigger::AnyDodge, paired) && !Fires(Trigger::Execution, ActionBits("Ability.Action.Hun.Light.AA")),
              "executions (paired and simple) fire 处决 and nothing else");
        Config ec = Linked({{NodeType::Skill, "GA_A"}});  // A0 key1
        Node exe = TriggerNode(Trigger::Execution, 9, 9); exe.next = 0; exe.weapon = "Long"; ec.chain.push_back(exe);
        const int e = static_cast<int>(ec.chain.size()) - 1;
        Press with_long{paired, false, "Long"}, with_tang{paired, false, "Tang"};
        Check(TriggerFor(ec, with_long) == e && TriggerFor(ec, with_tang) == -1, "a 处决 trigger naming a weapon starts only with that weapon in hand");
        ec.chain[static_cast<size_t>(e)].weapon.clear();
        Check(TriggerFor(ec, with_tang) == e, "without a weapon it takes every execution");
        ec.chain[static_cast<size_t>(e)].weapon = "Long";
        const std::wstring etext = Encode(ec);
        Check(etext.find(L"|T:13,1200,1,9,9,0,Long") != std::wstring::npos && Decode(etext).chain[static_cast<size_t>(e)].weapon == "Long" && Encode(Decode(etext)) == etext,
              "saved as T:13,...,next,weapon and read back");
        Check(Decode(L"E1|T:1,1200,1,0,0,-1,Long").chain[0].weapon.empty() && Decode(L"E1|K:0,0,13,0,-1,1,0,-1").chain[0].routes.size() == 1,
              "only a 处决 trigger keeps a weapon; a branch row never takes 处决");
    }

    // 武器派生修改: one weapon's hero actions start chains of their own (O:x,y,weapon,reset,action,next,...).
    {
        const unsigned paired = ActionBits("Ability.Action.Execution.Channel.Execution"), simple = ActionBits("Ability.Action.Hun.SimpleExecution");
        const unsigned perfect = ActionBits("Ability.Action.Hun.PerfectDodge"), stance = ActionBits("Ability.Action.Stance.Guard");
        Check(Fires(Trigger::SimpleExecution, simple) && Fires(Trigger::Execution, simple) && !Fires(Trigger::SimpleExecution, paired) &&
              !ValidRouteButton(static_cast<int>(Trigger::SimpleExecution)) && std::wstring(TriggerLabel(Trigger::SimpleExecution)) == L"主角简易处决" &&
              WeaponTrigger(static_cast<int>(Trigger::SimpleExecution)) && Decode(L"E1|T:17,1200,1,0,0,-1,Long").chain[0].weapon == "Long",
              "简易处决: a trigger of its own (a 处决 trigger takes it too), never a branch row, may name a weapon");
        Config wc = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_B"}, {NodeType::Skill, "GA_C"}});  // A0 B1 C2 key3
        wc.chain[0].next = -1; wc.chain[1].next = -1;
        Node wa = WeaponActionsNode(5, 6); wa.weapon = "Long";
        Check(wa.routes.size() == 4 && wa.routes[0].button == static_cast<int>(Trigger::PerfectDodge) && wa.routes[2].button == static_cast<int>(Trigger::SimpleExecution) &&
              wa.routes[3].button == static_cast<int>(Trigger::Stance) && OutCount(wa) == 4 && !Groupable(wa) && TakesModifiers(wa) && !HasWeaponActions(wc),
              "a new 武器派生修改 has the rows 完美闪避 / 处决 / 简易处决 / 格挡, an output each, takes modifiers, never sits in a 组合");
        wc.chain.push_back(wa); const int w = static_cast<int>(wc.chain.size()) - 1;
        Link(wc, w, 0, 0); Link(wc, w, 1, 1); Link(wc, w, 2, 3);  // 完美闪避 -> A, 处决 -> B, 格挡 -> C
        Link(wc, 0, w);
        Check(IsTrigger(wc, w) && HasWeaponActions(wc) && wc.chain[0].next == -1 && wc.chain[static_cast<size_t>(w)].routes[1].next == 1 &&
              TriggerOf(wc, 1) == w, "no exec wire goes into it; its rows are its outputs and its chains are its own");
        const Press dodge_long{perfect, false, "Long"}, dodge_tang{perfect, false, "Tang"}, paired_long{paired, false, "Long"}, simple_long{simple, false, "Long"},
            guard_long{stance, false, "Long"}, paired_none{paired, false, ""};
        Check(TriggerFor(wc, dodge_long) == w && TriggerFor(wc, dodge_tang) == -1 && TriggerFor(wc, guard_long) == w && TriggerFor(wc, paired_none) == -1 &&
              TriggerFor(wc, Press{ActionBits("Ability.Action.Hun.Jump"), false, "Long"}) == -1,
              "it starts only with its weapon in hand, for the actions of its wired rows");
        Check(Follow(wc, w, StartButton(wc, w, dodge_long), dodge_long, -1).move == 0 && Follow(wc, w, -1, guard_long, -1).move == 2 &&
              Follow(wc, w, -1, guard_long, -1).button == static_cast<int>(Trigger::Stance) && StartButton(wc, w, guard_long) == static_cast<int>(Trigger::Stance) &&
              StartButton(wc, w, dodge_tang) == -1 && Follow(wc, w, -1, dodge_tang, -1).move < 0,
              "a press goes on from its action's row, on that action's button");
        Check(Follow(wc, w, -1, paired_long, -1).move == 1 && Follow(wc, w, -1, simple_long, -1).move == 1, "处决 takes the quick executions too while 简易处决 is unwired");
        Link(wc, w, 2, 2);
        Check(Follow(wc, w, -1, simple_long, -1).move == 2 && Follow(wc, w, -1, paired_long, -1).move == 1, "a wired 简易处决 row takes the quick ones");
        Node any = TriggerNode(Trigger::PerfectDodge, 0, 0); any.next = 2; wc.chain.push_back(any); const int t = static_cast<int>(wc.chain.size()) - 1;
        Check(TriggerFor(wc, dodge_long) == w && TriggerFor(wc, dodge_tang) == t, "it goes before a plain 完美闪避 trigger; other weapons keep that one");
        wc.chain.push_back(ModifierNode(NodeType::Weapon, 0, 0)); const int wm = static_cast<int>(wc.chain.size()) - 1;
        wc.chain.push_back(ModifierNode(NodeType::KeepLook, 0, 0)); const int km = static_cast<int>(wc.chain.size()) - 1;
        Check(LinkModifier(wc, w, wm) && LinkModifier(wc, w, km) && WeaponOn(wc, w), "it points at 武器属性 / 保留变身 as a trigger does");
        const std::wstring wtext = Encode(wc);
        Check(wtext.find(L"|O:5,6,Long,1200,9,0,13,1,17,2,16,2") != std::wstring::npos && Encode(Decode(wtext)) == wtext &&
              Decode(wtext).chain[static_cast<size_t>(w)].type == NodeType::WeaponActions && Decode(wtext).chain[static_cast<size_t>(w)].weapon == "Long",
              "saved as O:x,y,weapon,reset,action,next,... and read back");
        const Config odd = Decode(L"E1|O:0,0,_,1200,0,-1,14,-1,9,-1|O:0,0,Long,1200,9");
        Check(odd.chain[0].type == NodeType::WeaponActions && odd.chain[0].weapon.empty() && odd.chain[0].routes.size() == 1 &&
              odd.chain[1].type == NodeType::Trigger,
              "the NPC key / own-keys rows are dropped, no weapon is _, a cut-off O: is skipped");
        Config gone = wc; Remove(gone, 0);
        Check(gone.chain[static_cast<size_t>(w - 1)].routes[0].next == -1 && gone.chain[static_cast<size_t>(w - 1)].routes[1].next == 0 &&
              gone.chain[static_cast<size_t>(w - 1)].routes[3].next == 1, "removing a move its row fed unwires that row and keeps the others");
    }

    // 组合: one move of the chain made of the nodes in its frame, played one after another. Order: the frame's own
    // wires, else left to right then top to bottom; a 组合 inside plays its own nodes there. Modifiers reach the nodes
    // through their frames. Saved as U:x,y,w,h,next,cut,members.
    {
        Config gc = Linked({{NodeType::Skill, "GA_A"}, {NodeType::Skill, "GA_Z"}});  // A0 Z1 key2
        Node frame; frame.type = NodeType::Group; frame.x = 100; frame.y = 0; frame.w = 500; frame.h = 300; frame.ms = 700;
        const int g = InsertAfter(gc, 0, frame), gk = KeyTrigger(gc);  // A0 -> g -> Z1
        const auto add = [&](NodeType type, const char* ability, int x, int y, int in) {
            Node n; n.type = type; n.ability = ability; n.x = x; n.y = y; n.group = in;
            if (type == NodeType::Group) { n.w = 300; n.h = 180; }
            gc.chain.push_back(n); return static_cast<int>(gc.chain.size()) - 1;
        };
        const int gb = add(NodeType::Skill, "GA_B", 300, 50, g), gx = add(NodeType::Skill, "GA_C", 120, 50, g), gt = add(NodeType::Teleport, "", 200, 200, g);
        Check(IsMove(gc.chain[static_cast<size_t>(g)]) && TakesModifiers(gc.chain[static_cast<size_t>(g)]) && Follow(gc, 0, K, key, 100).move == g &&
              CancelOf(gc, g) == 700 && Continues(gc, g), "a 组合 is one move of the chain, with its own 派生点 and output");
        Check(Members(gc, g) == std::vector<int>{gx, gt, gb}, "without wires its nodes play left to right, then top to bottom");
        gc.chain[static_cast<size_t>(gb)].next = gx;  // B -> C inside the frame
        Check(Members(gc, g) == std::vector<int>{gt, gb, gx}, "the frame's own wires set the order (B then C)");
        const int gi = add(NodeType::Group, "", 400, 150, g), ge = add(NodeType::Skill, "GA_E", 410, 160, gi);
        Check(GroupSteps(gc, g) == std::vector<int>{gt, gb, gx, ge} && GroupDepth(gc, ge) == 2 && InGroup(gc, ge, g) && !InGroup(gc, gb, gi),
              "a 组合 inside plays its own nodes in its place");
        gc.chain.push_back(ModifierNode(NodeType::Speed, 0, 0)); const int gs = static_cast<int>(gc.chain.size()) - 1;
        gc.chain.push_back(ModifierNode(NodeType::Speed, 0, 0)); const int gs2 = static_cast<int>(gc.chain.size()) - 1; gc.chain[static_cast<size_t>(gs2)].speed = 300;
        gc.chain.push_back(ModifierNode(NodeType::Interrupt, 0, 0)); const int gu = static_cast<int>(gc.chain.size()) - 1;
        Check(LinkModifier(gc, g, gs) && SpeedFor(gc, gk, gb) == kSpeedNew && SpeedFor(gc, gk, ge) == kSpeedNew && SpeedFor(gc, gk, 0) == kSpeedDefault,
              "加速 on the 组合 speeds every node in it (inner frames too), nothing outside");
        Check(LinkModifier(gc, gb, gs2) && SpeedFor(gc, gk, gb) == 300 && SpeedFor(gc, gk, gx) == kSpeedNew, "a node's own arrow goes first");
        Check(LinkModifier(gc, gi, gu) && InterruptFor(gc, gk, ge) && !InterruptFor(gc, gk, gb), "允许中断 on the inner 组合 covers only its own nodes");
        Check(!CanJoin(gc, g, gi) && !CanJoin(gc, gk, g) && !CanJoin(gc, gs, g) && !CanJoin(gc, g, g) && CanJoin(gc, 1, g) && CanJoin(gc, gb, -1),
              "no 组合 goes into itself or a frame inside it; triggers and modifiers stay out");
        const std::wstring gtext = Encode(gc);
        const Config gback = Decode(gtext);
        Check(gtext.find(L"|U:100,0,500,300,1,700,") != std::wstring::npos && gback.chain[static_cast<size_t>(ge)].group == gi && gback.chain[static_cast<size_t>(gb)].group == g &&
              GroupSteps(gback, g) == GroupSteps(gc, g) && Encode(gback) == gtext, "saved with its frame's node list and read back");
        Config gone = gc; Remove(gone, static_cast<size_t>(g));
        Check(gone.chain[static_cast<size_t>(gb - 1)].group == -1 && gone.chain[static_cast<size_t>(ge - 1)].group == gi - 1 && gone.chain[0].next == 1 &&
              gone.chain[1].ability == "GA_Z", "removing a 组合 leaves its nodes on the canvas (inner frames keep theirs) and the chain goes on through it");
        Config deep = Linked({});
        int last = -1;
        for (int i = 0; i < kGroupDepthMax + 1; ++i) { Node n; n.type = NodeType::Group; n.w = 400; n.h = 200; deep.chain.push_back(n); const int made = static_cast<int>(deep.chain.size()) - 1;
            if (last >= 0 && CanJoin(deep, made, last)) deep.chain[static_cast<size_t>(made)].group = last; last = made; }
        Check(GroupDepth(deep, last - 1) == kGroupDepthMax - 1 && deep.chain[static_cast<size_t>(last)].group == -1, "frames sit at most kGroupDepthMax deep");
        const Config bad = Decode(L"E1|U:0,0,400,200,-1,0,0;1;5|U:0,0,400,200,-1,0,0|U:0,0,400,200,-1");
        Check(bad.chain.size() == 3 && bad.chain[1].group == 0 && bad.chain[0].group == -1 && bad.chain[0].w == 400, "a frame naming itself, a frame around it or a missing node is ignored; a short U: is skipped");
        Check(Decode(L"E1|U:0,0,10,99999,-1,0,").chain[0].w == kGroupMinW && Decode(L"E1|U:0,0,10,99999,-1,0,").chain[0].h == kGroupMaxSize, "frame sizes are clamped");
    }
    // 瞬移: the hero's own 瞬身 dodge landing in front of the target (P:distance,lift,x,y,next[,cut]).
    {
        Config tc = Linked({{NodeType::Skill, "GA_A"}});
        Node tp; tp.type = NodeType::Teleport; tp.dist = 250; tp.lift = 40; tp.x = 5; tp.y = 6;
        const int t = InsertAfter(tc, 0, tp);
        Check(IsMove(tc.chain[static_cast<size_t>(t)]) && TakesModifiers(tc.chain[static_cast<size_t>(t)]) && Follow(tc, 0, K, key, 100).move == t,
              "a 瞬移 is a move of the chain that takes modifiers");
        const std::wstring ttext = Encode(tc);
        Check(ttext.find(L"|P:250,40,5,6,-1") != std::wstring::npos && Decode(ttext).chain[static_cast<size_t>(t)].dist == 250 && Encode(Decode(ttext)) == ttext,
              "saved as P:distance,lift,x,y,next and read back");
        const Config clamp = Decode(L"E1|P:5,999,0,0,-1|P:1,2,3");
        Check(clamp.chain.size() == 2 && clamp.chain[0].dist == kTeleportDistMin && clamp.chain[0].lift == kTeleportLiftMax, "distance and lift are clamped; a short P: is skipped");
        Check(TeleportDistances(150, 80) == std::vector<int>{150, 125, 100, 80} && TeleportDistances(50, 80) == std::vector<int>{80},
              "a blocked landing tries shorter distances, never closer than the capsules allow");
        // 自定义组合键.
        {
        const int kL = static_cast<int>(Trigger::Light), kH = static_cast<int>(Trigger::Heavy), kD = static_cast<int>(Trigger::AnyDodge);
        const unsigned lbit = 1u << kL, hbit = 1u << kH, dbit = 1u << static_cast<int>(Trigger::Dash);
        const std::vector<int> lh{kL, kH}, llh{kL, kL, kH}, ld{kL, kD};
        Check(SequenceDone({{lbit, 100}, {hbit, 400}}, lh, 400) && !SequenceDone({{lbit, 100}, {hbit, 600}}, lh, 400) &&
              !SequenceDone({{hbit, 100}, {lbit, 200}}, lh, 400), "in order: the keys one after another, each within the window");
        Check(SequenceDone({{hbit, 0}, {lbit, 100}, {lbit, 300}, {hbit, 500}}, llh, 300) && !SequenceDone({{lbit, 100}, {hbit, 300}}, llh, 300) &&
              !SequenceDone({{lbit, 100}, {lbit, 200}, {dbit, 250}, {hbit, 300}}, llh, 300), "in order: the newest presses only, nothing else in between");
        Check(ChordDone({{lbit, 100}, {hbit, 200}}, lh, kChordMs) && ChordDone({{lbit | hbit, 100}}, lh, kChordMs) &&
              ChordDone({{hbit, 100}, {lbit, 150}}, lh, kChordMs) && !ChordDone({{lbit, 100}, {hbit, 400}}, lh, kChordMs) &&
              !ChordDone({{lbit, 100}, {hbit, 150}, {dbit, 160}}, lh, kChordMs) && ChordDone({{dbit, 100}, {lbit, 120}}, ld, kChordMs),
              "together: every key within 150 ms, the newest press one of them (dodge = any dodge)");
        Check(!SequenceDone({{lbit, 100}}, std::vector<int>{kL}, 400) && !ValidComboKey(static_cast<int>(Trigger::Execution)) &&
              ValidRouteButton(static_cast<int>(Trigger::Combo)) && !ValidRouteButton(static_cast<int>(Trigger::Execution)),
              "one key is no combo; 处决 is no key nor a branch row; a branch row may be a 组合键 of its own");
        Config ck; Node combo = TriggerNode(Trigger::Combo, 5, 6); combo.keys = {kL, kH, kD}; combo.chord = false; combo.window = 650; combo.next = 1; ck.chain.push_back(combo);
        Node hit; hit.ability = "GA_X_Skill_01"; ck.chain.push_back(hit);
        const Config kd = Decode(Encode(ck));
        Check(kd.chain.size() == 3 && kd.chain[0].keys == combo.keys && !kd.chain[0].chord && kd.chain[0].window == 650 && kd.chain[0].next == 1 &&
              ComboKeysText(kd.chain[0]) == L"轻攻击 → 重攻击 → 闪避", "a 组合键 trigger saves its keys, mode and window (Decode adds the NPC key node)");
        Press kp; kp.bits = 1u << static_cast<int>(Trigger::Combo); kp.combo = 0;
        Press other = kp; other.combo = 5;
        Check(TriggerFor(kd, kp) == 0 && TriggerFor(kd, other) < 0 && Follow(kd, 0, static_cast<int>(Trigger::Combo), kp, -1).move == 1,
              "its own keys start it (another combo's do not), and pressing them again goes on");
        Press plain; plain.bits = lbit;
        Check(TriggerFor(kd, plain) < 0, "a plain press never starts a 组合键 trigger");
        // 自定义按键: raw keys (keyboard / pad codes), one key, in order, together; 吞掉 saved.
        const int kQ = 'Q', kE = 'E', kLB = kPadKey + 8, kA = kPadKey + 10;
        Check(ValidRawKey(kQ) && ValidRawKey(0x05) && ValidRawKey(kPadKey + 15) && !ValidRawKey(1) && !ValidRawKey(2) && !ValidRawKey(0xFF) &&
              ValidRawKey(kStickKey) && ValidRawKey(kStickKey + 3) && !ValidRawKey(kStickKey + 4) && ValidRouteButton(static_cast<int>(Trigger::Keys)),
              "raw keys: keyboard / mouse side / 16 pad buttons / 4 left-stick ways, never the left or right mouse button; a branch row may be 自定义按键");
        // Left-stick ways (上推 / 下推 ...): pad codes after the buttons, read from the stick bits after the button bits.
        const int kUpStick = kStickKey, kDownStick = kStickKey + 1, kSquare = kPadKey + 12;
        Check(IsPadKey(kUpStick) && IsStickKey(kDownStick) && !IsStickKey(kSquare) && PadCodeBits(1u << 12, 2u) == ((1u << 12) | (1u << 17)) &&
              ((PadCodeBits(0, 1u) >> (kUpStick - kPadKey)) & 1u) && !((PadCodeBits(0xFFFFu, 0) >> (kUpStick - kPadKey)) & 1u) &&
              RawKeyToken(kDownStick) == "pad:LS-Down" && std::wstring(PadKeyName(kUpStick, true)) == L"LS-Up",
              "stick ways: their own bits after the 16 buttons, named LS-Up / LS-Down / LS-Left / LS-Right");
        Check(RawKeysDone({{kUpStick, 100}, {kDownStick, 250}, {kSquare, 380}}, {kUpStick, kDownStick, kSquare}, false, 300, kChordMs) &&
              !RawKeysDone({{kUpStick, 100}, {kSquare, 380}}, {kUpStick, kDownStick, kSquare}, false, 300, kChordMs) &&
              !RawKeysDone({{kDownStick, 100}, {kUpStick, 250}, {kSquare, 380}}, {kUpStick, kDownStick, kSquare}, false, 300, kChordMs),
              "上推 -> 下推 -> 方块 in order (a motion input): only that order, each within the window");
        {
            Config sc; Node motion = TriggerNode(Trigger::Keys, 1, 2); motion.keys = {kUpStick, kDownStick, kSquare}; motion.window = 300; motion.next = -1; sc.chain.push_back(motion);
            const std::wstring stext = Encode(sc); const Config sback = Decode(stext);
            Check(!sback.chain.empty() && sback.chain[0].keys == motion.keys && stext.find(L",272;273;268/0/300") != std::wstring::npos &&
                  Encode(Decode(Encode(sback))) == Encode(sback), "a motion input's stick ways are saved (272, 273) and read back");
        }
        Check(RawKeysDone({{kQ, 100}}, {kQ}, false, 400, kChordMs) && !RawKeysDone({{kE, 100}}, {kQ}, false, 400, kChordMs), "one key: its press");
        Check(RawKeysDone({{kLB, 100}, {kA, 450}}, {kLB, kA}, false, 400, kChordMs) && !RawKeysDone({{kLB, 100}, {kA, 600}}, {kLB, kA}, false, 400, kChordMs) &&
              !RawKeysDone({{kA, 100}, {kLB, 200}}, {kLB, kA}, false, 400, kChordMs) && RawKeysDone({{kQ, 50}, {kLB, 100}, {kLB, 300}, {kA, 500}}, {kLB, kLB, kA}, false, 300, kChordMs),
              "in order: the newest presses, each within the window of the one before");
        Check(RawKeysDone({{kA, 100}, {kLB, 140}}, {kLB, kA}, true, 400, kChordMs) && !RawKeysDone({{kA, 100}, {kLB, 300}}, {kLB, kA}, true, 400, kChordMs) &&
              !RawKeysDone({{kA, 100}, {kLB, 120}, {kQ, 130}}, {kLB, kA}, true, 400, kChordMs), "together: every key within 150 ms, the newest press one of them");
        Config rc; Node raw = TriggerNode(Trigger::Keys, 1, 2); raw.keys = {kQ, kLB}; raw.chord = true; raw.window = 700; raw.swallow = true; raw.next = 1; rc.chain.push_back(raw);
        rc.chain.push_back(hit);
        const Config rd = Decode(Encode(rc));
        Check(rd.chain[0].trigger == static_cast<int>(Trigger::Keys) && rd.chain[0].keys == raw.keys && rd.chain[0].chord && rd.chain[0].window == 700 && rd.chain[0].swallow &&
              Encode(rc).find(L",81;264/1/700/1") != std::wstring::npos && RawKeysToken(rd.chain[0]) == "vk:0x51 + pad:LB", "a 自定义按键 trigger saves its keys, mode, window and 吞掉");
        Press rp; rp.bits = 1u << static_cast<int>(Trigger::Keys); rp.combo = 0;
        Press rother = rp; rother.combo = 3;
        Check(TriggerFor(rd, rp) == 0 && TriggerFor(rd, rother) < 0 && TriggerFor(rd, plain) < 0, "its own keys start it; another trigger's keys or a plain press never do");
        // 继续派生: a 自定义组合键 / 自定义按键 chain's moves after its first on another key.
        {
            Config thc; Node thcombo = TriggerNode(Trigger::Combo, 0, 0); thcombo.keys = {static_cast<int>(Trigger::Light), static_cast<int>(Trigger::Heavy)}; thcombo.next = 1;
            thcombo.then = static_cast<int>(Trigger::Light); thc.chain.push_back(thcombo);
            Node ma; ma.ability = "GA_Test_Attack_01"; ma.next = 2; thc.chain.push_back(ma);
            Node mb; mb.ability = "GA_Test_Attack_02"; thc.chain.push_back(mb);
            Press own; own.bits = 1u << static_cast<int>(Trigger::Combo); own.combo = 0;
            Press thlight; thlight.bits = 1u << static_cast<int>(Trigger::Light);
            const Hop first = Follow(thc, 0, StartButton(thc, 0, own), own, -1);
            Check(TriggerFor(thc, own) == 0 && first.move == 1 && first.button == static_cast<int>(Trigger::Light) && TriggerFor(thc, thlight) < 0,
                  "继续派生 on a hero button: the thcombo plays the first move, then the chain is on that button");
            Check(Follow(thc, 1, first.button, thlight, 300).move == 2 && Follow(thc, 1, first.button, own, 300).move < 0,
                  "that button takes the next move; the thcombo again does not (it starts the chain over)");
            Config oc = thc; oc.chain[0].then = kThenOwn;
            const Hop ofirst = Follow(oc, 0, StartButton(oc, 0, own), own, -1);
            Check(ofirst.button == static_cast<int>(Trigger::Combo) && Follow(oc, 1, ofirst.button, own, 300).move == 2 && Follow(oc, 1, ofirst.button, thlight, 300).move < 0,
                  "default: the thcombo again takes the chain on, its button alone does not");
            const std::wstring ctext = Encode(thc), otext = Encode(oc);
            Check(ctext.find(L",1;2/0/400/1|") != std::wstring::npos && otext.find(L",1;2/0/400|") != std::wstring::npos && Decode(ctext).chain.size() == 4 &&
                  Decode(ctext).chain[0].then == static_cast<int>(Trigger::Light) && Encode(Decode(ctext)).find(L",1;2/0/400/1|") != std::wstring::npos &&
                  Encode(Decode(Encode(Decode(ctext)))) == Encode(Decode(ctext)) && Decode(otext).chain[0].then == kThenOwn,
                  "a 组合键's 继续派生 is saved after its keys' mode (/<button>) and read back; the default writes nothing");
            Config thkc = thc; thkc.chain[0] = TriggerNode(Trigger::Keys, 0, 0); thkc.chain[0].keys = {'Q'}; thkc.chain[0].next = 1; thkc.chain[0].then = kThenRaw + 'E';
            Press kown; kown.bits = 1u << static_cast<int>(Trigger::Keys); kown.combo = 0;
            Press then; then.then = true;
            const Hop kfirst = Follow(thkc, 0, StartButton(thkc, 0, kown), kown, -1);
            Check(kfirst.move == 1 && kfirst.button == kThenKeyButton && ThenRawKey(thkc.chain[0]) == 'E' && Follow(thkc, 1, kfirst.button, then, 300).move == 2 &&
                  Follow(thkc, 1, kfirst.button, kown, 300).move < 0 && Follow(thkc, 1, kfirst.button, thlight, 300).move < 0 && TriggerFor(thkc, then) < 0 &&
                  !Matches(static_cast<int>(Trigger::Light), then) && !Matches(static_cast<int>(Trigger::NpcKey), then),
                  "继续派生 on a key of its own: only that key's press takes the next move, and it starts nothing");
            const std::wstring thktext = Encode(thkc);
            Check(thktext.find(L",81/0/400/0/4165|") != std::wstring::npos && Decode(thktext).chain[0].then == kThenRaw + 'E' &&
                  Encode(Decode(thktext)).find(L",81/0/400/0/4165|") != std::wstring::npos && Encode(Decode(Encode(Decode(thktext)))) == Encode(Decode(thktext)),
                  "a 自定义按键's 继续派生 key is saved after 吞掉 (/4096+code) and read back");
            std::wstring bad = ctext; bad.replace(bad.find(L",1;2/0/400/1|"), 13, L",1;2/0/400/4165|");
            Check(Decode(bad).chain[0].then == kThenOwn && !ValidThen(static_cast<int>(Trigger::Combo), kThenRaw + 'E') &&
                  !ValidThen(static_cast<int>(Trigger::Combo), static_cast<int>(Trigger::Keys)) && !ValidThen(static_cast<int>(Trigger::Keys), static_cast<int>(Trigger::Execution)) &&
                  !ValidThen(static_cast<int>(Trigger::Light), static_cast<int>(Trigger::Heavy)) && ValidThen(static_cast<int>(Trigger::Keys), static_cast<int>(Trigger::Light)) &&
                  ValidThen(static_cast<int>(Trigger::Combo), static_cast<int>(Trigger::NpcKey)) && ThenButton(oc.chain[0]) == static_cast<int>(Trigger::Combo),
                  "a key of the other kind (a 组合键 takes no keyboard key) or an execution reads as the default; other triggers have none");
            Config bc = thc; bc.chain[1].next = 3; Node branch = BranchNode(0, 0); branch.routes[0].button = static_cast<int>(Trigger::Heavy); branch.routes[0].next = 2; bc.chain.push_back(branch);
            Press thheavy; thheavy.bits = 1u << static_cast<int>(Trigger::Heavy);
            Check(Follow(bc, 1, first.button, thheavy, 300).move == 2 && Follow(bc, 1, first.button, thlight, 300).move < 0, "a branch after it goes by its rows as before");
        }
        // 加速 held on a montage.
        Check(std::fabs(HeldRate(1.0, 10.0, 10.0) - 10.0) < 1e-9 && HeldRate(10.0, 10.0, 10.0) == 0 && HeldRate(0, 10.0, 10.0) == 0 &&
              std::fabs(HeldRate(0.05, 10.0, 10.0) - 0.5) < 1e-9 && HeldRate(1.0, 1.0, 1.0) == 0,
              "加速 held: the game's own rate (reset, hit stop) is scaled again; ours or a paused montage is left alone");
        }
        const Landing turned = LandingAt(0, 0, 1, 0, -500, 0, 100, 90);
        Check(std::fabs(turned.x) < 1e-6 && std::fabs(turned.y - 100) < 1e-6 && std::fabs(turned.yaw + 90) < 1e-6,
              "a landing swung 90 degrees sits beside the target, facing it");
        const Landing front = LandingAt(0, 0, 1, 0, -500, 0, 150), side = LandingAt(0, 0, 0, 0, 0, 300, 100);
        Check(std::fabs(front.x - 150) < 1e-6 && std::fabs(front.y) < 1e-6 && std::fabs(std::fabs(front.yaw) - 180) < 1e-6 &&
              std::fabs(side.y - 100) < 1e-6 && std::fabs(side.yaw + 90) < 1e-6, "the landing is in front of the target, facing it (no facing: from the hero's side)");
    }
    // Grab moves and NPC skins name one boss differently; GrabBoss::Key folds them.
    {
        using GrabBoss::Same;
        Check(Same(std::string("GuiChai_Red"), std::string("GuiChaiRedP2")) && Same(std::string("YuDing_00"), std::string("YuDing")) &&
              Same(std::string("Luanshi"), std::string("LuanShi")) && Same(std::string("MBossVariants"), std::string("MBossP2")) &&
              Same(std::wstring(L"MoTian"), std::string("MoTianMask")) && Same(std::wstring(L"MoTian"), std::string("MoTianYoung")) &&
              Same(std::string("ZuoShang"), std::string("ZuoShangLongTi")) && Same(std::string("HuaShan"), std::string("HuaShanJianOu")) &&
              Same(std::string("DaTouWaWa"), std::string("WuWuWu")) && Same(std::string("GuiChai_Yellow"), std::string("GuiChaiGouHun")) &&
              Same(std::string("GreatBladeMan"), std::string("HuoDaoDuiZhang")) && Same(std::string("MengHua"), std::string("MengHuaP2")),
              "grab boss ids meet their skins (case, _, phase suffix, digits, looks of one boss)");
        Check(!Same(std::string("TieGui"), std::string("LuanShi")) && !Same(std::string("WuXuan"), std::string("ZhuoBi")) &&
              !Same(std::string("GuiChai_Red"), std::string("GuiChaiGouHun")) && !Same(std::string(""), std::string("")) &&
              !Same(std::string("MoTian"), std::string("MuXiaoKui")), "different bosses (even in one folder) stay apart; empty names never match");
    }

    // Older texts.
    const Config list = Decode(L"E1|S:GA bad name|I:-5|R:9:GA_X:1|S:GA_Ok|R:1:GA_Y|R:2:GA_Z:1|R:5::0");
    const int lk = KeyTrigger(list);
    Check(list.enabled && !list.placed && lk == 2 && list.chain[0].ms == kIntervalMin && list.chain[1].ability == "GA_Ok" && list.chain[static_cast<size_t>(lk)].next == 0 &&
          list.chain[0].next == 1, "the list before the canvas: a wired row after the key node, bad names dropped, intervals clamped");
    Check(list.chain.size() == 7 && list.chain[3].trigger == static_cast<int>(Trigger::PerfectDodge) && list.chain[3].next == 4 && list.chain[4].ability == "GA_Z" &&
          list.chain[5].trigger == static_cast<int>(Trigger::AnyDodge) && !list.chain[5].on && list.chain[6].type == NodeType::AutoDodge,
          "old replace nodes become a dodge trigger wired to the move (or to the stick's dodge)");
    const Config canvas1 = Decode(L"E1|B:1,10,20|S:GA_A,200,40,-1|S:GA_B,0,0,0|R:0:GA_Q:1:30:400");
    const int ck = KeyTrigger(canvas1);
    Check(canvas1.placed && ck == 2 && canvas1.chain[static_cast<size_t>(ck)].next == 1 && canvas1.chain[static_cast<size_t>(ck)].x == 10 && canvas1.chain[1].next == 0 &&
          canvas1.chain[3].trigger == static_cast<int>(Trigger::Dash) && canvas1.chain[3].x == 30 && canvas1.chain[4].x == 330, "the first canvas: B: is the key node, R: a trigger");
    Check(Decode(L"").enabled && Decode(L"").chain.size() == 1 && KeyTrigger(Decode(L"")) == 0, "nothing saved: on, just the key node");
    std::wstring many = L"E1"; for (int i = 0; i < 80; ++i) many += L"|S:GA_A,0,0,-1";
    Check(Decode(many).chain.size() == kMaxNodes + 1, "at most kMaxNodes nodes are read, then the key node");
    {
        // 变身 / 变回主角: moves a press reaches (the same button again on a straight wire), kept by the text, no
        // modifiers, no 派生点; a 变身 with no boss picked passes the press on; both may sit in a 组合.
        Config looks;  // 0 light trigger -> 1 A -> 2 变身 MoTian -> 3 B -> 4 变回主角; 5 a 变身 with no boss -> 6 C
        looks.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
        looks.chain.push_back({NodeType::Skill, "GA_A"});
        Node morph; morph.type = NodeType::Look; morph.boss = "MoTian"; looks.chain.push_back(morph);
        looks.chain.push_back({NodeType::Skill, "GA_B"});
        Node hero_back; hero_back.type = NodeType::HeroLook; looks.chain.push_back(hero_back);
        Node unset; unset.type = NodeType::Look; looks.chain.push_back(unset);
        looks.chain.push_back({NodeType::Skill, "GA_C"});
        looks.chain[0].next = 1; looks.chain[1].next = 2; looks.chain[2].next = 3; looks.chain[3].next = 4; looks.chain[5].next = 6;
        EnsureKey(looks);
        Check(IsMove(looks.chain[2]) && IsMove(looks.chain[4]) && !IsMove(looks.chain[5]) && EmptyMove(looks.chain[5]) && IsLookChange(looks.chain[2]) && IsLookChange(looks.chain[5]),
              "a 变身 with a boss and a 变回主角 are moves; a 变身 with none is an empty move");
        Check(Follow(looks, 1, L, light, 100).move == 2 && Follow(looks, 2, L, light, 100).move == 3 && Follow(looks, 3, L, light, 100).move == 4 && Follow(looks, 4, L, light, 100).move == -1,
              "a press reaches the 变身, goes on from it, reaches the 变回主角 (the end of the chain)");
        looks.chain[1].next = 5;
        Check(Follow(looks, 1, L, light, 100).move == 6, "a 变身 with no boss passes the press on to the move after it");
        looks.chain[1].next = 2;
        const int look_speed = static_cast<int>(looks.chain.size()); looks.chain.push_back(ModifierNode(NodeType::Speed, 0, 0));
        Check(!TakesModifiers(looks.chain[2]) && !TakesModifiers(looks.chain[4]) && !LinkModifier(looks, 2, look_speed) && LinkModifier(looks, 0, look_speed) && LinkModifier(looks, 1, look_speed),
              "no modifier arrows from a 变身 / 变回主角 (the trigger and moves still take them)");
        looks.chain[2].ms = 400;
        Check(CancelOf(looks, 2) == 0 && CancelOf(looks, 4) == 0 && CutMs(looks, 2) == 0, "a 变身 has no 派生点 (it is over once the look is on)");
        const Config round = Decode(Encode(looks));
        Check(Encode(looks).find(L"|M:MoTian,") != std::wstring::npos && Encode(looks).find(L"|F:") != std::wstring::npos && round.chain.size() == looks.chain.size() &&
              round.chain[2].type == NodeType::Look && round.chain[2].boss == "MoTian" && round.chain[2].next == 3 && round.chain[4].type == NodeType::HeroLook &&
              round.chain[5].type == NodeType::Look && round.chain[5].boss.empty() && round.chain[5].next == 6 && Encode(round) == Encode(looks),
              "the text keeps 变身 (|M:boss,x,y,next; _ = none) and 变回主角 (|F:x,y,next)");
        Check(Groupable(looks.chain[2]) && Groupable(looks.chain[4]), "both may sit in a 组合 (变身 then a move: one press)");
        // 保留变身: a modifier only a trigger, a BOSS 技能 or a frame points at; the BOSS 技能's look is kept.
        Node boss_move; boss_move.type = NodeType::BossSkill; boss_move.boss = "MoTian"; boss_move.ability = "GA_X";
        const int bm = static_cast<int>(looks.chain.size()); looks.chain.push_back(boss_move);
        const int keep = static_cast<int>(looks.chain.size()); looks.chain.push_back(ModifierNode(NodeType::KeepLook, 0, 0));
        const auto node_at = [&](const Config& c, int i) -> const Node& { return c.chain[static_cast<size_t>(i)]; };
        Check(IsModifier(node_at(looks, keep)) && SlotOf(NodeType::KeepLook) == kModKeepLook && node_at(looks, bm).mods[kModKeepLook] == -1 && !KeepLookFor(looks, 0, bm),
              "a 保留变身 nothing points at keeps nothing (a new node's slot is empty)");
        Check(!LinkModifier(looks, 1, keep) && !LinkModifier(looks, 2, keep) && !LinkModifier(looks, 4, keep) && LinkModifier(looks, bm, keep) &&
              KeepLookFor(looks, 0, bm) && !KeepLookFor(looks, 0, 1),
              "only a BOSS 技能 (or a trigger / frame) points at 保留变身; a plain move or a look change cannot");
        const Config kept = Decode(Encode(looks));
        Check(Encode(looks).find(L"|E:") != std::wstring::npos && node_at(kept, keep).type == NodeType::KeepLook && KeepLookFor(kept, 0, bm) && Encode(kept) == Encode(looks),
              "保留变身 saved as E and its arrow reads back");
        UnlinkModifiers(looks, bm);
        Check(!KeepLookFor(looks, 0, bm) && LinkModifier(looks, 0, keep) && KeepLookFor(looks, 0, bm) && !KeepLookFor(looks, 0, 1) && !KeepLookFor(looks, 0, 2),
              "the trigger at 保留变身: every BOSS 技能 of its chain keeps its look (only BOSS 技能 take one)");
    }

    {
        // 强制受击: fa modifier like 霸体 (fa trigger or fa move points at it); its kind picks the game's hit strength.
        Config fh; fh.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
        Node fa; fa.ability = "GA_X_Skill_01"; fh.chain.push_back(fa); fh.chain[0].next = 1;
        Node fb; fb.ability = "GA_X_Skill_02"; fh.chain.push_back(fb); fh.chain[1].next = 2;
        Node fgrab; fgrab.type = NodeType::Grab; fgrab.ability = "MoTian"; fh.chain.push_back(fgrab); fh.chain[2].next = 3;
        const int fmod = static_cast<int>(fh.chain.size()); fh.chain.push_back(ModifierNode(NodeType::ForceHit, 40, 50));
        const int fmod2 = static_cast<int>(fh.chain.size()); fh.chain.push_back(ModifierNode(NodeType::ForceHit, 40, 150)); fh.chain.back().hit = static_cast<int>(HitKind::Break);
        EnsureKey(fh);  // the NPC key node Decode adds
        Check(IsModifier(fh.chain[static_cast<size_t>(fmod)]) && SlotOf(NodeType::ForceHit) == kModForceHit && kModForceHit + 1 == kModSlots && fh.chain[1].mods[kModForceHit] == -1 &&
              ForceHitFor(fh, 0, 1) < 0 && fh.chain[static_cast<size_t>(fmod)].hit == static_cast<int>(HitKind::Launch),
              "a new 强制受击 is 击飞 and forces nothing until something points at it");
        Check(LinkModifier(fh, 0, fmod) && LinkModifier(fh, 2, fmod2) && !LinkModifier(fh, 3, fmod) && ForceHitFor(fh, 0, 1) == static_cast<int>(HitKind::Launch) &&
              ForceHitFor(fh, 0, 2) == static_cast<int>(HitKind::Break) && ForceHitFor(fh, 0, 3) < 0,
              "the trigger's for every move, fa move's own first; fa 投技 neither points at one nor takes the trigger's");
        Check(std::wstring(HitKindLabel(static_cast<int>(HitKind::Launch))) == L"击飞" && std::wstring(HitKindLabel(static_cast<int>(HitKind::Break))) == L"破防" &&
              std::string(HitKindTag(static_cast<int>(HitKind::Launch))) == "Hitreact.Strength.Income.Launchback.Heavy" &&
              std::string(HitKindTag(static_cast<int>(HitKind::Knockdown))) == "Hitreact.Strength.Income.Knockdown" &&
              std::string(HitKindTag(static_cast<int>(HitKind::Break))) == "Hitreact.Strength.Income.Break.Start" && ClampHitKind(99) == 0 && ClampHitKind(-1) == 0 &&
              std::string(HitKindTag(99)) == std::string(HitKindTag(0)),
              "kinds: labels, the game's Hitreact.Strength.Income tags, an unknown kind is 击飞");
        const std::wstring ftext = Encode(fh);
        const Config fback = Decode(ftext);
        Check(ftext.find(L"|Q:0,40,50") != std::wstring::npos && ftext.find(L"|Q:4,40,150") != std::wstring::npos && fback.chain.size() == fh.chain.size() &&
              fback.chain[static_cast<size_t>(fmod2)].type == NodeType::ForceHit && fback.chain[static_cast<size_t>(fmod2)].hit == static_cast<int>(HitKind::Break) &&
              ForceHitFor(fback, 0, 2) == static_cast<int>(HitKind::Break) && ForceHitFor(fback, 0, 1) == static_cast<int>(HitKind::Launch) && Encode(fback) == ftext,
              "强制受击 saved as Q:<kind>,x,y with its arrows");
        Check(Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,-1|Q:77,10,10|L:0,2").chain[2].hit == 0, "a bad kind in the ftext reads as 击飞");
    }
    {
        // A 按键分支 row of 自定义组合键 / 自定义按键: its own keys, saved with the row; only its own press takes it.
        const int kC = static_cast<int>(Trigger::Combo), kR = static_cast<int>(Trigger::Keys), kLt = static_cast<int>(Trigger::Light), kHv = static_cast<int>(Trigger::Heavy);
        Config kbr; kbr.chain.push_back(TriggerNode(Trigger::Light, 0, 0));
        Node kfirst; kfirst.ability = "GA_X_Skill_01"; kbr.chain.push_back(kfirst); kbr.chain[0].next = 1;
        Node kbranch = BranchNode(300, 0);
        kbranch.routes[0].button = kC; kbranch.routes[0].keys = {kLt, kHv}; kbranch.routes[0].window = 300; kbranch.routes[0].next = 3;
        Route kraw; kraw.button = kR; kraw.keys = {'Q', kPadKey + 8}; kraw.chord = true; kraw.swallow = true; kraw.next = 4; kbranch.routes.push_back(kraw);
        Route kplain; kplain.button = kHv; kplain.next = 5; kbranch.routes.push_back(kplain);
        Route kempty; kempty.button = kC; kempty.keys = {kLt}; kempty.next = 5; kbranch.routes.push_back(kempty);  // one key: not a combo yet
        kbr.chain.push_back(kbranch); kbr.chain[1].next = 2;
        for (const char* name : {"GA_X_Skill_02", "GA_X_Skill_03", "GA_X_Skill_04"}) { Node m; m.ability = name; kbr.chain.push_back(m); }
        Check(KeysRoute(kbr.chain[2].routes[0]) && KeysRouteReady(kbr.chain[2].routes[0]) && KeysRouteReady(kbr.chain[2].routes[1]) && !KeysRoute(kbr.chain[2].routes[2]) &&
              !KeysRouteReady(kbr.chain[2].routes[3]),
              "a 组合键 row needs 2 keys, a 自定义按键 row 1; a kplain row has none");
        Press row0; row0.bits = 1u << kC; row0.branch = 2; row0.row = 0;
        Press row1; row1.bits = 1u << kR; row1.branch = 2; row1.row = 1;
        Press stray; stray.bits = 1u << kC;  // a combo press of no row (a 组合键 trigger's)
        Press wrong = row0; wrong.branch = 7;
        Press short_row = row0; short_row.row = 3;
        const Press heavy_press{1u << kHv, false};
        const Hop h0 = Follow(kbr, 1, kLt, row0, 500), h1 = Follow(kbr, 1, kLt, row1, 500);
        Check(h0.move == 3 && h0.button == kC && h0.branch == 2 && h0.row == 0 && h1.move == 4 && h1.branch == 2 && h1.row == 1 &&
              Follow(kbr, 1, kLt, stray, 500).move < 0 && Follow(kbr, 1, kLt, wrong, 500).move < 0 && Follow(kbr, 1, kLt, short_row, 500).move < 0 &&
              Follow(kbr, 1, kLt, heavy_press, 500).move == 5 && Follow(kbr, 1, kLt, heavy_press, 500).row == 2,
              "only the row's own keys take it (no other combo, nor a row without enough keys); a kplain row by its button; the hop names its row");
        kbr.chain[3].next = 6; Node kagain; kagain.ability = "GA_X_Skill_05"; kbr.chain.push_back(kagain); EnsureKey(kbr);
        Check(Follow(kbr, 3, kC, row0, 500).move == 6 && Follow(kbr, 3, kC, heavy_press, 500).move < 0 && Follow(kbr, 3, kC, row0, 500).branch < 0,
              "after the row's move a straight wire takes the same keys kagain (as a button kagain)");
        const std::wstring rtext = Encode(kbr);
        const Config rback = Decode(rtext);
        const Node& rrb = rback.chain[2];
        Check(rtext.find(L",14/1;2/0/300,0,3") != std::wstring::npos && rtext.find(L",15/81;264/1/400/1,0,4") != std::wstring::npos && rrb.routes.size() == 4 &&
              rrb.routes[0].keys == std::vector<int>({kLt, kHv}) && !rrb.routes[0].chord && rrb.routes[0].window == 300 && rrb.routes[1].keys == std::vector<int>({'Q', kPadKey + 8}) &&
              rrb.routes[1].chord && rrb.routes[1].swallow && rrb.routes[2].button == kHv && rrb.routes[2].keys.empty() && rrb.routes[3].keys == std::vector<int>({kLt}) && Encode(rback) == rtext,
              "rows save their keys in the button field (<button>/<keys>/<together>/<window>[/<swallow>]) and read back");
        Check(Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|K:300,0,14,0,3,15/_/0/400/0,0,3|S:GA_X_Skill_02,600,0,-1").chain[2].routes.size() == 2 &&
              Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|K:300,0,14,0,3,15/_/0/400/0,0,3|S:GA_X_Skill_02,600,0,-1").chain[2].routes[0].keys.empty() &&
              Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|K:300,0,14/1;99;2/0/50,0,3|S:GA_X_Skill_02,600,0,-1").chain[2].routes[0].keys == std::vector<int>({kLt, kHv}) &&
              Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|K:300,0,14/1;99;2/0/50,0,3|S:GA_X_Skill_02,600,0,-1").chain[2].routes[0].window == kKeyWindowMin &&
              Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|K:300,0,13,0,3,1,0,3|S:GA_X_Skill_02,600,0,-1").chain[2].routes.size() == 1,
              "a keys row without keys reads with none; bad keys are dropped and the window clamped; 处决 is still no row (dropped)");
        Check(ComboKeysText(rrb.routes[0]) == L"轻攻击 → 重攻击" && RawKeysToken(rrb.routes[1]) == "vk:0x51 + pad:LB", "a row's keys as rtext (the page / the log)");
    }

    // 预输入: the chain's one fixed node (its clear time), no wires, never removed, saved as "J:ms,x,y" in place.
    {
        Config pre = Decode(L"E1|T:1,1200,1,0,0,1|S:GA_X_Skill_01,300,0,2|S:GA_X_Skill_02,600,0,-1");
        Check(PreInputNode(pre) < 0 && PreInputMs(pre) == kPreInputDefault && kPreInputDefault == 300, "an older chain has no 预输入 node: 0.3 s");
        const size_t before = pre.chain.size();
        EnsurePreInput(pre); EnsurePreInput(pre);
        const int pre_at = PreInputNode(pre);
        Check(pre_at == static_cast<int>(before) && pre.chain.size() == before + 1 && pre.chain[static_cast<size_t>(pre_at)].ms == kPreInputDefault &&
              pre.chain[static_cast<size_t>(pre_at)].y < 0 && pre.chain[static_cast<size_t>(pre_at)].x == 0, "EnsurePreInput adds one, above the other nodes, once");
        Check(OutCount(pre.chain[static_cast<size_t>(pre_at)]) == 0 && OutOf(pre.chain[static_cast<size_t>(pre_at)], 0) == -1 && !Groupable(pre.chain[static_cast<size_t>(pre_at)]) &&
              !IsMove(pre.chain[static_cast<size_t>(pre_at)]) && !IsModifier(pre.chain[static_cast<size_t>(pre_at)]) && !TakesModifiers(pre.chain[static_cast<size_t>(pre_at)]),
              "预输入 has no outputs, sits in no frame, plays nothing, takes no modifier");
        Link(pre, 2, pre_at); Link(pre, pre_at, 2);
        Check(pre.chain[2].next == -1 && pre.chain[static_cast<size_t>(pre_at)].next == -1, "no wire into or out of 预输入");
        Remove(pre, static_cast<size_t>(pre_at));
        Check(PreInputNode(pre) == pre_at && pre.chain.size() == before + 1, "预输入 is never removed");
        pre.chain[static_cast<size_t>(pre_at)].ms = 750;
        const int added = InsertAfter(pre, 2, {NodeType::Skill, "GA_X_Skill_03"});
        Check(added == static_cast<int>(pre.chain.size()) - 1 && pre.chain[2].next == added && PreInputNode(pre) == pre_at, "a node added later goes after it; the indices stay");
        Remove(pre, 1);
        Check(PreInputNode(pre) == pre_at - 1 && PreInputMs(pre) == 750, "removing a node before it shifts it like any node");
        const std::wstring pre_text = Encode(pre);
        const Config pre_back = Decode(pre_text);
        Check(pre_text.find(L"|J:750,0,") != std::wstring::npos && PreInputNode(pre_back) == pre_at - 1 && PreInputMs(pre_back) == 750 && Encode(pre_back) == pre_text, "saved in place and read back");
        Check(PreInputMs(Decode(L"E1|J:0,0,0")) == 0 && PreInputMs(Decode(L"E1|J:99999,0,0")) == kPreInputMax && PreInputMs(Decode(L"E1|J:-5,0,0")) == 0,
              "0 keeps a press until it plays; the time is clamped to 0..5 s");
        const Config two = Decode(L"E1|J:400,0,0|T:1,1200,1,0,0,2|J:900,0,0|S:GA_X_Skill_01,300,0,-1");
        Check(two.chain.size() == 4 && PreInputMs(two) == 400 && two.chain[1].next == 2 && two.chain[2].ability == "GA_X_Skill_01",
              "a second J is dropped and the wires point pre_at the same nodes");
        const Config wired = Decode(L"E1|T:1,1200,1,0,0,1|J:300,0,0|S:GA_X_Skill_01,300,0,-1");
        Check(wired.chain[0].next == -1, "a wire saved into 预输入 is dropped");
    }

    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
