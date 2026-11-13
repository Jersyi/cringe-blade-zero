#include "plugin_ui.h"
#include "mod_registry.h"
#include "plugin_text.h"
#include <atomic>
#include <cmath>
#include <mutex>

namespace PluginUi {
namespace {
using PluginText::IdText;
using PluginText::Short;
using PluginText::Wide;

std::mutex lock_;
std::vector<Page> pages_;
std::atomic<uint64_t> serial_{1};
void Bump() { serial_.fetch_add(1); }

double Clamp(double v, double lo, double hi) { return std::isfinite(v) ? (std::min)((std::max)(v, lo), hi) : lo; }
bool SplitBind(const std::string& bind, std::string& module, std::string& name, bool& action) {
    const size_t at = bind.find_first_of(".:");
    if (at == std::string::npos || at == 0 || at + 1 >= bind.size()) return false;
    action = bind[at] == ':'; module = bind.substr(0, at); name = bind.substr(at + 1);
    return true;
}
std::vector<std::wstring> Choices(const std::string& text) {
    std::vector<std::wstring> out; size_t at = 0;
    while (at <= text.size() && out.size() < 32) {
        const size_t comma = text.find(',', at);
        std::wstring part = Wide(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str(), 96);
        while (!part.empty() && part.front() == L' ') part.erase(part.begin());
        while (!part.empty() && part.back() == L' ') part.pop_back();
        if (!part.empty()) out.push_back(std::move(part));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

// One item as the plugin gave it, checked; PBZ_OK and `out` filled, or the error.
PbzResult MakeItem(const PbzUiItem& in, Item& out) {
    Item item;
    const std::string id = Short(in.id, 64);
    if (!IdText(id) || in.kind < PBZ_UI_LABEL || in.kind > PBZ_UI_TEXT) return PBZ_E_ARG;
    item.kind = in.kind; item.id = id; item.bind = Short(in.bind, 100);
    item.label = Wide(in.label, 96); item.text = Wide(in.text, in.kind == PBZ_UI_TEXT ? 1200 : 400);
    item.min = in.min; item.max = in.max; item.step = in.step; item.value = in.value;
    if (!std::isfinite(item.min) || !std::isfinite(item.max) || !std::isfinite(item.step) || !std::isfinite(item.value)) return PBZ_E_ARG;
    if (item.bound()) {
        std::string module, name; bool action = false;
        if (!SplitBind(item.bind, module, name, action)) return PBZ_E_ARG;
        if (item.kind == PBZ_UI_BUTTON) {
            if (!action) return PBZ_E_ARG;
            const PbzResult r = ModRegistry::FindAction(module, name);
            if (r != PBZ_OK) return r;
        } else {
            if (action) return PBZ_E_ARG;
            ModRegistry::PropInfo prop;
            const PbzResult r = ModRegistry::FindProp(module, name, prop);
            if (r != PBZ_OK) return r;
            using ModRegistry::Type;
            const bool numeric = prop.type == Type::Int || prop.type == Type::Float;
            if ((item.kind == PBZ_UI_TOGGLE && prop.type != Type::Bool) || (item.kind == PBZ_UI_SLIDER && !numeric) || (item.kind == PBZ_UI_CHOICE && prop.type != Type::Enum)) return PBZ_E_ARG;
            if (item.kind == PBZ_UI_SLIDER && (item.min >= item.max)) {  // the property's own range
                item.min = Clamp(prop.min, -1e9, 1e9); item.max = Clamp(prop.max, -1e9, 1e9);
                item.step = prop.type == ModRegistry::Type::Int ? 1 : (item.max - item.min) / 100;
            }
            if (item.kind == PBZ_UI_CHOICE) {
                item.choices.clear();
                for (const std::string& o : prop.options) item.choices.push_back(Wide(o.c_str(), 96));
            }
            if (prop.read_only && item.kind != PBZ_UI_LABEL && item.kind != PBZ_UI_TEXT) return PBZ_E_ARG;  // a control that cannot write
        }
    }
    switch (item.kind) {
    case PBZ_UI_TOGGLE: item.value = item.value != 0 ? 1 : 0; item.min = 0; item.max = 1; item.step = 1; break;
    case PBZ_UI_SLIDER:
        if (item.min >= item.max) return PBZ_E_ARG;
        if (item.step <= 0) item.step = (item.max - item.min) / 100;
        item.value = Clamp(item.value, item.min, item.max); break;
    case PBZ_UI_CHOICE:
        if (!item.bound()) item.choices = Choices(Short(in.text, 400));
        if (item.choices.empty()) return PBZ_E_ARG;
        item.min = 0; item.max = static_cast<double>(item.choices.size()) - 1; item.step = 1; item.value = std::floor(Clamp(item.value, item.min, item.max)); break;
    case PBZ_UI_BUTTON: if (item.label.empty()) return PBZ_E_ARG; break;
    default: break;
    }
    out = std::move(item);
    return PBZ_OK;
}
Item* FindItemIn(Page& page, const std::string& item) {
    for (Item& i : page.items) if (i.id == item) return &i;
    return nullptr;
}
Page* FindPage(const std::string& owner, const std::string& id) {
    for (Page& p : pages_) if (p.owner == owner && p.id == id) return &p;
    return nullptr;
}
}  // namespace

PbzResult Declare(const std::string& owner, const std::wstring& owner_name, const PbzUiPage& in) {
    const std::string id = Short(in.id, 64);
    if (!IdText(id) || !in.items || in.item_count < 1 || static_cast<size_t>(in.item_count) > kMaxItems) return PBZ_E_ARG;
    Page page; page.owner = owner; page.owner_name = owner_name; page.id = id; page.title = Wide(in.title, 96);
    if (page.title.empty()) page.title = Wide(id.c_str());
    for (int32_t k = 0; k < in.item_count; ++k) {
        PbzUiItem raw{};
        if (!in.items[k] || !PluginText::ReadIn(in.items[k], raw)) return PBZ_E_ARG;
        Item item; const PbzResult r = MakeItem(raw, item);
        if (r != PBZ_OK) return r;
        for (const Item& other : page.items) if (other.id == item.id) return PBZ_E_ARG;
        page.items.push_back(std::move(item));
    }
    std::lock_guard<std::mutex> hold(lock_);
    if (Page* existing = FindPage(owner, id)) *existing = std::move(page);
    else {
        size_t own = 0; for (const Page& p : pages_) if (p.owner == owner) ++own;
        if (own >= kMaxPages) return PBZ_E_FULL;
        pages_.push_back(std::move(page));
    }
    Bump(); return PBZ_OK;
}
PbzResult Remove(const std::string& owner, const std::string& page) {
    std::lock_guard<std::mutex> hold(lock_);
    for (auto it = pages_.begin(); it != pages_.end(); ++it) if (it->owner == owner && it->id == page) { pages_.erase(it); Bump(); return PBZ_OK; }
    return PBZ_E_NOT_FOUND;
}
void RemoveOwner(const std::string& owner) {
    std::lock_guard<std::mutex> hold(lock_);
    const size_t before = pages_.size();
    pages_.erase(std::remove_if(pages_.begin(), pages_.end(), [&](const Page& p) { return p.owner == owner; }), pages_.end());
    if (pages_.size() != before) Bump();
}
PbzResult SetValue(const std::string& owner, const std::string& page, const std::string& item, double value) {
    if (!std::isfinite(value)) return PBZ_E_ARG;
    std::lock_guard<std::mutex> hold(lock_);
    Page* p = FindPage(owner, page); if (!p) return PBZ_E_NOT_FOUND;
    Item* i = FindItemIn(*p, item); if (!i) return PBZ_E_NOT_FOUND;
    if (i->kind == PBZ_UI_LABEL || i->kind == PBZ_UI_TEXT || i->kind == PBZ_UI_BUTTON) return PBZ_E_ARG;
    const double v = i->kind == PBZ_UI_TOGGLE ? (value != 0 ? 1 : 0) : i->kind == PBZ_UI_CHOICE ? std::floor(Clamp(value, i->min, i->max)) : Clamp(value, i->min, i->max);
    if (i->value != v) { i->value = v; Bump(); }
    return PBZ_OK;
}
PbzResult SetText(const std::string& owner, const std::string& page, const std::string& item, const std::wstring& text) {
    std::lock_guard<std::mutex> hold(lock_);
    Page* p = FindPage(owner, page); if (!p) return PBZ_E_NOT_FOUND;
    Item* found = FindItemIn(*p, item); if (!found) return PBZ_E_NOT_FOUND;
    if (found->kind != PBZ_UI_LABEL && found->kind != PBZ_UI_TEXT && found->kind != PBZ_UI_BUTTON) return PBZ_E_ARG;
    std::wstring clipped = text.substr(0, found->kind == PBZ_UI_TEXT ? 1200 : 400);
    if (found->kind == PBZ_UI_BUTTON) { if (clipped.empty()) return PBZ_E_ARG; found->label = std::move(clipped); Bump(); return PBZ_OK; }
    if (found->text != clipped) { found->text = std::move(clipped); Bump(); }
    return PBZ_OK;
}
std::vector<Page> Snapshot() { std::lock_guard<std::mutex> hold(lock_); return pages_; }
bool FindItem(const std::string& owner, const std::string& page, const std::string& item, Item& out) {
    std::lock_guard<std::mutex> hold(lock_);
    Page* p = FindPage(owner, page); if (!p) return false;
    const Item* i = FindItemIn(*p, item); if (!i) return false;
    out = *i; return true;
}
std::vector<Binding> Bindings() {
    std::vector<Binding> out;
    std::lock_guard<std::mutex> hold(lock_);
    for (const Page& p : pages_) for (const Item& i : p.items) if (i.bound() && i.kind != PBZ_UI_BUTTON) out.push_back({p.owner, p.id, i.id, i.bind, i.kind});
    return out;
}
void SetShown(const std::string& owner, const std::string& page, const std::string& item, double value, const std::wstring& text, bool has_text) {
    std::lock_guard<std::mutex> hold(lock_);
    Page* p = FindPage(owner, page); if (!p) return;
    Item* i = FindItemIn(*p, item); if (!i) return;
    bool changed = false;
    if (has_text) { if (i->text != text) { i->text = text; changed = true; } }
    else if (i->value != value) { i->value = value; changed = true; }
    if (changed) Bump();
}
uint64_t Serial() { return serial_.load(); }
void ResetForTest() { std::lock_guard<std::mutex> hold(lock_); pages_.clear(); Bump(); }
}  // namespace PluginUi
