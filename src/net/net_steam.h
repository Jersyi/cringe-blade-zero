#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 联机测试 (online probe, phase 0): two players in a Steam lobby exchange messages through the game's own
// steam_api64.dll (Steam build only; the WeGame build ships no Steam DLL). Nothing is linked: every function is looked
// up in the already loaded DLL, so a build without it simply reports "no Steam".
// - Lobby: friends-only, two members. Invites go through the Steam overlay or straight to a friend who plays the game;
//   joining takes an invite (GameLobbyJoinRequested_t), a friend's lobby, a room number, or Steam's +connect_lobby.
// - Messages: ISteamNetworkingMessages to the other lobby member. Channel 0 reliable (hello, ping, the hero's parts),
//   channel 1 unreliable (the hero's state and pose, 30 a second). Protocol 3 (older builds: incompatible).
// - Callbacks are registered with SteamAPI_RegisterCallback (the game itself runs SteamAPI_RunCallbacks; never
//   ManualDispatch). They arrive on whatever thread the game runs them on and are only queued there; everything else
//   runs on the game thread. NET_* log lines record each step for the two-PC test.
namespace NetSteam {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
void OnGameThreadGuarded();

// Any thread: UI and command-file requests, done on the game thread.
enum class Request { Host, Overlay, Invite, Join, JoinFriend, Leave, Ping, Friends };
void Ask(Request request, uint64_t argument = 0);

struct Friend { uint64_t id = 0, lobby = 0; std::wstring name; };
// Texts come in both UI languages: [0] Chinese, [1] English.
struct Snapshot {
    int steam = 0;            // 0 still looking, 1 ready, -1 unavailable (why)
    std::wstring why[2], me;
    uint64_t me_id = 0, lobby = 0;
    bool owner = false, busy = false;
    int members = 0;
    uint64_t peer = 0;
    std::wstring peer_name;
    int link = 0;             // 0 no peer, 1 waiting for the peer's hello, 2 connected, 3 incompatible (note)
    bool other_mod = false;   // the peer runs another mod build (works, but report it)
    std::wstring note[2];     // incompatible: why
    std::string peer_map;     // the map the peer's hero is on (its newest state)
    std::wstring route;       // Steam's connection state for the peer (relayed / direct)
    int rtt_ms = -1;
    uint32_t sent = 0, received = 0, callbacks = 0, dropped = 0;
    std::vector<Friend> friends; uint64_t friends_serial = 0;
    std::wstring last[2];     // the newest event, for the page
};
Snapshot Get();

// Game thread: what the puppet (NetPuppet) sends and reads. montage_id: a hash of the playing montage's path (0 none),
// montage_pos its position (s); dilation: the hero's CustomTimeDilation (a 加速 node shows on the puppet too).
struct State { double pos[3]{}, vel[3]{}; double yaw = 0; char map[48]{}; float montage_pos = 0, dilation = 1; uint32_t montage_id = 0; };
bool PeerReady();
void SendState(const State& state);
// A montage the hero started (path "/Game/.../AM_X.AM_X", position and play rate), or stopped (empty path).
struct Montage { std::string path; float pos = 0, rate = 1; uint64_t received_ms = 0; };
void SendMontage(const Montage& montage);
bool TakeMontage(Montage& montage);                    // the peer's next montage start / stop
// NetPuppet's own payloads: the hero's parts (what it shows: meshes, assets, sockets; reliable, when they change and
// every few seconds) and its pose (each part's place and shown state, the bones of the parts that pose themselves;
// unreliable, 30 a second). Take: the newest one that came since the last take.
bool SendParts(const std::vector<unsigned char>& payload);
bool SendPose(const std::vector<unsigned char>& payload);
bool TakeParts(std::vector<unsigned char>& payload);
bool TakePose(std::vector<unsigned char>& payload, uint64_t& received_ms);
bool LatestState(State& state, uint64_t& received_ms); // the peer's newest state and when it came (GetTickCount64)
uint32_t PathId(const char* path);                     // the montage_id of a path
}
