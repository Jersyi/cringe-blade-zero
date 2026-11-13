#pragma once
// 插件 UI 页 (plugin UI pages): what a plugin declares with api->ui_page (label / toggle / slider / choice / button / text),
// kept as plain data. The panel draws it with its own controls (no HWND leaves the host); this is the model both sides share.
// Any thread; one lock. Bound items ("bind": a module property or action, see mod_registry.h) are refreshed by the plugin
// host on the game thread (SetShown) and written back through it.
#include "pbz_combo_api.h"
#include <cstdint>
#include <string>
#include <vector>

namespace PluginUi {
constexpr size_t kMaxPages = 8, kMaxItems = 64;
struct Item {
    int kind = PBZ_UI_LABEL;
    std::string id, bind;                  // bind: "module.prop" (a property) or "module:action" (a button runs the action with `text`)
    std::wstring label, text;
    double min = 0, max = 1, step = 1, value = 0;
    std::vector<std::wstring> choices;     // CHOICE
    bool bound() const { return !bind.empty(); }
};
struct Page {
    std::string owner;                     // the plugin's id
    std::wstring owner_name;
    std::string id;
    std::wstring title;
    std::vector<Item> items;
};

// Checks the declaration (ids, kinds, ranges, binds against the module registry) and stores it, replacing the owner's page
// with the same id. PBZ_E_ARG / PBZ_E_FULL / PBZ_E_NOT_FOUND (a bind to a module property or action that is not registered).
PbzResult Declare(const std::string& owner, const std::wstring& owner_name, const PbzUiPage& page);
PbzResult Remove(const std::string& owner, const std::string& page);
void RemoveOwner(const std::string& owner);
PbzResult SetValue(const std::string& owner, const std::string& page, const std::string& item, double value);
PbzResult SetText(const std::string& owner, const std::string& page, const std::string& item, const std::wstring& text);

std::vector<Page> Snapshot();
bool FindItem(const std::string& owner, const std::string& page, const std::string& item, Item& out);
struct Binding { std::string owner, page, item, bind; int kind = 0; };
std::vector<Binding> Bindings();           // every bound item (the host refreshes them)
void SetShown(const std::string& owner, const std::string& page, const std::string& item, double value, const std::wstring& text, bool has_text);
uint64_t Serial();
void ResetForTest();
}
