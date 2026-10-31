#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// In-process access to the UObject runtime of the one PBZ build accepted by
// proxy.cpp (SHA-256 pinned). Layout constants were read from that build's
// live reflection data. Attach/FindEngine/ValidateLayout only read memory and
// may run on the worker; everything else runs on the game thread (tick hook).
namespace ue {
using Object = unsigned char;

struct Name {
    uint32_t index = 0, number = 0;
    bool None() const { return !index && !number; }
    bool operator==(const Name& o) const { return index == o.index && number == o.number; }
};
// Matches FString/TArray headers. Engine-owned arrays are released with Free().
struct Array { void* data = nullptr; int32_t num = 0, max = 0; };
struct String { const wchar_t* data = nullptr; int32_t num = 0, max = 0; };

namespace layout {
constexpr uintptr_t kNamePool = 0x0dd11ac0, kObjectArray = 0x0ddb3c40;
constexpr uintptr_t kFree = 0x012e1930;                 // FMemory::Free
constexpr uintptr_t kProcessEvent = 0x01631410;         // UObject::ProcessEvent
constexpr uintptr_t kGameEngineTick = 0x03f3e670;       // UGameEngine::Tick
constexpr uintptr_t kApplySkinnedHairMode = 0x03db4460; // MainHairMode visibility of one USkinnedMeshComponent
constexpr uintptr_t kApplyGroomHairMode = 0x050dc010;   // MainHairMode state of one UGroomComponent
constexpr size_t kProcessEventSlot = 76, kTickSlot = 94;
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0c, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kFieldNext = 0x28, kSuper = 0x40, kChildren = 0x48, kChildProperties = 0x50;
constexpr size_t kPropertiesSize = 0xa8;  // UStruct::PropertiesSize (Vector 24, LinearColor 16, Transform 96)
constexpr size_t kPropNext = 0x10, kPropName = 0x20, kPropElementSize = 0x34, kPropOffset = 0x44;
constexpr size_t kFuncParmsSize = 0x106;
}

bool Attach(uintptr_t image_base);
uintptr_t Base();
Object* FindEngine();                  // first non-CDO object whose class is GameEngine
bool ValidateLayout(char* why, size_t why_size);

bool NameIs(Name name, const char* text);
bool NameContains(Name name, const char* const* needles, size_t count);  // case-insensitive
void NameText(Name name, char* out, size_t out_size);
Name NameOf(Object* object);
Object* ClassOf(Object* object);
Object* OuterOf(Object* object);
bool IsA(Object* object, Object* cls);
bool IsDefault(Object* object);
bool IsTemplate(Object* object);       // class default object or archetype (Blueprint component template)
bool Alive(Object* object);             // still in GUObjectArray and not garbage

Object* FindObject(const char* path);   // "/Script/Engine.Actor"; results cached
Object* FindFunction(Object* cls, const char* name);
int32_t Offset(Object* strukt, const char* property);  // -1 when missing
int32_t StructSize(Object* strukt);                    // UStruct::PropertiesSize, 0 when missing
template<class T> T& At(void* base, int32_t offset) { return *reinterpret_cast<T*>(static_cast<unsigned char*>(base) + offset); }

namespace detail { Object* ObjectAt(int32_t index); int32_t ObjectCount(); }
template<class F> void ForEachObject(F&& visit) {
    const int32_t count = detail::ObjectCount();
    for (int32_t i = 0; i < count; ++i)
        if (Object* o = detail::ObjectAt(i)) if (!visit(o)) return;
}

// ProcessEvent with a zeroed parameter block sized by the UFunction. The block is
// 16-byte aligned because FTransform parameters are read with aligned SSE loads.
#pragma warning(push)
#pragma warning(disable: 4324)
class Call {
public:
    Call(Object* target, const char* function);
    // Use a function already resolved on the class chain. Needed when more than
    // one GetTimelineByName exists and the first match is not the Lego scanner.
    Call(Object* target, Object* function);
    // Interface functions can be owned by UInterface rather than the actor's
    // class chain. ProcessEvent -> UFunction::Invoke adjusts the native this to
    // GetInterfaceAddress; never call an interface exec with the actor address.
    Call(Object* target, const char* function, const char* interface_path);
    bool Ok() const { return target_ && function_ && !bad_; }
    template<class T> Call& Set(const char* param, const T& value) {
        if (unsigned char* p = Slot(param, sizeof(T))) std::memcpy(p, &value, sizeof(T)); else bad_ = true;
        return *this;
    }
    template<class T> T Get(const char* param) {
        T value{}; if (unsigned char* p = Slot(param, sizeof(T))) std::memcpy(&value, p, sizeof(T)); return value;
    }
    unsigned char* Slot(const char* param, size_t bytes);
    bool Invoke();
private:
    Object* target_; Object* function_ = nullptr; uint16_t size_ = 0; bool bad_ = false;
    alignas(16) unsigned char params_[1024]{};
};
#pragma warning(pop)
Object* Default(const char* cdo_path);  // class default object used as a static-call target

Name MakeName(const wchar_t* text);      // KismetStringLibrary::Conv_StringToName
void Free(void* memory);
}
