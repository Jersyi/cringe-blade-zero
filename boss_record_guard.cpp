#include "boss_record_guard.h"
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>

namespace BossRecordGuard {
namespace {
using ue::Call;
using ue::Object;

// FJsonObjectWrapper: engine FString followed by TSharedPtr<FJsonObject>.
// Verified in this PBZ build's UHT and native exec cleanup sequences.
struct JsonWrapper {
    ue::String text;
    void* object = nullptr;
    void* controller = nullptr;
};
static_assert(sizeof(JsonWrapper) == 32);
constexpr uintptr_t kReleaseSharedReference = 0x0126fc00;
constexpr uintptr_t kSyncAdvancedToMaster = 0x06b2a4d0;
constexpr int32_t kMaxTextCharacters = 4 * 1024 * 1024;
Logger logger_ = nullptr;
Object* captured_save_ = nullptr;
ue::Name captured_save_name_;
bool captured_ = false, existed_ = false;
std::wstring baseline_;

void Log(const char* format, ...) {
    if (!logger_) return;
    char line[640]{};
    va_list args;
    va_start(args, format);
    vsprintf_s(line, format, args);
    va_end(args);
    logger_(line);
}

bool NativeHelpersMatch() {
    if (!ue::Base()) return false;
    static constexpr unsigned char release[] = {
        0x48, 0x85, 0xc9, 0x74, 0x42, 0x53, 0x48, 0x83,
        0xec, 0x20, 0x48, 0x89, 0x7c, 0x24, 0x30, 0x48
    };
    static constexpr unsigned char sync[] = {
        0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x83,
        0xb9, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b
    };
    return !std::memcmp(reinterpret_cast<const void*>(ue::Base() + kReleaseSharedReference), release, sizeof(release)) &&
        !std::memcmp(reinterpret_cast<const void*>(ue::Base() + kSyncAdvancedToMaster), sync, sizeof(sync));
}

// Own only an engine-created output. Call::Set inputs remain borrowed and are
// never destroyed here. Moving an output out zeroes its original parameter slot.
class OwnedJson {
public:
    JsonWrapper value;
    OwnedJson() = default;
    OwnedJson(const OwnedJson&) = delete;
    OwnedJson& operator=(const OwnedJson&) = delete;
    ~OwnedJson() { Reset(); }
    bool Take(Call& call, const char* field) {
        unsigned char* slot = call.Slot(field, sizeof(value));
        if (!slot) return false;
        Reset();
        std::memcpy(&value, slot, sizeof(value));
        std::memset(slot, 0, sizeof(value));
        return value.object && value.controller;
    }
private:
    void Reset() {
        // This exact release helper decrements strong refs, destroys the JSON
        // only at zero, then releases its weak ref. It is not FMemory::Free.
        if (value.controller)
            reinterpret_cast<void(__fastcall*)(void*)>(ue::Base() + kReleaseSharedReference)(value.controller);
        ue::Free(const_cast<wchar_t*>(value.text.data));
        value = {};
    }
};

class OwnedText {
public:
    ue::String value;
    ~OwnedText() { ue::Free(const_cast<wchar_t*>(value.data)); }
    bool Take(Call& call, const char* field) {
        unsigned char* slot = call.Slot(field, sizeof(value));
        if (!slot) return false;
        std::memcpy(&value, slot, sizeof(value));
        std::memset(slot, 0, sizeof(value));
        return value.num >= 1 && value.num <= kMaxTextCharacters && value.max >= value.num &&
            value.data && value.data[value.num - 1] == L'\0';
    }
};

ue::String Borrow(const std::wstring& text) {
    const int32_t size = static_cast<int32_t>(text.size() + 1);
    return {text.c_str(), size, size};
}
ue::String Field() {
    static constexpr wchar_t key[] = L"BossRecord";
    return {key, static_cast<int32_t>(std::size(key)), static_cast<int32_t>(std::size(key))};
}
Object* Utility() { return ue::Default("/Script/HTTPJsonUtility.Default__JSONUtility"); }

bool Ready(Object* save) {
    if (!save || !ue::Alive(save) || !NativeHelpersMatch()) return false;
    Object* wrapper = ue::FindObject("/Script/JsonUtilities.JsonObjectWrapper");
    Object* utility = Utility();
    if (!wrapper || ue::StructSize(wrapper) != static_cast<int32_t>(sizeof(JsonWrapper)) || ue::Offset(wrapper, "JsonString") != 0 || !utility)
        return false;
    const auto has = [](Object* object, const char* name) { return ue::FindFunction(ue::ClassOf(object), name) != nullptr; };
    return has(save, "GetAdvancedSaveJsonObject") && has(save, "GetAdvancedObjectJsonWrapper") &&
        has(save, "SaveAdvancedObjectDataAndSync") && has(save, "MarkAdvancedSaveDirty") &&
        has(utility, "JsonHasField") && has(utility, "JsonToString") &&
        has(utility, "ParseStringToJson") && has(utility, "RemoveJsonField") && has(utility, "TryGetObjField");
}

bool Root(Object* save, OwnedJson& root) {
    Call get(save, "GetAdvancedSaveJsonObject");
    return get.Invoke() && root.Take(get, "ReturnValue");
}
bool Exists(const JsonWrapper& root, bool& exists) {
    Call check(Utility(), "JsonHasField");
    if (!check.Set("JsonObject", root).Set("FieldToFind", Field()).Invoke()) return false;
    exists = check.Get<bool>("bSuccess");
    return check.Ok();
}
bool RecordIsObject(const JsonWrapper& root) {
    // The save subsystem getter supplies an empty object even for a malformed
    // or missing field. Refuse a non-object baseline instead of normalizing it.
    Call check(Utility(), "TryGetObjField");
    if (!check.Set("JsonObject", root).Set("FieldName", Field()).Invoke()) return false;
    OwnedJson field;
    const bool valid = field.Take(check, "JsonObjectField");
    return valid && check.Get<bool>("bSuccess") && check.Ok();
}
bool Serialize(const JsonWrapper& object, std::wstring& text) {
    Call stringify(Utility(), "JsonToString");
    if (!stringify.Set("JsonObject", object).Invoke()) return false;
    OwnedText output;
    const bool valid = output.Take(stringify, "JsonString");
    if (!valid || !stringify.Get<bool>("bSuccess") || !stringify.Ok()) return false;
    text.assign(output.value.data, static_cast<size_t>(output.value.num - 1));
    return true;
}
bool Parse(const std::wstring& text, OwnedJson& json) {
    if (text.empty() || text.size() >= static_cast<size_t>(kMaxTextCharacters)) return false;
    Call parse(Utility(), "ParseStringToJson");
    if (!parse.Set("RawStringData", Borrow(text)).Invoke()) return false;
    const bool valid = json.Take(parse, "LoadedJsonObject");
    return valid && parse.Get<bool>("bSuccess") && parse.Ok();
}
bool ReadRecord(Object* save, OwnedJson& json) {
    const ue::Name id = ue::MakeName(L"BossRecord");
    if (id.None()) return false;
    Call get(save, "GetAdvancedObjectJsonWrapper");
    if (!get.Set("ObjectID", id).Invoke()) return false;
    const bool valid = json.Take(get, "JsonObjectWrapper");
    return valid && get.Get<bool>("ReturnValue") && get.Ok();
}

bool RemoveRecord(Object* save) {
    // GetAdvancedSaveJsonObject returns a ref-counted alias of the Advanced root
    // at this+0xa8 (native 0x6b19ab0). RemoveJsonField modifies that same JSON
    // object before constructing its output (native exec 0x62c669a).
    // SyncAdvancedToMaster deep-copies this root into master and updates its
    // revision. It does not write disk or bypass the caller's save gates.
    // Its null guards at this+0xb8 / this+0xd8 are verified private build offsets.
    if (!ue::At<void*>(save, 0xb8) || !ue::At<void*>(save, 0xd8)) return false;
    OwnedJson root;
    bool present = false;
    if (!Root(save, root) || !Exists(root.value, present)) return false;
    if (present) {
        Call remove(Utility(), "RemoveJsonField");
        if (!remove.Set("JsonObject", root.value).Set("FieldName", Field()).Invoke()) return false;
        OwnedJson result;
        const bool valid = result.Take(remove, "NewJsonObject");
        if (!valid || !remove.Get<bool>("bSuccess") || !remove.Ok()) return false;
    }
    if (!Call(save, "MarkAdvancedSaveDirty").Invoke()) return false;
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + kSyncAdvancedToMaster)(save);
    OwnedJson check;
    bool after = true;
    return Root(save, check) && Exists(check.value, after) && !after;
}
}

void Initialize(Logger logger) { logger_ = logger; Clear(); }

bool Capture(Object* save) {
    if (captured_)
        return save == captured_save_ && ue::Alive(save) && ue::NameOf(save) == captured_save_name_;
    if (!Ready(save)) { Log("BOSS_RECORD capture refused: native helpers or reflected API mismatch"); return false; }
    OwnedJson root;
    bool present = false;
    if (!Root(save, root) || !Exists(root.value, present)) return false;
    std::wstring text;
    if (present) {
        OwnedJson record, parsed;
        std::wstring roundtrip;
        if (!RecordIsObject(root.value) || !ReadRecord(save, record) || !Serialize(record.value, text) || !Parse(text, parsed) ||
            !Serialize(parsed.value, roundtrip) || text != roundtrip) return false;
    }
    // Commit the baseline only after every read and deep-copy check succeeds.
    captured_save_ = save;
    captured_save_name_ = ue::NameOf(save);
    existed_ = present;
    baseline_ = std::move(text);
    captured_ = true;
    Log("BOSS_RECORD captured account section existed=%d chars=%zu", existed_ ? 1 : 0, baseline_.size());
    return true;
}

bool Restore(Object* save) {
    if (!captured_ || save != captured_save_ || !Ready(save) || !(ue::NameOf(save) == captured_save_name_)) return false;
    if (!existed_) {
        const bool restored = RemoveRecord(save);
        Log("BOSS_RECORD restore account section to absent verified=%d", restored ? 1 : 0);
        return restored;
    }
    OwnedJson baseline;
    const ue::Name id = ue::MakeName(L"BossRecord");
    if (id.None() || !Parse(baseline_, baseline)) return false;
    Call set(save, "SaveAdvancedObjectDataAndSync");
    if (!set.Set("ObjectID", id).Set("JsonObjectWrapper", baseline.value).Invoke()) return false;
    OwnedJson root, actual;
    bool present = false;
    std::wstring text;
    const bool restored = Root(save, root) && Exists(root.value, present) && present &&
        ReadRecord(save, actual) && Serialize(actual.value, text) && text == baseline_;
    Log("BOSS_RECORD restore account section deep-copy/readback verified=%d chars=%zu", restored ? 1 : 0, text.size());
    return restored;
}

void Clear() {
    captured_ = false;
    existed_ = false;
    captured_save_ = nullptr;
    captured_save_name_ = {};
    std::wstring{}.swap(baseline_);
}
}
