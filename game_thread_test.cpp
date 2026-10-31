// Offline checks for the map-change fix: the tick hook survives faults in its callback (reported
// with code and address, stops after kMaxFaults), and ue:: caches drop objects the garbage
// collector freed even when a new object is built at the same address.
#include "game_thread.h"
#include "ue_runtime.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }

int ticks = 0, callbacks = 0, mode = 0;  // mode: 0 = ok, 1 = access violation
DWORD reported_code = 0, reported_faults = 0; uintptr_t reported_address = 0; bool reported_disabled = false; int reports = 0;
void __fastcall FakeTick(void*, float, bool) { ++ticks; }
void __fastcall Other(void*) {}
void Callback() {
    ++callbacks;
    if (mode == 1) *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(8)) = 1;
}
void OnFault(DWORD code, uintptr_t address, DWORD faults, bool disabled) {
    ++reports; reported_code = code; reported_address = address; reported_faults = faults; reported_disabled = disabled;
}

void TestFaultRecovery() {
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    ue::Attach(exe);
    std::vector<uintptr_t> table(1 + 100 + 1, 0);
    for (size_t i = 1; i <= 100; ++i) table[i] = reinterpret_cast<uintptr_t>(&Other);
    table[1 + ue::layout::kTickSlot] = reinterpret_cast<uintptr_t>(&FakeTick);
    struct Engine { uintptr_t* vtable; } engine{table.data() + 1};
    char why[160]{};
    Check(GameThread::Install(&engine, reinterpret_cast<uintptr_t>(&FakeTick), &Callback, &OnFault, why, sizeof(why)), "hook installs");
    using Tick = void(__fastcall*)(void*, float, bool);
    auto tick = [&] { reinterpret_cast<Tick>(engine.vtable[ue::layout::kTickSlot])(&engine, 0.016f, false); };
    tick();
    Check(ticks == 1 && callbacks == 1 && reports == 0, "callback then engine tick");
    mode = 1; tick();
    Check(ticks == 2 && reports == 1 && reported_code == EXCEPTION_ACCESS_VIOLATION && reported_faults == 1 && !reported_disabled,
        "fault reported, engine tick still runs");
    HMODULE module = nullptr;
    Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(reported_address), &module) && reinterpret_cast<uintptr_t>(module) == exe, "fault address is the faulting instruction");
    mode = 0; tick();
    Check(callbacks == 3 && GameThread::Faults() == 1, "callback resumes after a fault");
    mode = 1; tick(); tick();
    Check(reports == 3 && reported_faults == GameThread::kMaxFaults && reported_disabled, "last allowed fault says disabled");
    mode = 0; tick();
    Check(callbacks == 5 && ticks == 6 && reports == 3, "callback stopped, engine keeps ticking");
}

// Fake name pool and GUObjectArray inside a reserved "image" at the layout RVAs.
struct Fake {
    uintptr_t base = 0;
    std::vector<unsigned char> names = std::vector<unsigned char>(4096);
    size_t name_end = 2;
    std::vector<unsigned char> items = std::vector<unsigned char>(64 * 24);
    uintptr_t chunks[1]{};
    int32_t count = 0;
    std::vector<std::vector<unsigned char>> objects;

    ue::Name Add(const char* text) {
        const size_t at = name_end, n = std::strlen(text);
        const uint16_t header = static_cast<uint16_t>(n << 6);
        std::memcpy(&names[at], &header, 2); std::memcpy(&names[at + 2], text, n);
        name_end = (at + 2 + n + 1) & ~size_t{1};
        return ue::Name{static_cast<uint32_t>(at / 2), 0};
    }
    unsigned char* Memory() { objects.emplace_back(0x80); return objects.back().data(); }
    // Registers `o` (fresh or reused memory) in a new GUObjectArray slot.
    unsigned char* Put(unsigned char* o, const char* name, unsigned char* outer, unsigned char* cls = nullptr) {
        const int32_t index = count++;
        std::memset(o, 0, 0x80);
        const ue::Name n = Add(name);
        std::memcpy(o + ue::layout::kObjIndex, &index, 4); std::memcpy(o + ue::layout::kObjName, &n, 8);
        std::memcpy(o + ue::layout::kObjOuter, &outer, 8); std::memcpy(o + ue::layout::kObjClass, &cls, 8);
        std::memcpy(&items[index * 24 + 8], &o, 8);
        *reinterpret_cast<int32_t*>(base + ue::layout::kObjectArray + 0x20) = count;
        return o;
    }
    void Collect(unsigned char* o) {  // the garbage collector frees the slot
        int32_t index = 0; std::memcpy(&index, o + ue::layout::kObjIndex, 4);
        std::memset(&items[index * 24 + 8], 0, 8);
    }
    bool Init() {
        base = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, 0x10000000, MEM_RESERVE, PAGE_NOACCESS));
        if (!base) return false;
        for (uintptr_t rva : {ue::layout::kNamePool, ue::layout::kObjectArray})
            if (!VirtualAlloc(reinterpret_cast<void*>(base + (rva & ~uintptr_t{4095})), 8192, MEM_COMMIT, PAGE_READWRITE)) return false;
        *reinterpret_cast<uint32_t*>(base + ue::layout::kNamePool + 8) = 0;
        *reinterpret_cast<uintptr_t*>(base + ue::layout::kNamePool + 16) = reinterpret_cast<uintptr_t>(names.data());
        chunks[0] = reinterpret_cast<uintptr_t>(items.data());
        *reinterpret_cast<uintptr_t*>(base + ue::layout::kObjectArray + 0x18) = reinterpret_cast<uintptr_t>(chunks);
        ue::Attach(base);
        return true;
    }
};

void Link(unsigned char* cls, unsigned char* function) { std::memcpy(cls + ue::layout::kChildren, &function, 8); }

void TestCaches() {
    Fake fake;
    if (!fake.Init()) { Check(false, "fake runtime"); return; }
    unsigned char* package = fake.Put(fake.Memory(), "/Game/BP_Donor", nullptr);
    unsigned char* donor = fake.Put(fake.Memory(), "BP_Donor_C", package);
    unsigned char* function = fake.Put(fake.Memory(), "Fn", donor);
    Link(donor, function);
    const char* path = "/Game/BP_Donor.BP_Donor_C";
    Check(ue::FindObject(path) == donor && ue::FindFunction(donor, "Fn") == function, "found and cached");
    Check(ue::Alive(donor) && ue::FindObject(path) == donor, "cached object still served while alive");
    // The class is collected and an unrelated object is built at its address.
    fake.Collect(donor); fake.Collect(function);
    fake.Put(donor, "WB_Other_C", package);
    Check(ue::FindObject(path) == nullptr, "stale cached class not returned for reused memory");
    // The donor is loaded again at the same address, with a new function object.
    fake.Collect(donor);
    fake.Put(donor, "BP_Donor_C", package);
    unsigned char* reloaded = fake.Put(fake.Memory(), "Fn", donor);
    Link(donor, reloaded);
    Check(ue::FindObject(path) == donor, "reloaded class found at the same address");
    Check(ue::FindFunction(donor, "Fn") == reloaded, "stale cached function replaced by the reloaded one");
    VirtualFree(reinterpret_cast<void*>(fake.base), 0, MEM_RELEASE);
}
}

int main() {
    TestFaultRecovery();
    TestCaches();
    std::printf("game_thread_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
