#pragma once
#include "bone_motion_model.h"
#include "ue_runtime.h"
#include <string>
#include <vector>
namespace BoneMotionRuntime {
using Logger=void(*)(const char*);
void Initialize(Logger logger);
void Tick(bool transitioning); // game thread, after appearance and preview updates
void SetGlobal(const BoneMotion::Config& config,bool save=true); // any thread
BoneMotion::Config Global();
void Preview(const BoneMotion::Config& config); // any thread; follows the preview boss only
bool BeforeDormancy(ue::Object* component,bool(*quiesce)(ue::Object*)); // retire supplemental displays before parking the source
void BeforeComponentHook(ue::Object* component); // game thread, after animation tasks are quiescent
struct Snapshot {std::string mesh,note,supplement_note;bool supplement_enabled=false;int supplement_displays=0;std::vector<std::string> bones,missing;uint64_t frames=0;int hooks=0,matched=0;double max_delta=0;
    bool debug=false,debug_preview=false;uint64_t debug_frames=0;std::vector<int> parents;std::vector<std::array<double,3>> positions;std::vector<BoneMotion::Transform> reference;};
void Debug(bool enabled);
void Supplement(bool enabled,bool save=true);
bool SupplementEnabled();
Snapshot Get();
bool Command(const std::string& text); // worker, diagnostic commands are not persisted
}
