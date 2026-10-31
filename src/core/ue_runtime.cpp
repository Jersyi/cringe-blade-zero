#include "ue_runtime.h"
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace ue {
namespace {
uintptr_t base_ = 0;
std::map<std::string, Object*> object_cache;
std::map<std::pair<Object*, std::string>, Object*> function_cache;
std::map<std::pair<Object*, std::string>, int32_t> offset_cache;

template<class T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }
uintptr_t Addr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

// FNameEntry: 16-bit header (length << 6 | wide) followed by the characters.
bool Entry(Name name, const unsigned char*& text, size_t& length, bool& wide) {
    const uintptr_t pool = base_ + layout::kNamePool;
    const uint32_t current = Read<uint32_t>(pool + 8), block = name.index >> 16;
    if (block > current || block >= 8192) return false;
    const uintptr_t entry = Read<uintptr_t>(pool + 16 + block * 8ull) + (name.index & 0xffffu) * 2ull;
    const uint16_t header = Read<uint16_t>(entry);
    length = header >> 6; wide = header & 1; text = reinterpret_cast<const unsigned char*>(entry + 2);
    return length > 0 && length < 1024;
}
char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// Splits "/Script/Engine.Actor" or "A.B:C" into names from the leaf upwards.
bool MatchPath(Object* object, const char* path) {
    std::string p(path);
    while (object) {
        const size_t cut = p.find_last_of(".:");
        const std::string leaf = cut == std::string::npos ? p : p.substr(cut + 1);
        if (!NameIs(NameOf(object), leaf.c_str())) return false;
        object = OuterOf(object);
        if (cut == std::string::npos) return object == nullptr;
        p.resize(cut);
    }
    return false;
}
}

namespace detail {
int32_t ObjectCount() { return Read<int32_t>(base_ + layout::kObjectArray + 0x20); }
Object* ObjectAt(int32_t index) {
    const uintptr_t chunks = Read<uintptr_t>(base_ + layout::kObjectArray + 0x18);
    const uintptr_t chunk = Read<uintptr_t>(chunks + (static_cast<uint32_t>(index) >> 16) * 8ull);
    return chunk ? Read<Object*>(chunk + (index & 0xffff) * 24ull + 8) : nullptr;
}
}

bool Attach(uintptr_t image_base) { base_ = image_base; return base_ != 0; }
uintptr_t Base() { return base_; }

bool NameIs(Name name, const char* wanted) {
    const unsigned char* text = nullptr; size_t length = 0; bool wide = false;
    if (!Entry(name, text, length, wide)) return false;
    char suffix[16]{};
    if (name.number) sprintf_s(suffix, "_%u", name.number - 1);
    const size_t want = std::strlen(wanted), extra = std::strlen(suffix);
    if (want != length + extra) return false;
    for (size_t i = 0; i < length; ++i) {
        // FNames compare case-insensitively: the stored casing is the first registration's ("Lods" for LODs).
        const unsigned c = wide ? (text[i * 2] | text[i * 2 + 1] << 8) : text[i];
        const unsigned w = static_cast<unsigned char>(wanted[i]);
        if (c != w && !(c < 128 && std::tolower(static_cast<int>(c)) == std::tolower(static_cast<int>(w)))) return false;
    }
    return std::memcmp(wanted + length, suffix, extra) == 0;
}
void NameText(Name name, char* out, size_t size) {
    const unsigned char* text = nullptr; size_t length = 0; bool wide = false;
    if (!size) return;
    if (!Entry(name, text, length, wide)) { strcpy_s(out, size, "?"); return; }
    size_t n = 0;
    for (; n < length && n + 1 < size; ++n) {
        const unsigned c = wide ? (text[n * 2] | text[n * 2 + 1] << 8) : text[n];
        out[n] = c < 128 ? static_cast<char>(c) : '?';
    }
    out[n] = 0;
    if (name.number) { char suffix[16]{}; sprintf_s(suffix, "_%u", name.number - 1); strcat_s(out, size, suffix); }
}
bool NameContains(Name name, const char* const* needles, size_t count) {
    char text[256]{}; NameText(name, text, sizeof(text));
    for (char& c : text) c = Lower(c);
    for (size_t i = 0; i < count; ++i) if (std::strstr(text, needles[i])) return true;
    return false;
}
Name NameOf(Object* o) { return Read<Name>(Addr(o) + layout::kObjName); }
Object* ClassOf(Object* o) { return Read<Object*>(Addr(o) + layout::kObjClass); }
Object* OuterOf(Object* o) { return Read<Object*>(Addr(o) + layout::kObjOuter); }
bool IsDefault(Object* o) { return (Read<uint32_t>(Addr(o) + layout::kObjFlags) & 0x10) != 0; }  // RF_ClassDefaultObject
bool IsTemplate(Object* o) { return (Read<uint32_t>(Addr(o) + layout::kObjFlags) & 0x30) != 0; }  // RF_ClassDefaultObject | RF_ArchetypeObject
bool IsA(Object* object, Object* cls) {
    if (!object || !cls) return false;
    for (Object* c = ClassOf(object); c; c = Read<Object*>(Addr(c) + layout::kSuper)) if (c == cls) return true;
    return false;
}
bool Alive(Object* object) {
    if (!object) return false;
    __try {
        const int32_t index = Read<int32_t>(Addr(object) + layout::kObjIndex);
        if (index < 0 || index >= detail::ObjectCount() || detail::ObjectAt(index) != object) return false;
        const uintptr_t chunks = Read<uintptr_t>(base_ + layout::kObjectArray + 0x18);
        const uint64_t item = Read<uint64_t>(Read<uintptr_t>(chunks + (static_cast<uint32_t>(index) >> 16) * 8ull) + (index & 0xffff) * 24ull);
        const uint32_t internal = static_cast<uint32_t>(item >> 32), flags = Read<uint32_t>(Addr(object) + layout::kObjFlags);
        // EInternalObjectFlags Garbage|Unreachable; RF_BeginDestroyed|RF_FinishDestroyed|RF_MirroredGarbage.
        return !(internal & ((1u << 21) | (1u << 28))) && !(flags & (0x8000u | 0x10000u | 0x40000000u));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Object* FindObject(const char* path) {
    if (auto it = object_cache.find(path); it != object_cache.end()) {
        // Loaded (blueprint) objects are garbage-collected once unused and their memory reused.
        if (Alive(it->second) && MatchPath(it->second, path)) return it->second;
        object_cache.erase(it);
    }
    const char* leaf = path + std::strlen(path);
    while (leaf > path && leaf[-1] != '.' && leaf[-1] != ':' ) --leaf;
    Object* found = nullptr;
    ForEachObject([&](Object* o) {
        if (NameIs(NameOf(o), leaf) && MatchPath(o, path)) { found = o; return false; }
        return true;
    });
    if (found) object_cache[path] = found;
    return found;
}
Object* FindFunction(Object* cls, const char* name) {
    const auto key = std::make_pair(cls, std::string(name));
    if (auto it = function_cache.find(key); it != function_cache.end()) {
        // A collected blueprint class can be rebuilt at the same address: the function must still be its own.
        if (Alive(it->second)) for (Object* s = cls; s; s = Read<Object*>(Addr(s) + layout::kSuper)) if (OuterOf(it->second) == s) return it->second;
        function_cache.erase(it);
    }
    for (Object* s = cls; s; s = Read<Object*>(Addr(s) + layout::kSuper))
        for (Object* f = Read<Object*>(Addr(s) + layout::kChildren); f; f = Read<Object*>(Addr(f) + layout::kFieldNext))
            if (NameIs(NameOf(f), name)) return function_cache[key] = f;
    return nullptr;
}
int32_t Offset(Object* strukt, const char* property) {
    const auto key = std::make_pair(strukt, std::string(property));
    if (auto it = offset_cache.find(key); it != offset_cache.end()) return it->second;
    for (Object* s = strukt; s; s = Read<Object*>(Addr(s) + layout::kSuper))
        for (uintptr_t f = Read<uintptr_t>(Addr(s) + layout::kChildProperties); f; f = Read<uintptr_t>(f + layout::kPropNext))
            if (NameIs(Read<Name>(f + layout::kPropName), property)) return offset_cache[key] = Read<int32_t>(f + layout::kPropOffset);
    return -1;
}

int32_t StructSize(Object* strukt) { return strukt ? Read<int32_t>(Addr(strukt) + layout::kPropertiesSize) : 0; }

Object* FindEngine() {
    Object* engine = nullptr;
    __try {
        ForEachObject([&](Object* o) {
            if (!IsDefault(o) && NameIs(NameOf(ClassOf(o)), "GameEngine")) { engine = o; return false; }
            return true;
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return engine;
}

bool ValidateLayout(char* why, size_t size) {
    struct Expect { const char* strukt; const char* property; int32_t offset; };
    static constexpr Expect expected[] = {
        {"/Script/HairStrandsCore.GroomComponent", "GroomAsset", 0x580},
        {"/Script/HairStrandsCore.GroomComponent", "bIsMainHair", 0x588},
        {"/Script/HairStrandsCore.GroomComponent", "BindingAsset", 0x5b0},
        {"/Script/HairStrandsCore.GroomComponent", "SimulationSettings", 0x5d0},
        {"/Script/Engine.SkinnedMeshComponent", "bInfluencedByMainHairMode", 0x794},
        {"/Script/Engine.SceneComponent", "AttachParent", 0xd0},
        {"/Script/ProjectPBZ.PBZAppearanceStruct", "GroomDataArray", 0x198},
    };
    for (const Expect& e : expected) {
        Object* s = FindObject(e.strukt);
        if (!s || Offset(s, e.property) != e.offset) { sprintf_s(why, size, "layout %s.%s", e.strukt, e.property); return false; }
    }
    Object* object_cdo = FindObject("/Script/CoreUObject.Default__Object");
    if (!object_cdo || Read<uintptr_t>(Read<uintptr_t>(Addr(object_cdo)) + layout::kProcessEventSlot * 8) != base_ + layout::kProcessEvent) {
        strcpy_s(why, size, "ProcessEvent slot"); return false;
    }
    return true;
}

Call::Call(Object* target, const char* function) : target_(target) {
    if (target_) function_ = FindFunction(ClassOf(target_), function);
    if (function_) size_ = Read<uint16_t>(Addr(function_) + layout::kFuncParmsSize);
    bad_ = !function_ || size_ > sizeof(params_);
}
Call::Call(Object* target, Object* function) : target_(target), function_(function) {
    if (function_) size_ = Read<uint16_t>(Addr(function_) + layout::kFuncParmsSize);
    bad_ = !target_ || !function_ || size_ > sizeof(params_);
}
Call::Call(Object* target, const char* function, const char* interface_path) : target_(target) {
    // A Blueprint override is the executable implementation. Calling the
    // interface owner's native exec directly can dereference a null native
    // interface address when the implementation exists only in Blueprint.
    if (target_) function_ = FindFunction(ClassOf(target_), function);
    if (function_) {
        size_ = Read<uint16_t>(Addr(function_) + layout::kFuncParmsSize);
        bad_ = size_ > sizeof(params_); return;
    }
    Object* interface_class = FindObject(interface_path);
    Call implements(Default("/Script/Engine.Default__KismetSystemLibrary"), "DoesImplementInterface");
    if (!target_ || !interface_class || !implements.Set("TestObject", target_).Set("Interface", interface_class).Invoke() ||
        !implements.Get<bool>("ReturnValue")) { bad_ = true; return; }
    function_ = FindFunction(interface_class, function);
    if (function_) size_ = Read<uint16_t>(Addr(function_) + layout::kFuncParmsSize);
    bad_ = !function_ || size_ > sizeof(params_);
}
unsigned char* Call::Slot(const char* param, size_t bytes) {
    if (!function_) return nullptr;
    const int32_t o = Offset(function_, param);
    return o >= 0 && o + bytes <= size_ ? params_ + o : nullptr;
}
bool Call::Invoke() {
    if (!Ok()) return false;
    using ProcessEvent = void(__fastcall*)(Object*, Object*, void*);
    const auto vtable = Read<uintptr_t*>(Addr(target_));
    reinterpret_cast<ProcessEvent>(vtable[layout::kProcessEventSlot])(target_, function_, params_);
    return true;
}
Object* Default(const char* cdo_path) { return FindObject(cdo_path); }

Name MakeName(const wchar_t* text) {
    const int32_t n = static_cast<int32_t>(wcslen(text)) + 1;
    Call c(Default("/Script/Engine.Default__KismetStringLibrary"), "Conv_StringToName");
    c.Set("InString", String{text, n, n});
    return c.Invoke() ? c.Get<Name>("ReturnValue") : Name{};
}
void Free(void* memory) {
    if (memory) reinterpret_cast<void(__fastcall*)(void*)>(base_ + layout::kFree)(memory);
}
}
