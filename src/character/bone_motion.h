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
// The 身材 / 骨骼 editor (NpcBody / NpcBridge) writes the hero's pose through this module's hook, so one component has one
// hook (two hooks on the pose finalize slot refuse each other). `listener` runs right after the engine finalized
// `component`'s pose and after this module's own edits (骨骼变换 nodes), on the thread that finalized it.
// Game thread; nullptr stops at once. The hook itself follows at the next Tick.
using PoseListener=void(*)(ue::Object* component);
void Listen(ue::Object* component,PoseListener listener);
// More listeners beside the editor's (2026-11-13: Listen is one slot of several, so a later listener no longer replaces an earlier
// one). AddListener returns an id (> 0) or 0 when the slots (3) are taken; RemoveListener(id) stops it at once. Same rules as Listen: game
// thread to add / remove, the function runs on the finalizing thread right after the engine's pose and the edits above, and must not
// call engine functions or allocate (an atomic counter is the intended use). A listener keeps the component's hook alive.
int AddListener(ue::Object* component,PoseListener listener);
void RemoveListener(int id);
bool Listening();          // the listened component has its hook now
const char* ListenNote();  // one line for the editor's status
}
