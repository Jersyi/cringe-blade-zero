#include "net_steam.h"
#include "game_build.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <utility>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace NetSteam {
namespace {
// ---- Steam flat API, as steam_api64.dll (10.83.75.46) exports it --------------------------------------------------
using ApiCall = uint64_t;
// Callback ids: k_iSteamFriendsCallbacks 300, k_iSteamMatchmakingCallbacks 500, k_iSteamNetworkingMessagesCallbacks 1250.
constexpr int kJoinRequested = 333, kLobbyEnter = 504, kLobbyChatUpdate = 506, kLobbyCreated = 513, kSessionRequest = 1251, kSessionFailed = 1252;
// Their sizes on Win64 (VALVE_CALLBACK_PACK_LARGE: 8-byte packing).
constexpr int kJoinRequestedSize = 16, kLobbyEnterSize = 24, kLobbyChatUpdateSize = 32, kLobbyCreatedSize = 16, kSessionRequestSize = 136,
              kSessionFailedSize = 696;
constexpr int kLobbyFriendsOnly = 1, kFriendFlagImmediate = 4, kIdentitySteamId = 16;
constexpr int kSendUnreliable = 1 /*no Nagle*/ | 32 /*restart a broken session*/, kSendReliable = 8 | 1 | 32;
constexpr int kControl = 0, kStateChannel = 1;

struct Identity { int32_t type = 0, size = 0; uint64_t steam_id = 0; unsigned char rest[120]{}; };  // SteamNetworkingIdentity
static_assert(sizeof(Identity) == 136, "SteamNetworkingIdentity");
struct Message {  // SteamNetworkingMessage_t (only read; released through the DLL)
    void* data; int32_t size; uint32_t connection; Identity peer; int64_t connection_user_data, received_us, number;
    void* free_data; void* release; int32_t channel, flags; int64_t user_data; uint16_t lane;
};
static_assert(offsetof(Message, peer) == 16 && offsetof(Message, channel) == 192, "SteamNetworkingMessage_t");

// CCallbackBase exactly as steam_api.h declares it (no virtual destructor: the DLL calls these slots).
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, ApiCall call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
protected:
    uint8_t flags_ = 0;
    int callback_ = 0;
};

struct Api {
    HMODULE dll = nullptr;
    int32_t (*GetHSteamUser)() = nullptr;
    void (*RegisterCallback)(CallbackBase*, int) = nullptr;
    void* (*Matchmaking)() = nullptr; void* (*User)() = nullptr; void* (*Friends)() = nullptr; void* (*Utils)() = nullptr;
    void* (*Messages)() = nullptr; void* (*NetUtils)() = nullptr;
    ApiCall (*CreateLobby)(void*, int, int) = nullptr;
    ApiCall (*JoinLobby)(void*, uint64_t) = nullptr;
    void (*LeaveLobby)(void*, uint64_t) = nullptr;
    bool (*InviteUserToLobby)(void*, uint64_t, uint64_t) = nullptr;
    int (*GetNumLobbyMembers)(void*, uint64_t) = nullptr;
    uint64_t (*GetLobbyMemberByIndex)(void*, uint64_t, int) = nullptr;
    bool (*SetLobbyData)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*GetLobbyData)(void*, uint64_t, const char*) = nullptr;
    uint64_t (*GetLobbyOwner)(void*, uint64_t) = nullptr;
    uint64_t (*GetSteamID)(void*) = nullptr;
    const char* (*GetPersonaName)(void*) = nullptr;
    const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
    void (*ActivateInviteDialog)(void*, uint64_t) = nullptr;
    int (*GetFriendCount)(void*, int) = nullptr;
    uint64_t (*GetFriendByIndex)(void*, int, int) = nullptr;
    bool (*GetFriendGamePlayed)(void*, uint64_t, void*) = nullptr;
    bool (*IsAPICallCompleted)(void*, ApiCall, bool*) = nullptr;
    bool (*GetAPICallResult)(void*, ApiCall, void*, int, int, bool*) = nullptr;
    uint32_t (*GetAppID)(void*) = nullptr;
    bool (*IsOverlayEnabled)(void*) = nullptr;
    int (*SendMessageToUser)(void*, const Identity&, const void*, uint32_t, int, int) = nullptr;
    int (*ReceiveMessagesOnChannel)(void*, int, Message**, int) = nullptr;
    bool (*AcceptSessionWithUser)(void*, const Identity&) = nullptr;
    bool (*CloseSessionWithUser)(void*, const Identity&) = nullptr;
    int (*GetSessionConnectionInfo)(void*, const Identity&, void*, void*) = nullptr;
    void (*ReleaseMessage)(Message*) = nullptr;
    void (*InitRelayNetworkAccess)(void*) = nullptr;
};

// ---- our messages ---------------------------------------------------------------------------------------------------
constexpr uint32_t kMagic = 0x305a4250;  // "PBZ0"
constexpr uint16_t kProtocol = 3;  // 2: montages instead of ability starts; 3: the hero's parts and pose (NetPuppet)
enum : uint8_t { kHello = 1, kPing = 2, kPong = 3, kState = 4, kMontage = 6, kParts = 7, kPose = 8 };
#pragma pack(push, 1)
struct Header { uint32_t magic = kMagic; uint16_t version = kProtocol; uint8_t type = 0, flags = 0; };
struct HelloMsg { Header h; uint32_t mod_stamp = 0; uint8_t game = 0, got_yours = 0; char name[64]{}; };
struct PingMsg { Header h; uint32_t seq = 0; uint64_t sent_us = 0; };
struct StateMsg { Header h; uint32_t seq = 0; double pos[3]{}, vel[3]{}, yaw = 0; char map[48]{}; float montage_pos = 0, dilation = 1; uint32_t montage_id = 0; };
struct MontageMsg { Header h; uint32_t seq = 0; float pos = 0, rate = 1; char path[240]{}; };
#pragma pack(pop)

struct Event { int id = 0; unsigned char data[kSessionFailedSize]{}; };
constexpr ULONGLONG kUnavailableAfterMs = 30000, kHelloMs = 1000, kKeepAliveMs = 5000, kSilentMs = 15000, kPingMs = 2000, kFriendsMs = 10000;

Logger logger_ = nullptr;
Api api_;
void* mm_ = nullptr; void* user_ = nullptr; void* friends_ = nullptr; void* utils_ = nullptr; void* messages_ = nullptr;
int steam_ = 0; bool registered_ = false, unavailable_logged_ = false, faulted_ = false;
std::wstring why_[2];
ULONGLONG started_ = 0, next_attach_ = 0;
uint64_t me_ = 0; std::wstring me_name_; uint32_t app_ = 0;
uint64_t lobby_ = 0; bool owner_ = false;
ApiCall create_call_ = 0, join_call_ = 0; uint64_t join_target_ = 0, invite_after_create_ = 0, connect_lobby_ = 0;
uint64_t peer_ = 0; std::wstring peer_name_; int members_ = 0;
bool peer_hello_ = false, incompatible_ = false, connected_logged_ = false, other_mod_ = false;
std::wstring note_[2], route_, last_[2];
ULONGLONG next_hello_ = 0, next_ping_ = 0, next_members_ = 0, next_info_ = 0, next_friends_ = 0, next_publish_ = 0, last_heard_ = 0;
uint32_t ping_seq_ = 0, state_seq_ = 0, montage_seq_ = 0, remote_state_seq_ = 0; int rtt_ = -1;
uint32_t sent_ = 0, received_ = 0, callbacks_ = 0, dropped_ = 0, send_failures_ = 0;
int conn_state_ = -1;
std::vector<Friend> friends_list_; uint64_t friends_serial_ = 0;
std::deque<Montage> montages_in_;
// The peer's newest parts list (reliable) and pose (unreliable; NetPuppet orders them by its own sequence number).
std::vector<unsigned char> parts_in_, pose_in_; bool parts_new_ = false, pose_new_ = false; uint64_t pose_at_ = 0;
State remote_; uint64_t remote_at_ = 0; bool remote_valid_ = false;

SRWLOCK events_lock_ = SRWLOCK_INIT; std::deque<Event> events_;
SRWLOCK requests_lock_ = SRWLOCK_INIT; std::vector<std::pair<Request, uint64_t>> requests_;
SRWLOCK snapshot_lock_ = SRWLOCK_INIT; Snapshot snapshot_;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[900]{}; va_list arguments; va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments); va_end(arguments);
    logger_(text);
}
std::wstring Wide(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n); out.resize(static_cast<size_t>(n - 1)); return out;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr); out.resize(static_cast<size_t>(n - 1)); return out;
}
// The newest event shown on the page, in both languages (the page picks one).
void Say(const std::wstring& zh, const std::wstring& en) { last_[0] = zh; last_[1] = en; next_publish_ = 0; }
uint64_t NowUs() {
    static LARGE_INTEGER frequency{}; if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart / frequency.QuadPart * 1000000 + now.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}
uint32_t ModStamp() {
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const unsigned char*>(&__ImageBase) + __ImageBase.e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}
Identity For(uint64_t steam_id) { Identity id; id.type = kIdentitySteamId; id.size = 8; id.steam_id = steam_id; return id; }
std::wstring FriendName(uint64_t id) {
    const char* name = friends_ && api_.GetFriendPersonaName ? api_.GetFriendPersonaName(friends_, id) : nullptr;
    std::wstring wide = Wide(name);
    return wide.empty() ? std::to_wstring(id) : wide;
}

// ---- callbacks (the game's Steam thread: queue only) ----------------------------------------------------------------
class Listener final : public CallbackBase {
public:
    Listener(int id, int size) : id_(id), size_(size) {}
    void Run(void* param) override { Push(param); }
    void Run(void* param, bool, ApiCall) override { Push(param); }
    int GetCallbackSizeBytes() override { return size_; }
    int Id() const { return id_; }
private:
    void Push(const void* param) {
        if (!param) return;
        AcquireSRWLockExclusive(&events_lock_);
        if (events_.size() < 256) { events_.emplace_back(); events_.back().id = id_; std::memcpy(events_.back().data, param, static_cast<size_t>(size_)); }
        ReleaseSRWLockExclusive(&events_lock_);
    }
    int id_, size_;
};
Listener join_requested_(kJoinRequested, kJoinRequestedSize), lobby_enter_(kLobbyEnter, kLobbyEnterSize),
    chat_update_(kLobbyChatUpdate, kLobbyChatUpdateSize), lobby_created_(kLobbyCreated, kLobbyCreatedSize),
    session_request_(kSessionRequest, kSessionRequestSize), session_failed_(kSessionFailed, kSessionFailedSize);

// ---- attach ----------------------------------------------------------------------------------------------------------
template<class F> bool Export(const char* name, F& out) {
    out = reinterpret_cast<F>(GetProcAddress(api_.dll, name));
    if (!out) Logf("NET_STEAM missing export %s", name);
    return out != nullptr;
}
bool ResolveExports() {
    bool ok = true;
    ok &= Export("SteamAPI_GetHSteamUser", api_.GetHSteamUser);
    ok &= Export("SteamAPI_RegisterCallback", api_.RegisterCallback);
    ok &= Export("SteamAPI_SteamMatchmaking_v009", api_.Matchmaking);
    ok &= Export("SteamAPI_SteamUser_v023", api_.User);
    ok &= Export("SteamAPI_SteamFriends_v018", api_.Friends);
    ok &= Export("SteamAPI_SteamUtils_v011", api_.Utils);
    ok &= Export("SteamAPI_SteamNetworkingMessages_SteamAPI_v002", api_.Messages);
    ok &= Export("SteamAPI_SteamNetworkingUtils_SteamAPI_v004", api_.NetUtils);
    ok &= Export("SteamAPI_ISteamMatchmaking_CreateLobby", api_.CreateLobby);
    ok &= Export("SteamAPI_ISteamMatchmaking_JoinLobby", api_.JoinLobby);
    ok &= Export("SteamAPI_ISteamMatchmaking_LeaveLobby", api_.LeaveLobby);
    ok &= Export("SteamAPI_ISteamMatchmaking_InviteUserToLobby", api_.InviteUserToLobby);
    ok &= Export("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", api_.GetNumLobbyMembers);
    ok &= Export("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", api_.GetLobbyMemberByIndex);
    ok &= Export("SteamAPI_ISteamMatchmaking_SetLobbyData", api_.SetLobbyData);
    ok &= Export("SteamAPI_ISteamMatchmaking_GetLobbyData", api_.GetLobbyData);
    ok &= Export("SteamAPI_ISteamMatchmaking_GetLobbyOwner", api_.GetLobbyOwner);
    ok &= Export("SteamAPI_ISteamUser_GetSteamID", api_.GetSteamID);
    ok &= Export("SteamAPI_ISteamFriends_GetPersonaName", api_.GetPersonaName);
    ok &= Export("SteamAPI_ISteamFriends_GetFriendPersonaName", api_.GetFriendPersonaName);
    ok &= Export("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", api_.ActivateInviteDialog);
    ok &= Export("SteamAPI_ISteamFriends_GetFriendCount", api_.GetFriendCount);
    ok &= Export("SteamAPI_ISteamFriends_GetFriendByIndex", api_.GetFriendByIndex);
    ok &= Export("SteamAPI_ISteamFriends_GetFriendGamePlayed", api_.GetFriendGamePlayed);
    ok &= Export("SteamAPI_ISteamUtils_IsAPICallCompleted", api_.IsAPICallCompleted);
    ok &= Export("SteamAPI_ISteamUtils_GetAPICallResult", api_.GetAPICallResult);
    ok &= Export("SteamAPI_ISteamUtils_GetAppID", api_.GetAppID);
    ok &= Export("SteamAPI_ISteamUtils_IsOverlayEnabled", api_.IsOverlayEnabled);
    ok &= Export("SteamAPI_ISteamNetworkingMessages_SendMessageToUser", api_.SendMessageToUser);
    ok &= Export("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", api_.ReceiveMessagesOnChannel);
    ok &= Export("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", api_.AcceptSessionWithUser);
    ok &= Export("SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", api_.CloseSessionWithUser);
    ok &= Export("SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo", api_.GetSessionConnectionInfo);
    ok &= Export("SteamAPI_SteamNetworkingMessage_t_Release", api_.ReleaseMessage);
    ok &= Export("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", api_.InitRelayNetworkAccess);
    return ok;
}
void Unavailable(const wchar_t* zh, const wchar_t* en, const char* log) {
    steam_ = -1; why_[0] = zh; why_[1] = en;
    if (!unavailable_logged_) { unavailable_logged_ = true; Logf("NET_STEAM unavailable: %s", log); }
    next_publish_ = 0;
}
void JoinLobbyNow(uint64_t lobby);
// steam_api64.dll is delay-loaded by the game; Steam is ready once it has a user pipe.
void Attach(ULONGLONG now) {
    if (now < next_attach_) return;
    next_attach_ = now + (steam_ < 0 ? 5000 : 1000);
    const bool wegame = GameBuild::Current() == GameBuild::Id::WeGame;
    if (!api_.dll) {
        api_.dll = GetModuleHandleW(L"steam_api64.dll");
        if (!api_.dll) {
            if (wegame) Unavailable(L"WeGame 版没有 Steam 联机组件", L"The WeGame build has no Steam networking", "WeGame build, no steam_api64.dll");
            else if (now - started_ > kUnavailableAfterMs) Unavailable(L"游戏没有加载 Steam（steam_api64.dll）", L"The game has not loaded Steam (steam_api64.dll)", "steam_api64.dll not loaded");
            return;
        }
        if (!ResolveExports()) { Unavailable(L"这个 Steam 组件版本不对", L"Unexpected Steam DLL version", "exports missing"); next_attach_ = ULLONG_MAX; return; }
        Logf("NET_STEAM steam_api64.dll found at %p (%s build)", static_cast<void*>(api_.dll), wegame ? "WeGame" : "Steam");
    }
    if (!api_.GetHSteamUser()) {
        if (now - started_ > kUnavailableAfterMs) Unavailable(L"Steam 没有运行或没有登录", L"Steam is not running or not signed in", "SteamAPI_GetHSteamUser = 0");
        return;
    }
    mm_ = api_.Matchmaking(); user_ = api_.User(); friends_ = api_.Friends(); utils_ = api_.Utils(); messages_ = api_.Messages();
    void* net_utils = api_.NetUtils();
    if (!mm_ || !user_ || !friends_ || !utils_ || !messages_) {
        Logf("NET_STEAM interfaces mm=%p user=%p friends=%p utils=%p messages=%p", mm_, user_, friends_, utils_, messages_);
        Unavailable(L"取不到 Steam 联机接口", L"Steam networking interfaces unavailable", "interface accessor returned null");
        return;
    }
    if (!registered_) {
        for (Listener* l : {&join_requested_, &lobby_enter_, &chat_update_, &lobby_created_, &session_request_, &session_failed_}) api_.RegisterCallback(l, l->Id());
        registered_ = true;
    }
    if (net_utils) api_.InitRelayNetworkAccess(net_utils);
    me_ = api_.GetSteamID(user_); me_name_ = Wide(api_.GetPersonaName(friends_)); app_ = api_.GetAppID(utils_);
    steam_ = 1; why_[0].clear(); why_[1].clear(); next_attach_ = ULLONG_MAX; next_friends_ = 0;
    Logf("NET_STEAM ready me=%llu name=%s app=%u overlay=%d mod_stamp=%08x protocol=%u callbacks registered (333 504 506 513 1251 1252)",
         static_cast<unsigned long long>(me_), Utf8(me_name_).c_str(), app_, api_.IsOverlayEnabled(utils_) ? 1 : 0, ModStamp(), kProtocol);
    Say(L"Steam 已就绪", L"Steam is ready");
    if (connect_lobby_) { Logf("NET_STEAM +connect_lobby %llu from the command line", static_cast<unsigned long long>(connect_lobby_)); JoinLobbyNow(connect_lobby_); connect_lobby_ = 0; }
}

// ---- sending -----------------------------------------------------------------------------------------------------------
bool Send(const void* data, uint32_t bytes, bool reliable) {
    if (!peer_ || !messages_) return false;
    const int result = api_.SendMessageToUser(messages_, For(peer_), data, bytes, reliable ? kSendReliable : kSendUnreliable, reliable ? kControl : kStateChannel);
    if (result == 1) { ++sent_; return true; }
    if (++send_failures_ <= 10) Logf("NET_MSG send failed result=%d bytes=%u reliable=%d", result, bytes, reliable ? 1 : 0);
    return false;
}
void SendHello() {
    HelloMsg m; m.h.type = kHello; m.mod_stamp = ModStamp(); m.game = static_cast<uint8_t>(GameBuild::Current()); m.got_yours = peer_hello_ ? 1 : 0;
    strncpy_s(m.name, Utf8(me_name_).c_str(), _TRUNCATE);
    Send(&m, sizeof(m), true);
}
void SendPing() { PingMsg m; m.h.type = kPing; m.seq = ++ping_seq_; m.sent_us = NowUs(); Send(&m, sizeof(m), true); }

// ---- lobby -------------------------------------------------------------------------------------------------------------
void ResetPeer() {
    peer_ = 0; peer_name_.clear(); peer_hello_ = incompatible_ = connected_logged_ = other_mod_ = false; note_[0].clear(); note_[1].clear(); route_.clear();
    rtt_ = -1; conn_state_ = -1; remote_valid_ = false; remote_state_seq_ = 0; montages_in_.clear();
    parts_in_.clear(); pose_in_.clear(); parts_new_ = pose_new_ = false;
}
void RefreshMembers(ULONGLONG now) {
    next_members_ = now + 1000;
    if (!lobby_) { if (peer_) ResetPeer(); members_ = 0; return; }
    const int n = api_.GetNumLobbyMembers(mm_, lobby_);
    if (n <= 0) {
        Logf("NET_LOBBY %llu has no members for us any more; left", static_cast<unsigned long long>(lobby_));
        Say(L"房间已经不在了", L"The room is gone");
        if (peer_) api_.CloseSessionWithUser(messages_, For(peer_));
        lobby_ = 0; owner_ = false; members_ = 0; ResetPeer(); return;
    }
    members_ = n;
    uint64_t peer = 0;
    for (int i = 0; i < n && !peer; ++i) { const uint64_t id = api_.GetLobbyMemberByIndex(mm_, lobby_, i); if (id && id != me_) peer = id; }
    if (peer == peer_) return;
    if (peer_) {
        Logf("NET_LOBBY peer %llu (%s) left", static_cast<unsigned long long>(peer_), Utf8(peer_name_).c_str());
        Say(L"对方离开了房间：" + peer_name_, L"Left the room: " + peer_name_);
        api_.CloseSessionWithUser(messages_, For(peer_));
    }
    ResetPeer();
    if (!peer) return;
    peer_ = peer; peer_name_ = FriendName(peer); next_hello_ = now; last_heard_ = now;
    Logf("NET_LOBBY peer %llu (%s) in the room; members=%d", static_cast<unsigned long long>(peer), Utf8(peer_name_).c_str(), n);
    Say(L"对方进入房间：" + peer_name_ + L"，正在握手…", L"In the room: " + peer_name_ + L", handshaking…");
}
void Entered(uint64_t lobby, ULONGLONG now) {
    if (lobby_ == lobby) return;
    lobby_ = lobby; owner_ = api_.GetLobbyOwner(mm_, lobby) == me_;
    char stamp[16]{}; _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%08x", ModStamp());
    if (owner_) {
        api_.SetLobbyData(mm_, lobby, "pbz_proto", "1"); api_.SetLobbyData(mm_, lobby, "pbz_mod", stamp);
        api_.SetLobbyData(mm_, lobby, "pbz_game", GameBuild::Name(GameBuild::Current()));
        Logf("NET_LOBBY created %llu (friends only, 2 members); data written", static_cast<unsigned long long>(lobby));
        Say(L"房间已建立，可以邀请好友了", L"Room created; invite a friend");
    } else {
        const char* proto = api_.GetLobbyData(mm_, lobby, "pbz_proto"), *mod = api_.GetLobbyData(mm_, lobby, "pbz_mod");
        Logf("NET_LOBBY joined %llu owner=%llu pbz_proto=%s pbz_mod=%s (ours %s)", static_cast<unsigned long long>(lobby),
             static_cast<unsigned long long>(api_.GetLobbyOwner(mm_, lobby)), proto ? proto : "", mod ? mod : "", stamp);
        Say(L"已加入房间", L"Joined the room");
    }
    RefreshMembers(now);
    if (invite_after_create_ && owner_) {
        const bool ok = api_.InviteUserToLobby(mm_, lobby_, invite_after_create_);
        Logf("NET_LOBBY invite %llu after create ok=%d", static_cast<unsigned long long>(invite_after_create_), ok ? 1 : 0);
        Say(L"已邀请 " + FriendName(invite_after_create_) + L"，等对方在 Steam 聊天里接受", L"Invited " + FriendName(invite_after_create_) + L"; they accept it in Steam chat");
        invite_after_create_ = 0;
    }
}
void LeaveNow() {
    if (!lobby_) { Say(L"不在房间里", L"Not in a room"); return; }
    if (peer_) api_.CloseSessionWithUser(messages_, For(peer_));
    api_.LeaveLobby(mm_, lobby_);
    Logf("NET_LOBBY left %llu", static_cast<unsigned long long>(lobby_));
    lobby_ = 0; owner_ = false; members_ = 0; ResetPeer();
    Say(L"已离开房间", L"Left the room");
}
void JoinLobbyNow(uint64_t lobby) {
    if (!lobby) { Say(L"房间号不对", L"Bad room number"); return; }
    if (lobby == lobby_) { Say(L"已经在这个房间里", L"Already in this room"); return; }
    if (lobby_) LeaveNow();
    join_call_ = api_.JoinLobby(mm_, lobby); join_target_ = lobby;
    Logf("NET_LOBBY joining %llu call=%llu", static_cast<unsigned long long>(lobby), static_cast<unsigned long long>(join_call_));
    Say(L"正在加入房间…", L"Joining the room…");
}
void Host() {
    if (lobby_) { Say(L"已经在房间里", L"Already in a room"); return; }
    if (create_call_ || join_call_) { Say(L"上一个请求还没完成", L"The last request is still running"); return; }
    create_call_ = api_.CreateLobby(mm_, kLobbyFriendsOnly, 2);
    Logf("NET_LOBBY create requested call=%llu", static_cast<unsigned long long>(create_call_));
    Say(L"正在建立房间…", L"Creating a room…");
}
void RefreshFriends(ULONGLONG now) {
    next_friends_ = now + kFriendsMs;
    std::vector<Friend> list;
    const int count = api_.GetFriendCount(friends_, kFriendFlagImmediate);
    for (int i = 0; i < count && i < 2000; ++i) {
        const uint64_t id = api_.GetFriendByIndex(friends_, i, kFriendFlagImmediate);
        alignas(8) unsigned char info[32]{};  // FriendGameInfo_t: game id, ip, ports, lobby (24 bytes)
        if (!id || !api_.GetFriendGamePlayed(friends_, id, info)) continue;
        uint64_t game = 0, lobby = 0; std::memcpy(&game, info, 8); std::memcpy(&lobby, info + 16, 8);
        if ((game & 0xffffff) != app_) continue;
        list.push_back({id, lobby, FriendName(id)});
    }
    std::sort(list.begin(), list.end(), [](const Friend& a, const Friend& b) { return a.name < b.name; });
    const bool changed = list.size() != friends_list_.size() ||
        !std::equal(list.begin(), list.end(), friends_list_.begin(), [](const Friend& a, const Friend& b) { return a.id == b.id && a.lobby == b.lobby && a.name == b.name; });
    if (changed) { friends_list_ = std::move(list); ++friends_serial_; Logf("NET_FRIENDS %d friends, %zu in this game", count, friends_list_.size()); }
}

// ---- incoming ----------------------------------------------------------------------------------------------------------
void Handle(const unsigned char* data, int bytes, ULONGLONG now) {
    if (bytes < static_cast<int>(sizeof(Header))) { ++dropped_; return; }
    Header h; std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) { ++dropped_; return; }
    if (h.version != kProtocol) {
        if (!incompatible_) Logf("NET_MSG peer protocol %u, ours %u: incompatible", h.version, kProtocol);
        incompatible_ = true; note_[0] = L"对方的联机协议版本不同，请两边装同一版 MOD"; note_[1] = L"The peer speaks another protocol version; install the same mod build on both";
        return;
    }
    ++received_; last_heard_ = now;
    switch (h.type) {
    case kHello: {
        if (bytes < static_cast<int>(sizeof(HelloMsg))) return;
        HelloMsg m; std::memcpy(&m, data, sizeof(m)); m.name[63] = 0;
        if (!peer_hello_) {
            other_mod_ = m.mod_stamp != ModStamp();
            Logf("NET_MSG hello from %s mod_stamp=%08x (ours %08x%s) game=%s", m.name, m.mod_stamp, ModStamp(), other_mod_ ? ", differs" : "",
                 GameBuild::Name(static_cast<GameBuild::Id>(m.game)));
        }
        peer_hello_ = true;
        if (!m.got_yours) next_hello_ = now;  // they have not heard us yet: answer at once
        break;
    }
    case kPing: {
        if (bytes < static_cast<int>(sizeof(PingMsg))) return;
        PingMsg m; std::memcpy(&m, data, sizeof(m)); m.h.type = kPong; Send(&m, sizeof(m), true);
        break;
    }
    case kPong: {
        if (bytes < static_cast<int>(sizeof(PingMsg))) return;
        PingMsg m; std::memcpy(&m, data, sizeof(m));
        const int sample = static_cast<int>((NowUs() - m.sent_us) / 1000);
        if (sample >= 0 && sample < 60000) rtt_ = rtt_ < 0 ? sample : (rtt_ * 3 + sample) / 4;
        if (m.seq <= 3 || m.seq % 30 == 0) Logf("NET_MSG ping #%u rtt=%d ms (smoothed %d)", m.seq, sample, rtt_);
        break;
    }
    case kState: {
        if (bytes < static_cast<int>(sizeof(StateMsg))) return;
        StateMsg m; std::memcpy(&m, data, sizeof(m));
        if (remote_valid_ && m.seq <= remote_state_seq_) return;  // unreliable: older ones may come late
        remote_state_seq_ = m.seq; remote_valid_ = true; remote_at_ = now;
        std::memcpy(remote_.pos, m.pos, sizeof(m.pos)); std::memcpy(remote_.vel, m.vel, sizeof(m.vel)); remote_.yaw = m.yaw;
        std::memcpy(remote_.map, m.map, sizeof(m.map)); remote_.map[sizeof(remote_.map) - 1] = 0;
        remote_.montage_pos = m.montage_pos; remote_.dilation = m.dilation; remote_.montage_id = m.montage_id;
        break;
    }
    case kMontage: {
        if (bytes < static_cast<int>(offsetof(MontageMsg, path) + 1)) return;
        MontageMsg m; std::memcpy(&m, data, (std::min)(static_cast<size_t>(bytes), sizeof(m))); m.path[sizeof(m.path) - 1] = 0;
        if (montages_in_.size() >= 64) montages_in_.pop_front();
        montages_in_.push_back({m.path, m.pos, m.rate, now});
        break;
    }
    case kParts: case kPose: {
        std::vector<unsigned char>& into = h.type == kParts ? parts_in_ : pose_in_;
        into.assign(data + sizeof(Header), data + bytes);
        if (h.type == kParts) parts_new_ = true; else { pose_new_ = true; pose_at_ = now; }
        break;
    }
    default: ++dropped_; break;
    }
}
void Receive(ULONGLONG now) {
    for (int channel : {kControl, kStateChannel}) {
        Message* messages[32]{};
        const int n = api_.ReceiveMessagesOnChannel(messages_, channel, messages, 32);
        for (int i = 0; i < n; ++i) {
            Message* m = messages[i];
            if (!m) continue;
            if (m->peer.type == kIdentitySteamId && m->peer.steam_id == peer_ && peer_) Handle(static_cast<const unsigned char*>(m->data), m->size, now);
            else if (++dropped_ <= 10) Logf("NET_MSG dropped %d bytes from %llu (not the room's peer)", m->size, static_cast<unsigned long long>(m->peer.steam_id));
            api_.ReleaseMessage(m);
        }
    }
}
void DrainCallbacks(ULONGLONG now) {
    std::deque<Event> events;
    AcquireSRWLockExclusive(&events_lock_); events.swap(events_); ReleaseSRWLockExclusive(&events_lock_);
    for (const Event& e : events) {
        ++callbacks_;
        uint64_t a = 0, b = 0; std::memcpy(&a, e.data, 8); std::memcpy(&b, e.data + 8, 8);
        switch (e.id) {
        case kJoinRequested:  // GameLobbyJoinRequested_t: lobby, friend
            Logf("NET_CB 333 invite accepted in Steam: lobby=%llu from=%llu (%s)", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), Utf8(FriendName(b)).c_str());
            JoinLobbyNow(a);
            break;
        case kLobbyEnter: {  // LobbyEnter_t: lobby, permissions, locked, response (1: success)
            uint32_t response = 0; std::memcpy(&response, e.data + 16, 4);
            const bool ours = a == join_target_ || create_call_ != 0 || a == lobby_;
            Logf("NET_CB 504 lobby enter %llu response=%u%s", static_cast<unsigned long long>(a), response, ours ? "" : " (not ours, ignored)");
            if (ours && response == 1) { if (a == join_target_) { join_call_ = 0; join_target_ = 0; } Entered(a, now); }
            break;
        }
        case kLobbyChatUpdate: {  // LobbyChatUpdate_t: lobby, changed user, making change, state bits
            uint32_t bits = 0; std::memcpy(&bits, e.data + 24, 4);
            Logf("NET_CB 506 lobby %llu member %llu state=0x%x", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), bits);
            if (a == lobby_) RefreshMembers(now);
            break;
        }
        case kLobbyCreated: {  // LobbyCreated_t: result, lobby
            int32_t result = 0; std::memcpy(&result, e.data, 4);
            Logf("NET_CB 513 lobby created result=%d lobby=%llu", result, static_cast<unsigned long long>(b));
            if (result == 1 && b && create_call_) { create_call_ = 0; Entered(b, now); }
            break;
        }
        case kSessionRequest: {  // SteamNetworkingMessagesSessionRequest_t: identity
            Identity id; std::memcpy(&id, e.data, sizeof(id));
            if (lobby_) RefreshMembers(now);
            const bool accept = id.type == kIdentitySteamId && id.steam_id && id.steam_id == peer_;
            const bool done = accept && api_.AcceptSessionWithUser(messages_, id);
            Logf("NET_CB 1251 session request from %llu %s", static_cast<unsigned long long>(id.steam_id), accept ? (done ? "accepted" : "accept failed") : "ignored (not the room's peer)");
            break;
        }
        case kSessionFailed: {  // SteamNetworkingMessagesSessionFailed_t: SteamNetConnectionInfo_t
            int32_t reason = 0; std::memcpy(&reason, e.data + 180, 4);
            char debug[129]{}; std::memcpy(debug, e.data + 184, 128);
            Logf("NET_CB 1252 session failed peer=%llu reason=%d %s", static_cast<unsigned long long>(b), reason, debug);
            Say(L"和对方的连接断了，Steam 会自动重连", L"The link to the peer broke; Steam retries on its own");
            break;
        }
        default: break;
        }
    }
}
void PollCalls(ULONGLONG now) {
    bool failed = false;
    if (create_call_ && api_.IsAPICallCompleted(utils_, create_call_, &failed)) {
        alignas(8) unsigned char result[kLobbyCreatedSize]{}; bool io = false;
        const bool got = api_.GetAPICallResult(utils_, create_call_, result, kLobbyCreatedSize, kLobbyCreated, &io);
        int32_t code = 0; uint64_t lobby = 0; std::memcpy(&code, result, 4); std::memcpy(&lobby, result + 8, 8);
        Logf("NET_LOBBY create result got=%d io_failure=%d failed=%d result=%d lobby=%llu", got ? 1 : 0, io ? 1 : 0, failed ? 1 : 0, code, static_cast<unsigned long long>(lobby));
        create_call_ = 0;
        if (got && !io && code == 1 && lobby) Entered(lobby, now);
        else if (!lobby_) { Say(L"建立房间失败（结果 " + std::to_wstring(code) + L"）", L"Could not create a room (result " + std::to_wstring(code) + L")"); invite_after_create_ = 0; }
    }
    failed = false;
    if (join_call_ && api_.IsAPICallCompleted(utils_, join_call_, &failed)) {
        alignas(8) unsigned char result[kLobbyEnterSize]{}; bool io = false;
        const bool got = api_.GetAPICallResult(utils_, join_call_, result, kLobbyEnterSize, kLobbyEnter, &io);
        uint64_t lobby = 0; uint32_t response = 0; std::memcpy(&lobby, result, 8); std::memcpy(&response, result + 16, 4);
        Logf("NET_LOBBY join result got=%d io_failure=%d failed=%d lobby=%llu response=%u", got ? 1 : 0, io ? 1 : 0, failed ? 1 : 0, static_cast<unsigned long long>(lobby), response);
        const uint64_t target = join_target_;
        join_call_ = 0; join_target_ = 0;
        if (got && !io && response == 1) Entered(lobby ? lobby : target, now);
        else if (lobby_ != target) Say(L"加入房间失败（代码 " + std::to_wstring(response) + L"：2 不存在、4 已满）", L"Could not join (code " + std::to_wstring(response) + L": 2 gone, 4 full)");
    }
}
void ProcessRequests(ULONGLONG now) {
    std::vector<std::pair<Request, uint64_t>> requests;
    AcquireSRWLockExclusive(&requests_lock_); requests.swap(requests_); ReleaseSRWLockExclusive(&requests_lock_);
    for (const auto& [request, argument] : requests) {
        if (steam_ != 1) { Say(L"Steam 还没就绪", L"Steam is not ready"); continue; }
        switch (request) {
        case Request::Host: Host(); break;
        case Request::Overlay:
            if (!lobby_) { Say(L"先建立房间，再邀请", L"Create a room first"); break; }
            api_.ActivateInviteDialog(friends_, lobby_);
            Logf("NET_LOBBY invite overlay opened overlay_enabled=%d", api_.IsOverlayEnabled(utils_) ? 1 : 0);
            if (api_.IsOverlayEnabled(utils_)) Say(L"已打开 Steam 邀请窗口", L"Steam invite window opened");
            else Say(L"Steam 界面没有启用：在下面的好友列表里直接邀请", L"The Steam overlay is off: invite from the friend list below");
            break;
        case Request::Invite:
            if (!argument) break;
            if (!lobby_) { invite_after_create_ = argument; Host(); break; }
            Logf("NET_LOBBY invite %llu ok=%d", static_cast<unsigned long long>(argument), api_.InviteUserToLobby(mm_, lobby_, argument) ? 1 : 0);
            Say(L"已邀请 " + FriendName(argument) + L"，等对方在 Steam 聊天里接受", L"Invited " + FriendName(argument) + L"; they accept it in Steam chat");
            break;
        case Request::Join: JoinLobbyNow(argument); break;
        case Request::JoinFriend: {
            RefreshFriends(now);
            const auto it = std::find_if(friends_list_.begin(), friends_list_.end(), [&](const Friend& f) { return f.id == argument; });
            if (it == friends_list_.end() || !it->lobby) { Say(L"对方现在不在房间里，请对方建立房间", L"That friend is not in a room; ask them to create one"); break; }
            JoinLobbyNow(it->lobby);
            break;
        }
        case Request::Leave: LeaveNow(); break;
        case Request::Ping:
            if (!peer_hello_) { Say(L"还没有连上对方", L"Not connected yet"); break; }
            SendPing(); Say(L"已发送测试消息", L"Test message sent");
            break;
        case Request::Friends: RefreshFriends(now); break;
        }
    }
}
void LinkInfo(ULONGLONG now) {
    if (!peer_ || now < next_info_) return;
    next_info_ = now + 1000;
    alignas(8) unsigned char info[kSessionFailedSize]{};
    const int state = api_.GetSessionConnectionInfo(messages_, For(peer_), info, nullptr);
    int32_t flags = 0; std::memcpy(&flags, info + 440, 4);
    const bool relayed = (flags & 16) != 0;
    static const wchar_t* const kStates[] = {L"none", L"connecting", L"finding route", L"connected", L"closed by peer", L"problem"};
    route_ = (state >= 0 && state <= 5 ? kStates[state] : L"?");
    if (state == 3) route_ += relayed ? L", relayed" : L", direct";
    if (state != conn_state_) {
        char description[129]{}; std::memcpy(description, info + 312, 128);
        int32_t reason = 0; std::memcpy(&reason, info + 180, 4);
        Logf("NET_LINK state %d -> %d flags=0x%x relayed=%d reason=%d %s", conn_state_, state, flags, relayed ? 1 : 0, reason, description);
        conn_state_ = state;
    }
}
void Publish(ULONGLONG now) {
    if (now < next_publish_) return;
    next_publish_ = now + 100;
    Snapshot s;
    s.steam = steam_; s.why[0] = why_[0]; s.why[1] = why_[1]; s.me = me_name_; s.me_id = me_;
    s.lobby = lobby_; s.owner = owner_; s.busy = create_call_ || join_call_; s.members = members_;
    s.peer = peer_; s.peer_name = peer_name_;
    s.link = !peer_ ? 0 : incompatible_ ? 3 : peer_hello_ && now - last_heard_ < kSilentMs ? 2 : 1;
    s.other_mod = other_mod_; s.note[0] = note_[0]; s.note[1] = note_[1];
    if (remote_valid_) s.peer_map = remote_.map;
    s.route = route_; s.rtt_ms = rtt_; s.sent = sent_; s.received = received_; s.callbacks = callbacks_; s.dropped = dropped_;
    s.friends = friends_list_; s.friends_serial = friends_serial_;
    s.last[0] = last_[0]; s.last[1] = last_[1];
    AcquireSRWLockExclusive(&snapshot_lock_); snapshot_ = std::move(s); ReleaseSRWLockExclusive(&snapshot_lock_);
}
void Tick() {
    const ULONGLONG now = GetTickCount64();
    if (!started_) started_ = now;
    if (steam_ != 1) {
        Attach(now);
        if (steam_ != 1) { ProcessRequests(now); Publish(now); return; }
    }
    DrainCallbacks(now);
    PollCalls(now);
    ProcessRequests(now);
    if (now >= next_members_) RefreshMembers(now);
    if (now >= next_friends_) RefreshFriends(now);
    Receive(now);
    if (peer_ && !incompatible_) {
        if (now >= next_hello_) { SendHello(); next_hello_ = now + (peer_hello_ ? kKeepAliveMs : kHelloMs); }
        const bool live = peer_hello_ && now - last_heard_ < kSilentMs;
        if (live && !connected_logged_) {
            connected_logged_ = true; next_ping_ = now;
            Logf("NET_LINK connected to %s (%llu)", Utf8(peer_name_).c_str(), static_cast<unsigned long long>(peer_));
            Say(L"已连上 " + peer_name_, L"Connected to " + peer_name_);
        } else if (!live && connected_logged_ && peer_hello_) {
            connected_logged_ = false;
            Logf("NET_LINK nothing from the peer for %llu ms", static_cast<unsigned long long>(now - last_heard_));
            Say(L"对方没有响应", L"The peer stopped answering");
        }
        if (live && now >= next_ping_) { SendPing(); next_ping_ = now + kPingMs; }
    }
    LinkInfo(now);
    Publish(now);
}
// "+connect_lobby <id>": Steam starts the game this way when an invite is accepted while it is not running.
uint64_t CommandLineLobby() {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* at = line ? wcsstr(line, L"+connect_lobby") : nullptr;
    if (!at) return 0;
    at += wcslen(L"+connect_lobby");
    while (*at == L' ' || *at == L'\t' || *at == L'"') ++at;
    return _wcstoui64(at, nullptr, 10);
}
DWORD faults_ = 0;
}  // namespace

void Initialize(Logger logger) {
    logger_ = logger;
    connect_lobby_ = CommandLineLobby();
}
void OnGameThreadGuarded() {
    if (faulted_) return;
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (++faults_ >= 3) faulted_ = true;
        Logf("NET_STEAM fault %lu/3 code=0x%08lx%s", faults_, GetExceptionCode(), faulted_ ? "; online probe disabled" : "");
    }
}
void Ask(Request request, uint64_t argument) {
    AcquireSRWLockExclusive(&requests_lock_);
    if (requests_.size() < 32) requests_.emplace_back(request, argument);
    ReleaseSRWLockExclusive(&requests_lock_);
}
Snapshot Get() {
    AcquireSRWLockShared(&snapshot_lock_); Snapshot s = snapshot_; ReleaseSRWLockShared(&snapshot_lock_);
    return s;
}
bool PeerReady() { return steam_ == 1 && peer_ && peer_hello_ && !incompatible_; }
void SendState(const State& state) {
    if (!PeerReady()) return;
    StateMsg m; m.h.type = kState; m.seq = ++state_seq_;
    std::memcpy(m.pos, state.pos, sizeof(m.pos)); std::memcpy(m.vel, state.vel, sizeof(m.vel)); m.yaw = state.yaw;
    std::memcpy(m.map, state.map, sizeof(m.map)); m.map[sizeof(m.map) - 1] = 0;
    m.montage_pos = state.montage_pos; m.dilation = state.dilation; m.montage_id = state.montage_id;
    Send(&m, sizeof(m), false);
}
void SendMontage(const Montage& montage) {
    if (!PeerReady()) return;
    MontageMsg m; m.h.type = kMontage; m.seq = ++montage_seq_; m.pos = montage.pos; m.rate = montage.rate;
    strncpy_s(m.path, montage.path.c_str(), _TRUNCATE);
    Send(&m, static_cast<uint32_t>(offsetof(MontageMsg, path) + std::strlen(m.path) + 1), true);
}
bool SendBlob(uint8_t type, const std::vector<unsigned char>& payload, bool reliable) {
    if (!PeerReady() || payload.size() > 400000) return false;
    std::vector<unsigned char> message(sizeof(Header) + payload.size());
    Header h; h.type = type; std::memcpy(message.data(), &h, sizeof(h));
    if (!payload.empty()) std::memcpy(message.data() + sizeof(h), payload.data(), payload.size());
    return Send(message.data(), static_cast<uint32_t>(message.size()), reliable);
}
bool SendParts(const std::vector<unsigned char>& payload) { return SendBlob(kParts, payload, true); }
bool SendPose(const std::vector<unsigned char>& payload) { return SendBlob(kPose, payload, false); }
bool TakeParts(std::vector<unsigned char>& payload) {
    if (!parts_new_ || !PeerReady()) return false;
    parts_new_ = false; payload = parts_in_; return true;
}
bool TakePose(std::vector<unsigned char>& payload, uint64_t& received_ms) {
    if (!pose_new_ || !PeerReady()) return false;
    pose_new_ = false; payload = pose_in_; received_ms = pose_at_; return true;
}
bool TakeMontage(Montage& montage) {
    if (montages_in_.empty()) return false;
    montage = std::move(montages_in_.front()); montages_in_.pop_front(); return true;
}
uint32_t PathId(const char* path) {
    uint32_t hash = 2166136261u;  // FNV-1a
    for (const char* c = path; c && *c; ++c) { hash ^= static_cast<unsigned char>(*c); hash *= 16777619u; }
    return path && *path ? (hash ? hash : 1) : 0;
}
bool LatestState(State& state, uint64_t& received_ms) {
    if (!remote_valid_ || !PeerReady()) return false;
    state = remote_; received_ms = remote_at_; return true;
}
}
