// 武器派生修改 rows that name one weapon action (Route::ability): matching, priority, saving and the older-build reading.
#include "npc_combo_model.h"
#include "combo_weapons.h"
#include <cstdio>
#include <string>
int main() {
    using namespace NpcCombo;
    int checks = 0, failed = 0;
    const auto check = [&](bool v, const char* what) { ++checks; if (!v) { ++failed; std::printf("failed: %s\n", what); } };
    const int light = static_cast<int>(Trigger::Light);
    const unsigned light_bit = 1u << light;
    Config c;
    Node w = WeaponActionsNode(0, 0); w.weapon = "WoodenSword"; w.routes.clear();
    Route plain; plain.button = light; plain.next = 1;                                   // every light attack
    Route air; air.button = light; air.ability = "GA_Hun_Young_Air_A"; air.next = 2;     // that one air attack
    Route skill; skill.button = static_cast<int>(Trigger::Special); skill.ability = "GA_Hun_Young_C"; skill.next = 3;  // a tagless move
    w.routes = {plain, air, skill};
    Node move; move.ability = "GA_Test";
    c.chain = {w, move, move, move};

    Press p; p.weapon = w.weapon; p.bits = light_bit;
    check(WeaponActionsRow(w, p) == 0, "a light press without a name takes the button row");
    p.ability = "GA_Hun_Young_Air_A";
    check(WeaponActionsRow(w, p) == 1, "the action a row names comes before the button row");
    check(Follow(c, 0, light, p, 0).move == 2, "the named row's move plays");
    p.ability = "GA_Hun_Young_AA";
    check(WeaponActionsRow(w, p) == 0, "another light attack with a name nobody uses takes the button row");
    Press tagless; tagless.weapon = w.weapon; tagless.ability = "GA_Hun_Young_C";
    check(WeaponActionsRow(w, tagless) == 2 && Follow(c, 0, 0, tagless, 0).move == 3, "a move with no hero action tag is taken by name alone");
    tagless.weapon = "Tang";
    check(WeaponActionsRow(w, tagless) < 0, "only on the node's weapon");
    tagless.weapon = w.weapon;
    c.chain[0].routes[2].hold_ms = 900;
    check(WeaponActionsRow(c.chain[0], tagless) == 2, "a named row has no 按住");
    c.chain[0].routes[2].hold_ms = 0;
    c.chain[0].routes[1].next = -1;
    check(WeaponActionsRow(c.chain[0], p) == 0 && !NamesAbility(c, "GA_Hun_Young_Air_A"), "an unwired named row takes nothing and names nothing");
    c.chain[0].routes[1].next = 2;

    check(HasAbilityRows(c) && NamesAbility(c, "GA_Hun_Young_C") && !NamesAbility(c, "GA_Hun_Young_AA") && !NamesAbility(c, ""), "the chain says which actions its rows name");
    check(NamedAbilityButton(c, "GA_Hun_Young_Air_A") == light && NamedAbilityButton(c, "GA_Missing") < 0, "the named row's button is the input that brings the move");
    Config off = c; off.chain[0].on = false;
    check(!HasAbilityRows(off) && !NamesAbility(off, "GA_Hun_Young_C"), "a node switched off names nothing");
    Config airset = c; airset.chain[0].air_set = true; airset.chain[0].weapon.clear();
    check(!HasAbilityRows(airset) && !NamedRow(airset.chain[0], airset.chain[0].routes[1]), "a 空中派生's rows never name a move");

    const std::wstring text = Encode(c);
    check(text.find(L"|n:0,1,GA_Hun_Young_Air_A") != std::wstring::npos && text.find(L"|n:0,2,GA_Hun_Young_C") != std::wstring::npos && text.find(L"|n:0,0,") == std::wstring::npos,
          "each named row saved with an n: mark, the button row with none");
    const Config round = Decode(text);
    check(round.chain[0].routes.size() == 3 && round.chain[0].routes[0].ability.empty() && round.chain[0].routes[1].ability == "GA_Hun_Young_Air_A" &&
          round.chain[0].routes[2].ability == "GA_Hun_Young_C" && round.chain[0].routes[2].button == static_cast<int>(Trigger::Special) && Encode(Decode(Encode(round))) == Encode(round),
          "the names read back, with their buttons");
    std::wstring older = text;
    for (size_t at; (at = older.find(L"|n:")) != std::wstring::npos;) older.erase(at, older.find(L'|', at + 1) == std::wstring::npos ? std::wstring::npos : older.find(L'|', at + 1) - at);
    const Config old = Decode(older);
    check(old.chain[0].routes.size() == 3 && old.chain[0].routes[1].ability.empty() && old.chain[0].routes[1].button == light && old.chain[0].routes[1].next == 2,
          "an older build reads a named row as its button");
    check(Decode(L"E1|O:0,0,WoodenSword,1500,1,1|n:0,0,bad name|n:9,0,GA_X|n:0,7,GA_X").chain[0].routes[0].ability.empty(), "a bad name, node or row is dropped");
    Config held = c; held.chain[0].routes[1].hold_ms = 600;
    check(Encode(held).find(L"|w:0,1,") == std::wstring::npos && Decode(Encode(held)).chain[0].routes[1].hold_ms == 0, "no 按住 is saved for a named row");

    // NextInputs (派生显示) say which move a row names.
    const auto next = NextInputs(c, 0, 0);
    bool named_shown = false, plain_shown = false;
    for (const NextInput& in : next) { if (in.row == 1 && in.ability == "GA_Hun_Young_Air_A") named_shown = true; if (in.row == 0 && in.ability.empty()) plain_shown = true; }
    check(named_shown && plain_shown, "the next-press list names the move of a named row");

    // Every kind of a weapon's move maps to a button an older build can read.
    check(NamedRowButton(ComboWeapons::kLight) == light && NamedRowButton(ComboWeapons::kHeavy) == static_cast<int>(Trigger::Heavy) &&
          NamedRowButton(ComboWeapons::kExecution) == static_cast<int>(Trigger::Execution) && ValidWeaponAction(NamedRowButton(ComboWeapons::kSkill)), "kind -> button");
    if (failed) {  // details for a failing run
        std::printf("tagless row=%d follow=%d\n", WeaponActionsRow(w, tagless), Follow(c, 0, 0, tagless, 0).move);
        std::printf("routes=%zu", round.chain[0].routes.size());
        for (const Route& x : round.chain[0].routes) std::printf(" [%d %s %d]", x.button, x.ability.c_str(), x.next);
        std::printf(" same=%d\n%ls\n%ls\n", Encode(round) == text ? 1 : 0, text.c_str(), Encode(round).c_str());
    }
    std::printf("weapon_action_named checks=%d failures=%d\n", checks, failed);
    return failed ? 1 : 0;
}
