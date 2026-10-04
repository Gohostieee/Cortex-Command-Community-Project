#include "MultiplayerMan.h"
#include "MultiplayerWorld.h"

#include "MultiplayerTransport.h"
#include "MultiplayerInput.h"
#include "MultiplayerRelay.h"
#include "ActivityMan.h"
#include "AudioMan.h"
#include "CameraMan.h"
#include "ConsoleMan.h"
#include "FrameMan.h"
#include "GameActivity.h"
#include "LuaMan.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SceneMan.h"
#include "SettingsMan.h"
#include "SoundContainer.h"
#include "SoundSet.h"
#include "UInputMan.h"
#include "WindowMan.h"
#include "GameVersion.h"
#include "GUI/imgui/imgui.h"
#include "glad/gl.h"
#include "raylib/rlgl.h"
#include "lz4.h"


#include <charconv>
#include <cstdlib>

#include <random>
#include <unordered_map>
#include <fstream>
#include <SDL3_image/SDL_image.h>
#include "System.h"
#include "DataModule.h"
#include "Actor.h"
#include "MOSRotating.h"
#include "MovableMan.h"
#include "SceneEditorGUI.h"
#include "MultiplayerMenuGUI.h"
#include "MenuMan.h"
#include "PerformanceMan.h"

using namespace RTE;
using namespace RTE::MP;
namespace {
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t NowMicros() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t Token() { static std::mt19937_64 random(std::random_device{}()); uint64_t value; do value = random(); while (!value); return value; }
bool Address(const std::string& text, std::string& host, uint16_t& port, uint16_t defaultPort = 8000) {
	const auto colon = text.find(':'); host = text.substr(0, colon); port = defaultPort;
	if (host.empty() || host.size() > 253 || host.find_first_of(" \t\r\n/") != std::string::npos) return false;
	if (colon != std::string::npos) { unsigned number = 0; const auto* start = text.data() + colon + 1; const auto parsed = std::from_chars(start, text.data() + text.size(), number); if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || number == 0 || number > 65535) return false; port = static_cast<uint16_t>(number); }
	return true;
}
bool AssetPath(const std::string& path) { return !path.empty() && path.size() <= 240 && path.find("..") == std::string::npos && path.find(':') == std::string::npos && path.front() != '/' && path.front() != '\\' && path.find(".rte/") != std::string::npos; }
}

struct MultiplayerMan::Impl {
	enum class Mode { Idle, Host, Connecting, Client, Reconnecting };
	struct Player {
		std::string Address, Name;
		uint64_t Token = 0, ReservedUntil = 0, LastFrame = 0, LastAck = 0, LastChat = 0;
		bool Connected = false, Ready = false;
		bool AudioReady = false, ReplayLoops = true;
		bool TextActive = false, SmokeTextSeen = false;
		bool SmokeRadialKeyboardSeen = false, SmokeRadialMouseSeen = false;
		bool SmokeQCameraSeen = false, SmokeQCameraMoved = false;
		float SmokeQTargetX = 0, SmokeQTargetY = 0;
		std::string Text;
		std::unordered_map<int, AudioMan::NetworkSoundData> Loops;
		uint8_t Team = 0;
		uint16_t ViewWidth = 960, ViewHeight = 540;
		InputReceiver Inputs;
		uint32_t FrameID = 0, AckID = 0;
		double Budget = 0;
		std::unordered_set<uint64_t> WorldResources;
		std::unordered_map<uint64_t, uint64_t> PendingResources;
		std::deque<std::pair<std::vector<uint8_t>, Delivery>> WorldPackets;
		MP::World::ResourceWindow ResourceFlight;
		std::unordered_map<uint64_t, uint32_t> ResourceMessages;
		std::unordered_map<uint32_t, size_t> QueuedResourceChunks;
		uint32_t WorldMessage = 0;
		uint64_t LastWorld = 0;
		bool SnapshotStarted = false;
		bool SceneSent = false;
		std::deque<uint64_t> SceneQueue;
	};
	Mode State = Mode::Idle;
	Transport Net;
	std::array<Player, MaxPlayers> Players;
	std::unordered_map<std::string, uint64_t> Pending;
	std::map<std::string, std::string> Discovered;
	std::unordered_map<int, std::unique_ptr<SoundContainer>> Sounds;
	uint64_t Session = 0, ReconnectToken = 0, ConnectStarted = 0, NextRetry = 0, LastUpdate = Now(), LastInputSent = 0, LastFrameReceived = 0;
	uint32_t Epoch = 0;
	int LocalSlot = 0;
	bool UI = false, Playing = false, Launch = false, Controls = true;
	bool Played = false;
	bool WarmingGuests = false;
	uint64_t MatchEndedAt = 0;
	std::string Notice;
	std::unique_ptr<MultiplayerMenuGUI> Menu;
	bool TextActive = false;
	std::string ServerAddress, Error, RoomName, ActivityName, SceneName;
	std::vector<std::string> Chat;
	uint64_t LastChatSent = 0;
	char Name[32] = "Player", Room[64] = "Cortex room", HostAddress[256] = "127.0.0.1:8000", Password[64] = "";
	char ServiceAddress[256] = "", JoinCode[32] = "";
	bool Online = true;
	std::string DefaultService;
	Impl() {
		std::string endpoint;
		std::ifstream bundled("MultiplayerService.txt"); std::getline(bundled, DefaultService);
		if (!DefaultService.empty() && DefaultService.back() == '\r') DefaultService.pop_back();
		std::ifstream personal(System::GetUserdataDirectory() + "MultiplayerService.txt"); std::getline(personal, endpoint);
		if (!endpoint.empty() && endpoint.back() == '\r') endpoint.pop_back();
		if (endpoint.empty()) endpoint = DefaultService;
		if (const char* configured = std::getenv("CCCP_MP_SERVICE")) endpoint = configured;
		std::string host; uint16_t port;
		if (Address(endpoint, host, port, 8001)) std::snprintf(ServiceAddress, sizeof(ServiceAddress), "%s", endpoint.c_str());
		if (const char* verify = std::getenv("CCCP_MPSMOKE_CURSOR"); verify && std::string(verify) == "1") { CursorVerification = UI = true; std::ofstream("build-mp/cursor-smoke.log") << "START\n"; }
	}
	void SaveService() { if (!Smoke && !std::getenv("CCCP_MP_SERVICE")) { std::ofstream file(System::GetUserdataDirectory() + "MultiplayerService.txt"); file << ServiceAddress << '\n'; } }
	int Port = 8000, Quality = 0, BandwidthMbps = 24, Difficulty = 50, Gold = 5000;
	int ActivityIndex = 0, SceneIndex = 0;
	bool Fog = false, Deploy = false, ClearOrbit = false;
	uint8_t AvailableTeams = 15, CPUTeam = 0;
	std::array<std::string, 4> Tech;
	std::vector<std::string> Factions;
	std::vector<GameActivity*> Activities;
	std::vector<Scene*> Scenes;
	Input LocalInput;
	float MouseRemainderX = 0, MouseRemainderY = 0;
	MultiplayerWorld World;
	MP::World::Assembler SnapshotAssembly, ResourceAssembly{30000};
	std::deque<MP::World::Snapshot> PendingWorlds;
	std::unordered_set<uint64_t> MissingResources, ReportedResources;
	std::vector<uint64_t> SceneManifest;
	std::unordered_set<uint64_t> SceneMissing;
	bool SceneReceived = false;
	uint64_t SceneReceivedAt = 0;
	MP::World::ResourceRequests ResourceRequests;
	void ResetSceneTransfer() { SceneManifest.clear(); SceneMissing.clear(); SceneReceived = false; ResourceRequests.Reset(); }
	uint64_t LastRender = 0;
	uint64_t LastResourceRequest = 0;
	unsigned Texture = 0;
	int TextureWidth = 0, TextureHeight = 0;
	uint64_t BytesReceived = 0;
	float FPS = 0;
	bool Smoke = false, SmokeCapture = false;
	bool SmokeWorldLoss = false;
	bool SmokeCombatStress = false;
	bool SmokeEncounter = false;
	bool SmokeNativeBaseline = false;
	uint64_t SmokeEncounterStart = 0, SmokeEncounterLastUpdate = 0, SmokeEncounterGap = 0;
	uint64_t SmokeEncounterUpdates = 0;
	size_t SmokeEncounterPeakNodes = 0, SmokeEncounterPeakTrails = 0, SmokeEncounterPeakBytes = 0;
	size_t SmokeEncounterPeakPacked = 0;
	uint64_t SmokeEncounterWireBytes = 0, SmokeEncounterWireUpdates = 0;
	uint64_t SmokeCaptureStarted = 0;
	std::vector<uint64_t> SmokeCaptureTimes;
	std::array<uint64_t, 4> SmokeCaptureParts{};
	std::unordered_set<uint64_t> SmokeEncounterSoldiers, SmokeEncounterFired, SmokeEncounterDamaged;
	uint64_t SmokeLastExplosion = 0, SmokeExplosions = 0, SmokeStressUpdates = 0, SmokeMaxUpdateGap = 0;
	uint64_t SmokeAutoHeld = 0;
	bool SmokeRadialKeyboardCaptured = false, SmokeRadialMouseCaptured = false;
	bool SmokeQCameraCaptured = false;
	std::string SmokeCaptureName;
	bool SmokeDeployment = false;
	bool SmokeDeploymentFailed = false;
	uint64_t SmokeDeploymentStart = 0, SmokeDeploymentCount = 0;
	bool CursorVerification = false;
	size_t CursorStage = 0;
	int CursorFrames = 0;
	int CursorPreparedStage = -1;
	struct CursorCase { const char* Name; Mode State; bool UI, Playing, Visible; int Page = 0; };
	static constexpr std::array<CursorCase, 24> CursorCases{{
		{"menu", Mode::Idle, true, false, true}, {"main-menu", Mode::Idle, false, false, false},
		{"host-lobby", Mode::Host, true, false, true}, {"host-gameplay", Mode::Host, false, true, false},
		{"host-session", Mode::Host, true, true, true}, {"host-resume", Mode::Host, false, true, false},
		{"guest-lobby", Mode::Client, true, false, true}, {"guest-gameplay", Mode::Client, false, true, false},
		{"guest-session", Mode::Client, true, true, true}, {"connecting", Mode::Connecting, false, false, true},
		{"reconnecting", Mode::Reconnecting, false, false, true}, {"closed-menu", Mode::Idle, false, false, false},
		{"create-menu", Mode::Idle, true, false, true, 1}, {"connection-settings", Mode::Idle, true, false, true, 2},
		{"host-rules", Mode::Host, true, false, true, 3}, {"host-factions", Mode::Host, true, false, true, 4},
		{"guest-rules", Mode::Client, true, false, true, 3}, {"room-chat", Mode::Host, true, false, true, 5},
		{"post-match-lobby", Mode::Host, true, false, true}, {"stream-settings", Mode::Idle, true, false, true, 3},
		{"host-session-chat", Mode::Host, true, true, true, 5}, {"guest-session-chat", Mode::Client, true, true, true, 5},
		{"close-room-confirmation", Mode::Host, true, false, true, 6}, {"leave-room-confirmation", Mode::Client, true, true, true, 6}
	}};
	bool SmokeReconnected = false, SmokeCapturedHost = false;
	bool SmokeSessionChecked = false;
	int SmokeStage = 0;
	int SmokeRejoins = 0;
	int SmokeGuests = 1;
	std::string SmokeRole;
	int SmokeCaptureDelay = 0;
	uint64_t SmokeStarted = 0, SmokeStageTime = 0, Presented = 0, AudioReceived = 0;
	uint64_t SmokeSecondStart = 0;
	uint64_t SmokeReadySent = 0;
	uint64_t SmokeLastStats = 0;
	uint64_t SmokeRenderStatsTime = 0, SmokeRenderStatsCount = 0, SmokeStateStatsCount = 0;
	std::array<float, 4> SmokeActorStartX{};
	std::array<bool, 4> SmokeActorSeen{}, SmokeActorMoved{}, SmokeFireSeen{};
	std::array<bool, 4> SmokeGUIKeySeen{};
	bool SmokeChatSent = false;
	bool SmokeTextSent = false;
	std::unique_ptr<SoundContainer> SmokeLoop;
	int SmokeLoopPlays = 0;
	int SmokeLoopChannel = -1;
	bool SmokeLoopStopped = false;
	void Verify(const std::string& message) { if (Smoke) { std::ofstream log("build-mp/" + SmokeRole + "-smoke.log", std::ios::app); log << message << '\n'; } }
	void SmokeTick();
	void DeploymentTick();
	void EncounterTick();

	void ClearSounds() { for (auto& [channel, sound]: Sounds) sound->Stop(); Sounds.clear(); }
	static void ClearWorldPackets(Player& player) {
		player.WorldPackets.clear(); player.ResourceFlight.Reset(); player.ResourceMessages.clear(); player.QueuedResourceChunks.clear(); player.SnapshotStarted = false;
		player.AckID = 0;
		player.SceneSent = false; player.SceneQueue.clear();
	}
	void Stop() {
		Menu.reset();
		if (TextActive && State != Mode::Host) SDL_StopTextInput(g_WindowMan.GetWindow()); TextActive = false;
		if (SmokeLoop) { SmokeLoop->Stop(); SmokeLoop.reset(); }
		if (State == Mode::Host) { MP::Writer leave(Kind::Leave, Session, Epoch); for (size_t i = 1; i < Players.size(); ++i) if (Players[i].Connected) Net.Send(Players[i].Address, leave.Data, Delivery::Control); }
		if (State == Mode::Client || State == Mode::Connecting || State == Mode::Reconnecting) { MP::Writer leave(Kind::Leave, Session, Epoch); Net.Send(ServerAddress, leave.Data, Delivery::Control); }
		Net.Stop();
		for (auto& player: Players) { player = Player(); }
		Pending.clear(); Chat.clear(); ClearSounds(); World.Reset(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); LocalInput = {}; Session = ReconnectToken = 0; Epoch = 0; MouseRemainderX = MouseRemainderY = 0;
		Texture = 0; TextureWidth = TextureHeight = 0;
		State = Mode::Idle; Playing = Launch = Played = false; MatchEndedAt = 0; Notice.clear(); g_AudioMan.SetMultiplayerMode(false); g_UInputMan.TrapMousePos(false);
		WarmingGuests = false;
		g_AudioMan.SetStreamListener(Vector(), false);
		for (int player = 1; player < 4; ++player) g_UInputMan.ClearRemoteInput(player);
	}
	void Advertise() {
		if (State != Mode::Host) return;
		MP::Writer writer(Kind::Announcement); writer.Text(RoomName, 63); uint8_t count = 0; for (const auto& player: Players) if (player.Connected) ++count;
		writer.U8(count); writer.U8(Playing); Net.Advertise(writer.Data);
	}
	void Lobby() {
		MP::Writer writer(Kind::Lobby, Session, Epoch); writer.U8(Playing); writer.Text(RoomName, 63); writer.Text(ActivityName); writer.Text(SceneName);
		writer.U8(static_cast<uint8_t>(Difficulty)); writer.U32(static_cast<uint32_t>(Gold)); writer.U8(Fog); writer.U8(Deploy); writer.U8(ClearOrbit); writer.U8(AvailableTeams); writer.U8(CPUTeam);
		for (const auto& tech: Tech) writer.Text(tech);
		for (const auto& player: Players) { writer.U8(player.Token != 0); writer.U8(player.Connected); writer.U8(player.Ready); writer.U8(player.Team); writer.Text(player.Name, 31); }
		for (size_t i = 1; i < Players.size(); ++i) if (Players[i].Connected) Net.Send(Players[i].Address, writer.Data, Delivery::Control);
		Advertise();
	}
	void Reject(const std::string& address, const std::string& message) { MP::Writer writer(Kind::Reject); writer.Text(message); Net.Send(address, writer.Data, Delivery::Control); Pending[address] = Now() - 4500; }
	void Welcome(int slot) {
		MP::Writer writer(Kind::Welcome, Session, Epoch); writer.U8(static_cast<uint8_t>(slot)); writer.U64(Players[slot].Token); writer.Text(c_GameVersion.str());
		Net.Send(Players[slot].Address, writer.Data, Delivery::Control); Lobby();
	}
	bool Host() {
		std::string service; uint16_t servicePort = 8001;
		if (Online && !Address(ServiceAddress, service, servicePort, 8001)) { Error = "Set the room service address under Connection settings."; return false; }
		if (!Online && (Port < 1 || Port > 65535)) { Error = "Choose a UDP port from 1 to 65535."; return false; }
		Stop(); if (!Net.Start(!Online, static_cast<uint16_t>(Port), Online ? "" : Password, Error)) return false;
		if (Online && !Net.ConnectRelay(true, service, servicePort, "", Password, Error)) { Net.Stop(); return false; }
		if (Online) SaveService();
		State = Mode::Host; Session = Token(); RoomName = Room[0] ? Room : "Cortex room";
		Players[0].Name = Name[0] ? Name : "Host"; Players[0].Token = Token(); Players[0].Connected = Players[0].Ready = true;
		Error.clear(); UI = true; LoadActivities(); Lobby(); return true;
	}
	bool Join(bool retry = false) {
		std::string host; uint16_t port;
		if (Online) { if (!Address(ServiceAddress, host, port, 8001)) { Error = "Set the room service address under Connection settings."; return false; } if (Relay::NormalizeCode(JoinCode).empty()) { Error = "Enter the ten-character room code from your host."; return false; } }
		else if (!Address(HostAddress, host, port)) { Error = "Enter a hostname or IPv4 address, optionally followed by :port."; return false; }
		if (!retry) { Stop(); ConnectStarted = Now(); }
		else if (!Online) Net.Stop();
		if (Online && retry) { if (!Net.ReconnectRelay(Error)) return false; }
		else { if (!Net.Start(false, 0, "", Error) || !(Online ? Net.ConnectRelay(false, host, port, JoinCode, Password, Error) : Net.Connect(host, port, Password, Error))) return false; }
		if (Online) SaveService();
		State = retry ? Mode::Reconnecting : Mode::Connecting; NextRetry = Now() + 3000; UI = true; return true;
	}
	void Hello(const std::string& address) {
		ServerAddress = address; MP::Writer writer(Kind::Hello); writer.Text(Name[0] ? Name : "Player", 31); writer.U64(ReconnectToken); writer.Text(c_GameVersion.str()); writer.U16(uint16_t(std::clamp(g_WindowMan.GetResX(), 320, 3840))); writer.U16(uint16_t(std::clamp(g_WindowMan.GetResY(), 180, 2160))); Net.Send(address, writer.Data, Delivery::Control);
	}
	void LoadActivities() {
		Factions = {"-All-"}; for (int i = 0; i < g_PresetMan.GetTotalModuleCount(); ++i) if (g_PresetMan.GetDataModule(i)->IsFaction()) Factions.push_back(g_PresetMan.GetDataModuleName(i));
		Activities.clear(); std::list<Entity*> entities; g_PresetMan.GetAllOfType(entities, "GameActivity");
		for (auto* entity: entities) if (auto* activity = dynamic_cast<GameActivity*>(entity); activity && activity->GetPresetName() != "None" && activity->GetMaxPlayerSupport() > 1) Activities.push_back(activity);
		std::sort(Activities.begin(), Activities.end(), [](const auto* a, const auto* b) { return a->GetPresetName() < b->GetPresetName(); }); ActivityIndex = SceneIndex = 0; LoadScenes();
	}
	void LoadScenes() {
		Scenes.clear(); SceneIndex = 0; if (Activities.empty()) return;
		std::list<Entity*> entities; g_PresetMan.GetAllOfType(entities, "Scene");
		for (auto* entity: entities) if (auto* scene = dynamic_cast<Scene*>(entity); scene && Activities[ActivityIndex]->SceneIsCompatible(scene) && !scene->IsSavedGameInternal() && !scene->IsMetagameInternal() && (scene->GetMetasceneParent().empty() || g_SettingsMan.ShowMetascenes())) Scenes.push_back(scene);
		std::sort(Scenes.begin(), Scenes.end(), [](const auto* a, const auto* b) { return a->GetPresetName() < b->GetPresetName(); });
		ActivityName = Activities[ActivityIndex]->GetPresetName(); SceneName = Scenes.empty() ? "" : Scenes.front()->GetPresetName();
		const auto* activity = Activities[ActivityIndex]; Fog = activity->GetDefaultFogOfWar(); Deploy = activity->GetDefaultDeployUnits(); ClearOrbit = activity->GetDefaultRequireClearPathToOrbit(); Gold = std::clamp(activity->GetDefaultGoldMediumDifficulty(), 0, 1000000);
		CPUTeam = static_cast<uint8_t>(activity->GetCPUTeam() + 1); AvailableTeams = 0; std::vector<uint8_t> humanTeams;
		for (int team = 0; team < 4; ++team) { Tech[team] = activity->GetTeamTech(team).empty() ? "-All-" : activity->GetTeamTech(team); if (activity->TeamActive(team) && team + 1 != CPUTeam) { AvailableTeams |= uint8_t(1 << team); humanTeams.push_back(static_cast<uint8_t>(team)); } }
		for (size_t i = 0; i < Players.size(); ++i) if (!humanTeams.empty()) Players[i].Team = humanTeams[CPUTeam ? 0 : i % humanTeams.size()];
	}
	void SettingsChanged() { for (size_t i = 1; i < Players.size(); ++i) Players[i].Ready = false; Lobby(); }
	void SendReady(bool ready, uint8_t team) { MP::Writer writer(Kind::Ready, Session, Epoch); writer.U8(ready); writer.U8(team); Net.Send(ServerAddress, writer.Data, Delivery::Control); }
	void AddChat(int player, const std::string& message) {
		Chat.push_back(Players[player].Name + ": " + message); if (Chat.size() > 64) Chat.erase(Chat.begin());
		Verify("CHAT: " + message);
		if (State == Mode::Host) { MP::Writer writer(Kind::Chat, Session, Epoch); writer.U8(static_cast<uint8_t>(player)); writer.Text(message, 192); for (int i = 1; i < 4; ++i) if (Players[i].Connected) Net.Send(Players[i].Address, writer.Data, Delivery::Control); }
	}
	bool StartGame() {
		if (const auto blocked = StartBlock(); !blocked.empty()) { Error = blocked; return false; }
		auto* game = dynamic_cast<GameActivity*>(Activities[ActivityIndex]->Clone()); game->ClearPlayers(false);
		if (game->GetCPUTeam() >= 0) game->SetCPUTeam(game->GetCPUTeam());
		for (int i = 0; i < 4; ++i) if (Players[i].Connected) game->AddPlayer(i, true, Players[i].Team, 0);
		game->SetDifficulty(Difficulty); game->SetStartingGold(Gold); game->SetFogOfWarEnabled(Fog); game->SetRequireClearPathToOrbit(ClearOrbit);
		for (int team = 0; team < 4; ++team) game->SetTeamTech(team, Tech[team]);
		if (game->GetCPUTeam() >= 0) { const int cpuTeam = game->GetCPUTeam(); for (const auto& player: Players) if (player.Connected && player.Team == cpuTeam) { delete game; Error = "That team belongs to the AI. Choose another team."; return false; } }
		g_SceneMan.SetSceneToLoad(Scenes[SceneIndex], true, Deploy); g_LuaMan.FileCloseAll(); g_ActivityMan.SetStartActivity(game); g_ActivityMan.SetRestartActivity();
		World.Reset(); ResetSceneTransfer(); ++Epoch; Playing = Launch = WarmingGuests = true; UI = false; Error.clear(); Notice.clear(); MatchEndedAt = 0;
		if (Menu) Menu->Hide();
		for (auto& player: Players) { player.Inputs.Reset(); ClearWorldPackets(player); player.WorldResources.clear(); player.PendingResources.clear(); player.LastWorld = 0; player.LastFrame = player.LastAck = Now(); player.Loops.clear(); player.AudioReady = false; player.ReplayLoops = true; player.Text.clear(); player.TextActive = false; }
		g_AudioMan.ClearSoundEvents(-1); g_AudioMan.SetMultiplayerMode(true); Lobby(); return true;
	}
	void ReturnToLobby() {
		if (State != Mode::Host || !Playing) return;
		Playing = Launch = WarmingGuests = false; Played = UI = true; MatchEndedAt = 0; ++Epoch;
		Notice = "Round complete. Ready up for the next match."; Error.clear();
		g_AudioMan.SetMultiplayerMode(false); g_AudioMan.ClearSoundEvents(-1); g_ActivityMan.PauseActivity();
		g_UInputMan.TrapMousePos(false);
		for (size_t i = 0; i < Players.size(); ++i) {
			auto& player = Players[i];
			player.Ready = i == 0; player.Inputs.Reset(); ClearWorldPackets(player); player.WorldResources.clear(); player.PendingResources.clear(); player.Text.clear(); player.TextActive = player.AudioReady = false; player.Loops.clear(); player.ReplayLoops = true;
			if (i > 0) g_UInputMan.ClearRemoteInput(static_cast<int>(i));
		}
		g_MenuMan.SetMultiplayerMenuBackground(true);
		Lobby();
	}
	std::string StartBlock() const;
	MultiplayerMenuGUI::View MenuView() const;
	void UpdateMenu();
	void HandleActivityExit() { if (State == Mode::Host && Playing && !g_ActivityMan.ActivitySetToRestart()) ReturnToLobby(); }
	void Receive(const TransportEvent& event);
	void HandleAudio(MP::Reader& reader);
	void SendAudio(int player);
	void Tick();
	void SampleInput();
	bool QueueWorld(Player& player, Kind kind, std::span<const uint8_t> payload, Delivery delivery, uint64_t resource = 0);
	bool QueueResource(Player& player, uint64_t id);
	void ReceiveWorld(Kind kind, MP::Reader& reader);
	void PresentWorld(MP::World::Snapshot snapshot);
	void PresentPendingWorlds();
	bool VerifyWorldFlow(std::ostream& log);
	void Draw();
};

MultiplayerMan::MultiplayerMan(): m_Impl(std::make_unique<Impl>()) {}
MultiplayerMan::~MultiplayerMan() = default;
void MultiplayerMan::Open() {
	m_Impl->UI = true; g_UInputMan.TrapMousePos(false);
	if (!m_Impl->Playing) g_MenuMan.SetMultiplayerMenuBackground(true);
}
void MultiplayerMan::Update() { m_Impl->Tick(); }
void MultiplayerMan::UpdateMenu() { if (!m_Impl->CursorVerification) m_Impl->UpdateMenu(); }
void MultiplayerMan::DrawUI() { m_Impl->Draw(); }
void MultiplayerMan::Stop() { m_Impl->Stop(); }
void MultiplayerMan::HandleActivityExit() { m_Impl->HandleActivityExit(); }
bool MultiplayerMan::StartRoom(bool host, const std::string& address, bool smokeTest) {
	auto& impl = *m_Impl; impl.UI = true; impl.Smoke = smokeTest;
	impl.SmokeWorldLoss = smokeTest && std::getenv("CCCP_MPSMOKE_WORLD_LOSS") && std::string(std::getenv("CCCP_MPSMOKE_WORLD_LOSS")) == "1";
	impl.SmokeCombatStress = smokeTest && std::getenv("CCCP_MPSMOKE_COMBAT");
	impl.SmokeEncounter = smokeTest && std::getenv("CCCP_MPSMOKE_ENCOUNTER");
	impl.SmokeNativeBaseline = impl.SmokeEncounter && std::getenv("CCCP_MPSMOKE_BASELINE");
	impl.Online = std::getenv("CCCP_MP_SERVICE") != nullptr;
	if (!host && impl.Online) std::snprintf(impl.JoinCode, sizeof(impl.JoinCode), "%s", address.c_str());
	if (smokeTest) {
		impl.SmokeDeployment = std::getenv("CCCP_MPSMOKE_DEPLOYMENT") != nullptr;
		impl.SmokeRole = host ? "host" : "client";
		if (const char* role = std::getenv("CCCP_MPSMOKE_ROLE"); role && (std::string(role) == "host" || std::string(role) == "client" || std::string(role) == "client2" || std::string(role) == "client3")) impl.SmokeRole = role;
		if (const char* count = std::getenv("CCCP_MPSMOKE_GUESTS"); count && count[0] >= '1' && count[0] <= '3' && !count[1]) impl.SmokeGuests = count[0] - '0';
		std::snprintf(impl.Name, sizeof(impl.Name), "%s", impl.SmokeRole.c_str());
	}
	if (host) { unsigned port = 0; const auto parsed = std::from_chars(address.data(), address.data() + address.size(), port); if (parsed.ec != std::errc() || parsed.ptr != address.data() + address.size() || port > 65535) { impl.Error = "Choose a UDP port from 1 to 65535."; return false; } impl.Port = static_cast<int>(port); }
	else std::snprintf(impl.HostAddress, sizeof(impl.HostAddress), "%s", address.c_str());
	const bool started = host ? impl.Host() : impl.Join();
	if (smokeTest) {
		impl.Smoke = true; impl.SmokeStarted = impl.SmokeStageTime = Now();
		std::ofstream("build-mp/" + impl.SmokeRole + "-smoke.log") << (started ? "START" : "FAIL: " + impl.Error) << '\n';
		if (host && started) { for (int i = 0; i < impl.Activities.size(); ++i) if (impl.Activities[i]->GetPresetName() == (impl.SmokeEncounter ? "Brain vs Brain" : "Test Activity")) { impl.ActivityIndex = i; impl.LoadScenes(); break; } if (impl.SmokeDeployment || impl.SmokeEncounter) { for (int i = 0; i < impl.Scenes.size(); ++i) if (impl.Scenes[i]->GetPresetName() == (impl.SmokeEncounter ? "Cave Bunkers" : "Grasslands")) impl.SceneIndex = i; impl.SceneName = impl.Scenes[impl.SceneIndex]->GetPresetName(); } impl.Lobby(); }
	}
	return started;
}
void MultiplayerMan::CaptureVerificationFrame() {
	auto& impl = *m_Impl;
	if (impl.CursorVerification) {
		if (impl.CursorPreparedStage != impl.CursorStage || ++impl.CursorFrames < 20) return;
		const auto& test = Impl::CursorCases[impl.CursorStage];
		const bool drawn = impl.Menu && impl.Menu->CursorDrawn();
		std::ofstream log("build-mp/cursor-smoke.log", std::ios::app);
		if (impl.CursorStage == 0) log << "RESOLUTION: " << g_WindowMan.GetResX() << 'x' << g_WindowMan.GetResY() << '\n';
		std::array<unsigned char, 24 * 24 * 3> pointerPixels{};
		glReadPixels(24, g_WindowMan.GetResY() - 48, 24, 24, GL_RGB, GL_UNSIGNED_BYTE, pointerPixels.data());
		bool rendered = false;
		for (size_t i = 0; i < pointerPixels.size(); i += 3) rendered |= pointerPixels[i] > 100 && pointerPixels[i + 1] > 80 && pointerPixels[i + 2] < 60;
		log << test.Name << ": expected=" << test.Visible << " native=" << drawn << " rendered=" << (drawn && rendered) << '\n';
		const auto layout = test.Visible && impl.Menu ? impl.Menu->VerifyLayout() : "";
		if (!layout.empty()) { log << "FAIL: " << test.Name << ": " << layout << '\n'; System::SetQuit(); }
		impl.SmokeCapture = true; impl.SmokeCaptureDelay = 2;
		if (drawn != test.Visible || (test.Visible && !rendered) || ImGui::GetIO().MouseDrawCursor) { log << "FAIL: cursor visibility in " << test.Name << '\n'; System::SetQuit(); }
	}
	if (!impl.SmokeCapture) return; if (++impl.SmokeCaptureDelay < 3) return; impl.SmokeCapture = false; impl.SmokeCaptureDelay = 0;
	int viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport); int alignment; glGetIntegerv(GL_PACK_ALIGNMENT, &alignment); glPixelStorei(GL_PACK_ALIGNMENT, 1);
	std::vector<uint8_t> pixels(size_t(viewport[2]) * viewport[3] * 3); glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels.data()); glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	if (impl.SmokeDeployment && (impl.State == Impl::Mode::Host || (impl.State == Impl::Mode::Client && impl.World.IsDeploying())) && impl.SmokeStage >= 1 && impl.SmokeStage <= 4) {
		size_t colored = 0; for (size_t i = 0; i < pixels.size(); i += 3) if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++colored;
		const double visible = double(colored) / (pixels.size() / 3);
		impl.Verify("DEPLOYMENT: world visible=" + std::to_string(visible));
		if (visible < 0.6) { impl.SmokeDeploymentFailed = true; impl.Verify("FAIL: deployment world is black"); }
	}
	const size_t pitch = size_t(viewport[2]) * 3; for (int row = 0; row < viewport[3] / 2; ++row) std::swap_ranges(pixels.begin() + row * pitch, pixels.begin() + (row + 1) * pitch, pixels.begin() + (viewport[3] - row - 1) * pitch);
	SDL_Surface* surface = SDL_CreateSurfaceFrom(viewport[2], viewport[3], SDL_PIXELFORMAT_RGB24, pixels.data(), static_cast<int>(pitch));
	const auto path = impl.CursorVerification ? std::string("build-mp/cursor-") + Impl::CursorCases[impl.CursorStage].Name + ".png" : std::string("build-mp/") + impl.SmokeRole + "-" + (impl.SmokeCaptureName.empty() ? "stage" + std::to_string(impl.SmokeStage) : impl.SmokeCaptureName) + ".png";
	if (surface) { impl.Verify(IMG_SavePNG(surface, path.c_str()) ? "SCREENSHOT: " + path : "FAIL: screenshot"); SDL_DestroySurface(surface); }
	impl.SmokeCaptureName.clear();
	if (impl.CursorVerification && !System::IsSetToQuit()) { impl.CursorFrames = 0; if (++impl.CursorStage == Impl::CursorCases.size()) { std::ofstream("build-mp/cursor-smoke.log", std::ios::app) << "PASS: rendered cursor in menu, lobbies, session and connection screens; hidden during gameplay and after closing\n"; impl.CursorVerification = false; impl.State = Impl::Mode::Idle; impl.UI = impl.Playing = false; System::SetQuit(); } }
}
bool MultiplayerMan::IsUIOpen() const { return m_Impl->UI || m_Impl->State == Impl::Mode::Client || m_Impl->State == Impl::Mode::Connecting || m_Impl->State == Impl::Mode::Reconnecting; }
bool MultiplayerMan::IsHostingMatch() const { return m_Impl->State == Impl::Mode::Host && m_Impl->Playing; }
bool MultiplayerMan::IsRemotePlayer(int player) const { return IsHostingMatch() && player > 0 && player < 4 && m_Impl->Players[player].Token != 0; }
void MultiplayerMan::SetTextInputActive(int player, bool active) {
	if (!IsRemotePlayer(player)) return; auto& peer = m_Impl->Players[player]; peer.TextActive = active; peer.Text.clear();
	MP::Writer writer(Kind::TextInput, m_Impl->Session, m_Impl->Epoch); writer.U8(1); writer.U8(active); if (peer.Connected) m_Impl->Net.Send(peer.Address, writer.Data, Delivery::Control);
}
bool MultiplayerMan::TakeTextInput(int player, std::string& text) {
	if (!IsRemotePlayer(player)) { text.clear(); return false; } text = std::move(m_Impl->Players[player].Text); m_Impl->Players[player].Text.clear(); return !text.empty();
}
bool MultiplayerMan::TakeLaunchRequest() { const bool launch = m_Impl->Launch; m_Impl->Launch = false; return launch; }
int MultiplayerMan::ViewWidth(int screen) const { const auto* activity = g_ActivityMan.GetActivity(); const int player = activity && screen > 0 ? activity->PlayerOfScreen(screen) : 0; return IsHostingMatch() && player > 0 ? m_Impl->Players[player].ViewWidth : g_WindowMan.GetResX(); }
int MultiplayerMan::ViewHeight(int screen) const { const auto* activity = g_ActivityMan.GetActivity(); const int player = activity && screen > 0 ? activity->PlayerOfScreen(screen) : 0; return IsHostingMatch() && player > 0 ? m_Impl->Players[player].ViewHeight : g_WindowMan.GetResY(); }
bool MultiplayerMan::WantsState(int player) const {
	if (m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3) return false;
	if (!IsRemotePlayer(player)) return false; const auto& peer = m_Impl->Players[player];
	return peer.Connected && Now() - peer.LastWorld >= MP::World::SnapshotIntervalMS;
}
void MultiplayerMan::BeginWorldCapture() { if (IsHostingMatch() && !(m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3)) m_Impl->World.BeginObjects(); }
void MultiplayerMan::BeginWorldTrails() { if (IsHostingMatch() && !(m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3)) m_Impl->World.BeginTrails(); }
void MultiplayerMan::EndWorldCapture() { m_Impl->World.EndObjects(); }
void MultiplayerMan::BeginGuestView(BITMAP* gui, float cameraX, float cameraY) { if (m_Impl->SmokeEncounter) m_Impl->SmokeCaptureStarted = NowMicros(); m_Impl->World.BeginView(gui, Vector(cameraX, cameraY)); }
void MultiplayerMan::EndGuestView(int player) {
	auto& impl = *m_Impl; auto& peer = impl.Players[player]; peer.LastWorld = Now();
	const uint64_t captured = impl.SmokeEncounter ? NowMicros() : 0;
	if (!++peer.FrameID) ++peer.FrameID;
	auto snapshot = impl.World.EndView(player, peer.FrameID, peer.Inputs.LastInput().Sequence, peer.LastWorld);
	snapshot.Paused = snapshot.Paused || impl.WarmingGuests;
	const uint64_t composed = impl.SmokeEncounter ? NowMicros() : 0;
	snapshot.MouseX = peer.Inputs.LastInput().MouseX; snapshot.MouseY = peer.Inputs.LastInput().MouseY;
	if (!peer.SceneSent) {
		if (impl.SceneManifest.empty()) impl.SceneManifest = impl.World.PrepareScene();
		MP::Writer manifest(Kind::WorldManifest); MP::World::WriteManifest(manifest, impl.SceneManifest);
		if (impl.QueueWorld(peer, Kind::WorldManifest, manifest.Data, Delivery::WorldResource)) { peer.SceneSent = true; peer.SceneQueue = {impl.SceneManifest.begin(), impl.SceneManifest.end()}; }
	}
	if (impl.SmokeCombatStress && impl.SmokeStage == 3 && std::any_of(snapshot.Nodes.begin(), snapshot.Nodes.end(), [&](const auto& node) { return node.Type == MP::World::Shape::Sprite && node.Parent == snapshot.ControlledActor && node.Control == MP::World::Interaction::RadialBackground; })) {
		const auto held = peer.Inputs.LastInput().Held;
		if (held & (uint64_t(1) << INPUT_PIEMENU_DIGITAL)) peer.SmokeRadialKeyboardSeen = true;
		if (held & (uint64_t(1) << INPUT_PIEMENU_ANALOG)) peer.SmokeRadialMouseSeen = true;
	}
	if (impl.SmokeCombatStress && impl.SmokeStage == 3 && snapshot.ViewMode == Activity::ActorSelect &&
		(peer.Inputs.LastInput().Held & (uint64_t(1) << INPUT_PREV))) {
		if (!peer.SmokeQCameraSeen) { peer.SmokeQCameraSeen = true; peer.SmokeQTargetX = snapshot.CameraTargetX; peer.SmokeQTargetY = snapshot.CameraTargetY; }
		peer.SmokeQCameraMoved |= std::hypot(MP::World::Displacement(peer.SmokeQTargetX, snapshot.CameraTargetX, snapshot.SceneWidth, snapshot.Wrap & 1),
			MP::World::Displacement(peer.SmokeQTargetY, snapshot.CameraTargetY, snapshot.SceneHeight, snapshot.Wrap & 2)) > 5;
	}
	// Drop obsolete resources that have not reached the reliable transport.
	// Explosions can otherwise leave dead particles and old terrain revisions
	// ahead of the resources actually needed by the current view.
	if (peer.AckID) {
		std::unordered_set<uint64_t> needed; for (const auto& node : snapshot.Nodes) if (node.Asset) needed.insert(node.Asset);
		std::unordered_set<uint32_t> obsolete;
		std::erase_if(peer.ResourceMessages, [&](const auto& entry) {
			if (needed.contains(entry.first) || peer.ResourceFlight.Contains(entry.second)) return false;
			obsolete.insert(entry.second); peer.PendingResources.erase(entry.first); peer.QueuedResourceChunks.erase(entry.second); return true;
		});
		std::erase_if(peer.WorldPackets, [&](const auto& packet) {
			if (packet.second != Delivery::WorldResource) return false;
			MP::Reader reader(packet.first); Header header; MP::World::Chunk chunk;
			return ReadHeader(reader, header) && MP::World::ReadChunk(reader, chunk) && obsolete.contains(chunk.ID);
		});
	}
	for (const auto& node : snapshot.Nodes) {
		if (!node.Asset || peer.WorldResources.contains(node.Asset)) continue;
		if (peer.WorldPackets.size() >= 8192) break;
		if (auto pending = peer.PendingResources.find(node.Asset); pending != peer.PendingResources.end() && Now() - pending->second < 5000) continue;
		if (auto message = peer.ResourceMessages.find(node.Asset); message != peer.ResourceMessages.end() && (peer.QueuedResourceChunks.contains(message->second) || peer.ResourceFlight.Contains(message->second))) continue;
		if (const auto* resource = impl.World.FindResource(node.Asset)) {
			MP::Writer writer(Kind::WorldResource); MP::World::WriteResource(writer, *resource);
			if (impl.QueueWorld(peer, Kind::WorldResource, writer.Data, Delivery::WorldResource, node.Asset)) peer.PendingResources[node.Asset] = Now();
		}
	}
	MP::Writer writer(Kind::WorldSnapshot); MP::World::WriteSnapshot(writer, snapshot);
	const uint64_t serialized = impl.SmokeEncounter ? NowMicros() : 0;
	if (impl.SmokeEncounter) { impl.SmokeEncounterPeakNodes = std::max(impl.SmokeEncounterPeakNodes, snapshot.Nodes.size()); impl.SmokeEncounterPeakTrails = std::max(impl.SmokeEncounterPeakTrails, size_t(std::count_if(snapshot.Nodes.begin(), snapshot.Nodes.end(), [](const auto& node) { return node.StartTime != 0; }))); impl.SmokeEncounterPeakBytes = std::max(impl.SmokeEncounterPeakBytes, writer.Data.size()); }
	impl.QueueWorld(peer, Kind::WorldSnapshot, writer.Data, Delivery::State);
	if (impl.SmokeEncounter && impl.SmokeStage == 3) {
		const uint64_t queued = NowMicros(); impl.SmokeCaptureTimes.push_back(queued - impl.SmokeCaptureStarted);
		impl.SmokeCaptureParts[0] += captured - impl.SmokeCaptureStarted; impl.SmokeCaptureParts[1] += composed - captured;
		impl.SmokeCaptureParts[2] += serialized - composed; impl.SmokeCaptureParts[3] += queued - serialized;
	}
}
bool MultiplayerMan::Impl::QueueResource(Player& player, uint64_t id) {
	if (player.WorldResources.contains(id)) return true;
	if (auto pending = player.PendingResources.find(id); pending != player.PendingResources.end() && Now() - pending->second < 5000) return true;
	if (auto message = player.ResourceMessages.find(id); message != player.ResourceMessages.end() && (player.QueuedResourceChunks.contains(message->second) || player.ResourceFlight.Contains(message->second))) return true;
	const auto* resource = World.FindResource(id); if (!resource) return false;
	MP::Writer writer(Kind::WorldResource); MP::World::WriteResource(writer, *resource);
	if (!QueueWorld(player, Kind::WorldResource, writer.Data, Delivery::WorldResource, id)) return false;
	player.PendingResources[id] = Now(); return true;
}
bool MultiplayerMan::Impl::QueueWorld(Player& player, Kind kind, std::span<const uint8_t> payload, Delivery delivery, uint64_t resource) {
	if (payload.empty() || payload.size() > MP::World::MaxPayload) return false;
	MP::Writer packed(kind); packed.U32(uint32_t(payload.size())); const size_t prefix = packed.Data.size();
	packed.Data.resize(prefix + LZ4_compressBound(int(payload.size())));
	const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(payload.data()), reinterpret_cast<char*>(packed.Data.data() + prefix), int(payload.size()), int(packed.Data.size() - prefix));
	if (compressed <= 0) return false; packed.Data.resize(prefix + compressed);
	if (SmokeEncounter && kind == Kind::WorldSnapshot) { SmokeEncounterPeakPacked = std::max(SmokeEncounterPeakPacked, packed.Data.size()); SmokeEncounterWireBytes += packed.Data.size(); ++SmokeEncounterWireUpdates; }
	uint32_t id = ++player.WorldMessage; if (!id) id = ++player.WorldMessage;
	// Unsent poses are replaceable. Retained resources remain reliable and are
	// not delayed by a backlog of obsolete scene states.
	if (kind == Kind::WorldSnapshot) {
		if (player.SnapshotStarted && std::any_of(player.WorldPackets.begin(), player.WorldPackets.end(), [](const auto& packet) { return packet.second == Delivery::State; })) return false;
		player.SnapshotStarted = false;
		std::erase_if(player.WorldPackets, [](const auto& packet) { return packet.second == Delivery::State; });
	}
	const size_t count = (packed.Data.size() + ChunkBytes - 1) / ChunkBytes;
	const size_t packetCount = count + (kind == Kind::WorldSnapshot ? (count + ParityGroup - 1) / ParityGroup : 0);
	if (player.WorldPackets.size() + packetCount > 8192) return false;
	if (resource) { player.ResourceMessages[resource] = id; player.QueuedResourceChunks[id] = packetCount; }
	std::vector<std::pair<std::vector<uint8_t>, Delivery>> packets;
	for (size_t first = 0; first < count; first += ParityGroup) {
		std::array<uint8_t, ChunkBytes> parity{};
		for (size_t i = first; i < std::min(first + ParityGroup, count); ++i) {
			const auto bytes = std::span(packed.Data).subspan(i * ChunkBytes, std::min<size_t>(ChunkBytes, packed.Data.size() - i * ChunkBytes));
			for (size_t j = 0; j < bytes.size(); ++j) parity[j] ^= bytes[j];
			MP::Writer packet(kind, Session, Epoch); MP::World::WriteChunk(packet, {id, uint32_t(packed.Data.size()), uint16_t(i), bytes}); packets.emplace_back(std::move(packet.Data), delivery);
		}
		if (kind == Kind::WorldSnapshot) { MP::Writer packet(kind, Session, Epoch); MP::World::WriteChunk(packet, {id, uint32_t(packed.Data.size()), uint16_t(first / ParityGroup), parity, true}); packets.emplace_back(std::move(packet.Data), delivery); }
	}
	if (kind == Kind::WorldSnapshot) for (auto it = packets.rbegin(); it != packets.rend(); ++it) player.WorldPackets.push_front(std::move(*it));
	else for (auto& packet : packets) player.WorldPackets.push_back(std::move(packet));
	return true;
}
void MultiplayerMan::Impl::ReceiveWorld(Kind kind, MP::Reader& reader) {
	MP::World::Chunk chunk; if (!MP::World::ReadChunk(reader, chunk)) return;
	if (Smoke && SmokeWorldLoss && kind == Kind::WorldSnapshot && !chunk.Parity && chunk.Index % ParityGroup == 0) return;
	// A relay's transport ACK covers only the first network leg. Guest receipts
	// bound reliable resources across both legs without delaying scene poses.
	if (kind != Kind::WorldSnapshot) { MP::Writer receipt(Kind::WorldAck, Session, Epoch); receipt.U8(3); receipt.U32(chunk.ID); receipt.U16(chunk.Index); Net.Send(ServerAddress, receipt.Data, Delivery::Control); }
	auto& assembly = kind != Kind::WorldSnapshot ? ResourceAssembly : SnapshotAssembly;
	auto complete = assembly.Push(chunk, Now()); if (!complete) return;
	MP::Reader packed(*complete); Header header; uint32_t size;
	if (!ReadHeader(packed, header) || header.Type != kind || !packed.U32(size) || !size || size > MP::World::MaxPayload) return;
	std::vector<uint8_t> payload(size);
	if (LZ4_decompress_safe(reinterpret_cast<const char*>(packed.Rest().data()), reinterpret_cast<char*>(payload.data()), int(packed.Remaining()), int(size)) != int(size)) return;
	MP::Reader data(payload); if (!ReadHeader(data, header) || header.Type != kind) return;
	if (kind == Kind::WorldManifest) {
		std::unordered_set<uint64_t> assets; if (!MP::World::ReadManifest(data, assets)) return;
		World.PinScene(assets); SceneReceived = true; SceneReceivedAt = Now(); SceneMissing.clear();
		if (const auto* scene = dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", SceneName))) World.PrimeSceneBackdrops(*scene);
		SceneManifest.assign(assets.begin(), assets.end());
		if (Smoke) Verify("SCENE: backdrop cache=" + std::to_string(std::count_if(assets.begin(), assets.end(), [&](auto id) { return World.FindResource(id) != nullptr; })) + " / " + std::to_string(assets.size()) + " map=" + SceneName);
		for (auto id : assets) {
            if (!World.FindResource(id)) SceneMissing.insert(id);
            else { MP::Writer ack(Kind::WorldAck, Session, Epoch); ack.U8(1); ack.U64(id); Net.Send(ServerAddress, ack.Data, Delivery::Control); ReportedResources.insert(id); }
        }
		PresentPendingWorlds();
	} else if (kind == Kind::WorldResource) {
		MP::World::Resource resource; if (!MP::World::ReadResource(data, resource)) return;
		const uint64_t id = resource.ID; if (!World.Install(std::move(resource))) { Error = "The synchronized scene exceeded its resource limit."; return; }
		MissingResources.erase(id); SceneMissing.erase(id); ReportedResources.insert(id);
		MP::Writer ack(Kind::WorldAck, Session, Epoch); ack.U8(1); ack.U64(id); Net.Send(ServerAddress, ack.Data, Delivery::Control);
		PresentPendingWorlds();
	} else {
		MP::World::Snapshot snapshot; if (!MP::World::ReadSnapshot(data, snapshot)) return;
		const auto missing = World.Missing(snapshot); MissingResources = {missing.begin(), missing.end()};
		// A content-addressed asset remains valid across rounds and reconnects.
		// Tell the host which cached assets need no second reliable transfer.
		for (const auto& node : snapshot.Nodes) if (node.Asset && World.FindResource(node.Asset) && ReportedResources.insert(node.Asset).second) {
			MP::Writer ack(Kind::WorldAck, Session, Epoch); ack.U8(1); ack.U64(node.Asset); Net.Send(ServerAddress, ack.Data, Delivery::Control);
		}
		if (!World.Ready() && (!SceneReceived || !SceneMissing.empty() || !World.Missing(snapshot).empty())) {
			if (PendingWorlds.empty() || Newer(snapshot.ID, PendingWorlds.back().ID)) {
				// Keep an attainable baseline while newer poses introduce resources.
				// Replacing it continually can prevent a slow guest ever loading.
				if (PendingWorlds.size() == 8) PendingWorlds.erase(std::next(PendingWorlds.begin()));
				PendingWorlds.push_back(std::move(snapshot));
			}
		}
		else PresentWorld(std::move(snapshot));
	}
}
void MultiplayerMan::Impl::PresentPendingWorlds() {
	if (!SceneReceived || !SceneMissing.empty()) return;
	for (size_t i = PendingWorlds.size(); i > 0; --i) if (World.Missing(PendingWorlds[i - 1]).empty()) {
		auto snapshot = std::move(PendingWorlds[i - 1]); const uint32_t id = snapshot.ID;
		PresentWorld(std::move(snapshot));
		std::erase_if(PendingWorlds, [id](const auto& pending) { return !Newer(pending.ID, id); });
		return;
	}
}
void MultiplayerMan::Impl::PresentWorld(MP::World::Snapshot snapshot) {
	const auto id = snapshot.ID; const auto cameraX = snapshot.CameraX, cameraY = snapshot.CameraY; const int width = snapshot.Width, height = snapshot.Height;
	if (!World.Install(std::move(snapshot), Now())) return;
	const uint64_t now = Now();
	if (SmokeCombatStress && SmokeStage == 3 && LastFrameReceived && World.Updates() > 1) { SmokeMaxUpdateGap = std::max(SmokeMaxUpdateGap, now - LastFrameReceived); ++SmokeStressUpdates; }
	LastFrameReceived = now;
	if (SmokeEncounter) {
		if (SmokeEncounterLastUpdate) SmokeEncounterGap = std::max(SmokeEncounterGap, now - SmokeEncounterLastUpdate);
		SmokeEncounterLastUpdate = now; ++SmokeEncounterUpdates;
	}
	std::erase_if(PendingWorlds, [id](const auto& pending) { return !Newer(pending.ID, id); });
	TextureWidth = width; TextureHeight = height; ++Presented;
	g_AudioMan.SetStreamListener(Vector(cameraX + width / 2, cameraY + height / 2), true);
	MP::Writer ack(Kind::WorldAck, Session, Epoch); ack.U8(0); ack.U32(id); Net.Send(ServerAddress, ack.Data, Delivery::Input);
}
bool MultiplayerMan::Impl::VerifyWorldFlow(std::ostream& log) {
	World.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear();
	SceneReceived = true; SceneReceivedAt = Now(); SceneMissing.clear();
	Player sender;
	auto receive = [&](const MP::World::Snapshot& snapshot) {
		MP::Writer writer(Kind::WorldSnapshot); MP::World::WriteSnapshot(writer, snapshot);
		if (!QueueWorld(sender, Kind::WorldSnapshot, writer.Data, Delivery::State)) return false;
		while (!sender.WorldPackets.empty()) {
			auto packet = std::move(sender.WorldPackets.front().first); sender.WorldPackets.pop_front();
			MP::Reader reader(packet); Header header; if (!ReadHeader(reader, header)) return false; ReceiveWorld(header.Type, reader);
		}
		return true;
	};
	MP::World::Resource resource; resource.Width = resource.Height = 1; resource.Pixels = {uint8_t(g_WhiteColor)}; resource.ID = MP::World::ResourceHash(resource);
	MP::World::Snapshot snapshot; snapshot.ID = 1; snapshot.Time = Now(); snapshot.SceneWidth = 640; snapshot.SceneHeight = 360;
	MP::World::Node node; node.ID = 1; node.Asset = resource.ID; node.SourceWidth = node.SourceHeight = 1; snapshot.Nodes.push_back(node);
	bool passed = receive(snapshot);
	++snapshot.ID; ++snapshot.Time; snapshot.Nodes[0].Asset = resource.ID + 1; passed &= receive(snapshot);
	World.Install(resource); PresentPendingWorlds();
	const bool baseline = World.Ready(); passed &= baseline;
	log << (baseline ? "OK: " : "FAIL: ") << "an attainable startup state survives newer missing resources\n";
	for (int burst = 0; burst < 200; ++burst) { ++snapshot.ID; ++snapshot.Time; snapshot.Nodes[0].Asset = resource.ID + 2 + burst; snapshot.Nodes[0].X = float(burst); passed &= receive(snapshot); }
	const bool moving = World.Updates() == 201; passed &= moving;
	log << (moving ? "OK: " : "FAIL: ") << "200 combat updates continue while every new effect resource is delayed; updates=" << World.Updates() << '\n';
	World.Reset(); PendingWorlds.clear(); SceneReceived = false; SceneMissing.clear();
	World.Install(resource); snapshot.Nodes[0].Asset = resource.ID; ++snapshot.ID; ++snapshot.Time;
	passed &= receive(snapshot);
	const bool waiting = !World.Ready(); passed &= waiting;
	auto scenery = resource; scenery.Pixels[0] ^= 1; scenery.ID = MP::World::ResourceHash(scenery);
	std::vector<uint64_t> sceneAssets{resource.ID, scenery.ID};
	MP::Writer manifest(Kind::WorldManifest); MP::World::WriteManifest(manifest, sceneAssets);
	passed &= QueueWorld(sender, Kind::WorldManifest, manifest.Data, Delivery::WorldResource);
	while (!sender.WorldPackets.empty()) { auto packet = std::move(sender.WorldPackets.front().first); sender.WorldPackets.pop_front(); MP::Reader reader(packet); Header header; passed &= ReadHeader(reader, header); ReceiveWorld(header.Type, reader); }
	const bool waitingForMap = !World.Ready() && SceneMissing.contains(scenery.ID); passed &= waitingForMap;
	MP::Writer terrain(Kind::WorldResource); MP::World::WriteResource(terrain, scenery);
	passed &= QueueWorld(sender, Kind::WorldResource, terrain.Data, Delivery::WorldResource);
	while (!sender.WorldPackets.empty()) { auto packet = std::move(sender.WorldPackets.front().first); sender.WorldPackets.pop_front(); MP::Reader reader(packet); Header header; passed &= ReadHeader(reader, header); ReceiveWorld(header.Type, reader); }
	const bool warmed = World.Ready() && SceneMissing.empty(); passed &= warmed;
	log << (waiting && waitingForMap && warmed ? "OK: " : "FAIL: ") << "guest waits for the full terrain manifest and scenery before accepting controls\n";
	World.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear();
	ResetSceneTransfer();
	return passed;
}
void MultiplayerMan::Impl::Receive(const TransportEvent& event) {
	MP::Reader reader(event.Data); Header header;
	if (!ReadHeader(reader, header)) { if (State == Mode::Host && Pending.contains(event.Address)) Reject(event.Address, "Incompatible multiplayer protocol. Install the same game build as the host."); return; }
	if (State == Mode::Host) {
		if (header.Type == Kind::Hello && Pending.contains(event.Address)) {
			std::string name, version; uint64_t token; uint16_t width, height;
			if (!reader.Text(name, 31) || name.empty() || !reader.U64(token) || !reader.Text(version) || !reader.U16(width) || width < 320 || width > 3840 || !reader.U16(height) || height < 180 || height > 2160 || !reader.Done()) { Reject(event.Address, "Invalid player registration."); return; }
			if (version != c_GameVersion.str()) { Reject(event.Address, "Game version differs from the host. Install the same build."); return; }
			int slot = -1;
			if (token) for (int i = 1; i < 4; ++i) if (Players[i].Token == token && !Players[i].Connected && Players[i].ReservedUntil > Now()) slot = i;
			if (slot < 0 && !Playing) for (int i = 1; i < 4; ++i) if (!Players[i].Token) { slot = i; break; }
			if (slot < 0) { Reject(event.Address, Playing ? "Match already started. Rejoin with your original player slot or wait for the lobby." : "All player slots are occupied or reserved for reconnecting players."); return; }
			if (Smoke && token && Players[slot].Token == token) { ++SmokeRejoins; Verify("REJOIN: player " + std::to_string(slot + 1)); }
			auto& player = Players[slot]; player.Address = event.Address; player.Name = name; player.Connected = true; player.Ready = false; player.ReservedUntil = 0;
			if (!player.Token) player.Token = Token();
			player.ViewWidth = width; player.ViewHeight = height; if (Playing && token == player.Token) player.Inputs.ReleaseControls(); else player.Inputs.Reset(); ClearWorldPackets(player); player.WorldResources.clear(); player.PendingResources.clear(); player.LastWorld = 0; player.LastAck = Now(); player.AudioReady = false; player.ReplayLoops = true; Pending.erase(event.Address); Welcome(slot); return;
		}
		int slot = -1; for (int i = 1; i < 4; ++i) if (Players[i].Connected && Players[i].Address == event.Address) slot = i;
		if (slot < 0 || header.Session != Session || (header.Epoch != Epoch && header.Type != Kind::Chat)) return;
		auto& player = Players[slot];
		if (header.Type == Kind::Input && Playing) { Input input; if (ReadInput(reader, input)) player.Inputs.Push(input, Now()); }
		else if (header.Type == Kind::Ready && !Playing) { uint8_t ready, team; if (reader.U8(ready) && ready <= 1 && reader.U8(team) && team < 4 && (AvailableTeams & (1 << team)) && reader.Done()) { player.Ready = ready != 0; player.Team = team; Lobby(); } }
		else if (header.Type == Kind::Chat) { std::string message; if (reader.Text(message, 192) && !message.empty() && reader.Done() && Now() - player.LastChat > 250) { player.LastChat = Now(); AddChat(slot, message); } }
		else if (header.Type == Kind::WorldAck) {
			uint8_t type; if (!reader.U8(type)) return;
			if (type == 1) { uint64_t id; if (reader.U64(id) && reader.Done() && World.FindResource(id)) { player.PendingResources.erase(id); player.WorldResources.insert(id); } }
			else if (type == 3) { uint32_t message; uint16_t index; if (reader.U32(message) && reader.U16(index) && reader.Done()) player.ResourceFlight.Receipt(message, index); }
			else if (type == 2) { uint64_t id; if (reader.U64(id) && reader.Done() && World.FindResource(id)) { player.WorldResources.erase(id); player.PendingResources.erase(id); if (std::find(player.SceneQueue.begin(), player.SceneQueue.end(), id) == player.SceneQueue.end()) player.SceneQueue.push_back(id); } }
			else if (type == 0) { uint32_t id; if (reader.U32(id) && reader.Done() && !Newer(id, player.FrameID) && Newer(id, player.AckID)) { player.AckID = id; player.LastAck = Now(); if (!player.AudioReady) g_MultiplayerMan.SetTextInputActive(slot, player.TextActive); player.AudioReady = true; } }
		}
		else if (header.Type == Kind::TextInput && Playing) { uint8_t direction; std::string text; if (reader.U8(direction) && direction == 0 && reader.Text(text, 64) && reader.Done() && player.TextActive && player.Text.size() + text.size() <= 256) { player.Text += text; if (Smoke && text == "Shop text test") { player.SmokeTextSeen = true; Verify("GUI TEXT: player " + std::to_string(slot + 1)); } } }
		else if (header.Type == Kind::Leave && reader.Done()) { Net.Close(event.Address); player.Connected = false; player.Ready = false; player.ReservedUntil = Playing ? Now() + 60000 : 0; player.Inputs.ReleaseControls(); g_UInputMan.ClearRemoteInput(slot); Lobby(); }
		return;
	}
	if (event.Address != ServerAddress) return;
	if (header.Type == Kind::Reject && (State == Mode::Connecting || State == Mode::Reconnecting)) { std::string message; if (reader.Text(message) && reader.Done()) { Error = message; Stop(); UI = true; } return; }
	if (header.Type == Kind::Welcome && (State == Mode::Connecting || State == Mode::Reconnecting)) {
		uint8_t slot; uint64_t token; std::string version;
		if (!header.Session || !reader.U8(slot) || slot < 1 || slot > 3 || !reader.U64(token) || !token || !reader.Text(version) || version != c_GameVersion.str() || !reader.Done()) return;
		const bool sameMatch = State == Mode::Reconnecting && Session == header.Session && Epoch == header.Epoch && ReconnectToken == token;
		if (sameMatch) {
			for (size_t i = 0; i < InputCount; ++i) if (LocalInput.Held & (uint64_t(1) << i)) ++LocalInput.Releases[i];
			for (size_t i = 0; i < 3; ++i) if (LocalInput.MouseHeld & (1 << i)) ++LocalInput.MouseReleases[i];
			LocalInput.Held = 0; LocalInput.MouseHeld = 0; LocalInput.AimX = LocalInput.AimY = LocalInput.MoveX = LocalInput.MoveY = 0;
		} else LocalInput = {};
		Session = header.Session; Epoch = header.Epoch; LocalSlot = slot; ReconnectToken = token; State = Mode::Client; World.ResetPresentation(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); Texture = 0; LastRender = 0; ClearSounds(); UI = !Playing; Error.clear(); LastFrameReceived = Now(); return;
	}
	if (State != Mode::Client || header.Session != Session) return;
	if (header.Type == Kind::Chat) { uint8_t player; std::string message; if (reader.U8(player) && player < 4 && reader.Text(message, 192) && reader.Done()) AddChat(player, message); return; }
	if (header.Type == Kind::Leave && reader.Done()) { Stop(); Error = "The host closed the room."; UI = true; return; }
	if (header.Type == Kind::Lobby && (header.Epoch == Epoch || Newer(header.Epoch, Epoch))) {
		uint8_t playing; std::string room, activity, scene;
		if (!reader.U8(playing) || playing > 1 || !reader.Text(room, 63) || !reader.Text(activity) || !reader.Text(scene)) return;
		uint8_t difficulty, fog, deploy, clearOrbit, teams, cpu; uint32_t gold; std::array<std::string, 4> tech;
		if (!reader.U8(difficulty) || difficulty > 100 || !reader.U32(gold) || gold > 1000000 || !reader.U8(fog) || fog > 1 || !reader.U8(deploy) || deploy > 1 || !reader.U8(clearOrbit) || clearOrbit > 1 || !reader.U8(teams) || teams > 15 || !reader.U8(cpu) || cpu > 4) return;
		for (auto& faction: tech) if (!reader.Text(faction)) return;
		struct Slot { uint8_t Occupied, Connected, Ready, Team; std::string Name; }; std::array<Slot, 4> slots;
		for (auto& slot: slots) if (!reader.U8(slot.Occupied) || slot.Occupied > 1 || !reader.U8(slot.Connected) || slot.Connected > 1 || !reader.U8(slot.Ready) || slot.Ready > 1 || !reader.U8(slot.Team) || slot.Team > 3 || !reader.Text(slot.Name, 31)) return;
		if (!reader.Done()) return;
		const bool changedMatch = Epoch != header.Epoch || Playing != (playing != 0);
		if (Epoch != header.Epoch) { Epoch = header.Epoch; LocalInput = {}; ClearSounds(); TextActive = false; LastFrameReceived = Now(); World.ResetPresentation(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); Texture = 0; TextureWidth = TextureHeight = 0; }
		if (changedMatch && !playing && Playing) { Played = true; Notice = "Round complete. Ready up for the next match."; }
		if (changedMatch && playing) Notice.clear();
		Playing = playing != 0; RoomName = room; ActivityName = activity; SceneName = scene; if (!Playing || changedMatch) UI = !Playing;
		Difficulty = difficulty; Gold = static_cast<int>(gold); Fog = fog; Deploy = deploy; ClearOrbit = clearOrbit; AvailableTeams = teams; CPUTeam = cpu; Tech = std::move(tech);
		if (!Playing) g_AudioMan.SetStreamListener(Vector(), false);
		if (changedMatch && !Playing) { ClearSounds(); TextActive = false; SDL_StopTextInput(g_WindowMan.GetWindow()); }
		if (Smoke) Verify("LOBBY UPDATE: epoch=" + std::to_string(Epoch) + " playing=" + std::to_string(Playing));
		for (size_t i = 0; i < slots.size(); ++i) { Players[i].Token = slots[i].Occupied; Players[i].Connected = slots[i].Connected; Players[i].Ready = slots[i].Ready; Players[i].Team = slots[i].Team; Players[i].Name = slots[i].Name; }
		g_UInputMan.TrapMousePos(Playing && !UI); return;
	}
	if (header.Epoch != Epoch) return;
	if (header.Type == Kind::TextInput && Playing) { uint8_t direction, active; if (reader.U8(direction) && direction == 1 && reader.U8(active) && active <= 1 && reader.Done()) TextActive = active != 0; return; }
	if ((header.Type == Kind::WorldSnapshot || header.Type == Kind::WorldResource || header.Type == Kind::WorldManifest) && Playing) { ReceiveWorld(header.Type, reader); return; }
	if (header.Type == Kind::Sound && Playing) HandleAudio(reader);
}

void MultiplayerMan::Impl::SampleInput() {
	static_assert(InputElements::INPUT_COUNT <= GUIKeyFirst && GUIKeyFirst + GUIKeys.size() <= InputCount);
	if (State != Mode::Client || !Playing) return;
	const bool enabled = !UI && Controls && World.Ready() && !World.Paused() && (g_WindowMan.AnyWindowHasFocus() || Smoke);
	if ((enabled || Smoke) && TextActive) { SDL_StartTextInput(g_WindowMan.GetWindow()); const auto& text = g_UInputMan.GetTextInput(); if (!text.empty()) { MP::Writer writer(Kind::TextInput, Session, Epoch); writer.U8(0); writer.Text(text, 64); Net.Send(ServerAddress, writer.Data, Delivery::Control); } }
	else if (enabled && !TextActive) SDL_StopTextInput(g_WindowMan.GetWindow());
	LocalInput.Device = static_cast<uint8_t>(g_UInputMan.GetInputDevice(0));
	for (int i = 0; i < InputElements::INPUT_COUNT; ++i) {
		const uint64_t mask = uint64_t(1) << i;
		if (enabled && g_UInputMan.ElementPressed(0, i)) ++LocalInput.Presses[i];
		if ((enabled && g_UInputMan.ElementReleased(0, i)) || (!enabled && (LocalInput.Held & mask))) ++LocalInput.Releases[i];
		if (enabled && g_UInputMan.ElementHeld(0, i)) LocalInput.Held |= mask; else LocalInput.Held &= ~mask;
	}
	for (size_t key = 0; key < GUIKeys.size(); ++key) {
		const size_t bit = GUIKeyFirst + key; const uint64_t mask = uint64_t(1) << bit;
		if (enabled && g_UInputMan.KeyPressed(GUIKeys[key])) ++LocalInput.Presses[bit];
		if ((enabled && g_UInputMan.KeyReleased(GUIKeys[key])) || (!enabled && (LocalInput.Held & mask))) ++LocalInput.Releases[bit];
		if (enabled && g_UInputMan.KeyHeld(GUIKeys[key])) LocalInput.Held |= mask; else LocalInput.Held &= ~mask;
	}
	const Vector movement = enabled ? g_UInputMan.GetMouseMovement(0) : Vector();
	const auto* viewport = ImGui::GetMainViewport(); const float scale = TextureWidth > 0 ? std::max(0.1f, std::min(viewport->Size.x / TextureWidth, viewport->Size.y / TextureHeight)) : 1.0f;
	MouseRemainderX += movement.GetX() / scale; MouseRemainderY += movement.GetY() / scale;
	const int dx = static_cast<int>(MouseRemainderX), dy = static_cast<int>(MouseRemainderY); MouseRemainderX -= dx; MouseRemainderY -= dy;
	LocalInput.MouseX += dx; LocalInput.MouseY += dy; LocalInput.Wheel += enabled ? g_UInputMan.MouseWheelMoved() : 0;
	for (int i = 0; i < 3; ++i) {
		if (enabled && g_UInputMan.MouseButtonPressed(i + 1, 0)) ++LocalInput.MousePresses[i];
		if ((enabled && g_UInputMan.MouseButtonReleased(i + 1, 0)) || (!enabled && (LocalInput.MouseHeld & (1 << i)))) ++LocalInput.MouseReleases[i];
		if (enabled && g_UInputMan.MouseButtonHeld(i + 1, 0)) LocalInput.MouseHeld |= uint8_t(1 << i); else LocalInput.MouseHeld &= uint8_t(~(1 << i));
	}
	const Vector aim = enabled ? g_UInputMan.AnalogAimValues(0) : Vector(), move = enabled ? g_UInputMan.AnalogMoveValues(0) : Vector();
	LocalInput.AimX = std::clamp(aim.GetX(), -1.0f, 1.0f); LocalInput.AimY = std::clamp(aim.GetY(), -1.0f, 1.0f); LocalInput.MoveX = std::clamp(move.GetX(), -1.0f, 1.0f); LocalInput.MoveY = std::clamp(move.GetY(), -1.0f, 1.0f);
	if (Smoke && !SmokeDeployment && !SmokeEncounter) { LocalInput.Held |= (uint64_t(1) << InputElements::INPUT_L_RIGHT) | (uint64_t(1) << InputElements::INPUT_FIRE); LocalInput.MouseHeld = 1; LocalInput.AimX = 0.75f; LocalInput.MouseX += 2; }
	if (SmokeCombatStress && !SmokeEncounter && SmokeStage == 3) {
		const uint64_t elapsed = Now() - SmokeStageTime;
		const float phase = float(elapsed) * 0.0005f;
		LocalInput.AimX = 0.75f * std::cos(phase); LocalInput.AimY = 0.75f * std::sin(phase);
		const bool keyboard = elapsed >= 2300 && elapsed < 3400, mouse = elapsed >= 4200 && elapsed < 5300;
		if (elapsed >= 1800 && elapsed < 5800) { LocalInput.Held &= ~(uint64_t(1) << INPUT_FIRE); LocalInput.MouseHeld = 0; }
		const bool cameraQ = elapsed >= 6200 && elapsed < 7800;
		const uint64_t held = (keyboard ? uint64_t(1) << INPUT_PIEMENU_DIGITAL : 0) | (mouse ? uint64_t(1) << INPUT_PIEMENU_ANALOG : 0) | (cameraQ ? uint64_t(1) << INPUT_PREV : 0);
		for (const int bit : {int(INPUT_PIEMENU_DIGITAL), int(INPUT_PIEMENU_ANALOG), int(INPUT_PREV)}) {
			const uint64_t mask = uint64_t(1) << bit;
			if ((held & mask) && !(SmokeAutoHeld & mask)) ++LocalInput.Presses[bit];
			if (!(held & mask) && (SmokeAutoHeld & mask)) ++LocalInput.Releases[bit];
		}
		if (mouse && !(SmokeAutoHeld & (uint64_t(1) << INPUT_PIEMENU_ANALOG))) ++LocalInput.MousePresses[2];
		if (!mouse && (SmokeAutoHeld & (uint64_t(1) << INPUT_PIEMENU_ANALOG))) ++LocalInput.MouseReleases[2];
		SmokeAutoHeld = held; LocalInput.Held |= held; if (mouse) LocalInput.MouseHeld |= 4;
		if (keyboard && elapsed > 2900 && !SmokeRadialKeyboardCaptured) { SmokeRadialKeyboardCaptured = true; SmokeCaptureName = "radial-digital"; SmokeCapture = true; }
		if (mouse && elapsed > 4800 && !SmokeRadialMouseCaptured) { SmokeRadialMouseCaptured = true; SmokeCaptureName = "radial-right"; SmokeCapture = true; }
		if (cameraQ) LocalInput.MouseX += 3;
		if (cameraQ && elapsed > 7000 && !SmokeQCameraCaptured) { SmokeQCameraCaptured = true; SmokeCaptureName = "camera-q"; SmokeCapture = true; }
	}
	if (Smoke && TextActive && !SmokeTextSent) { SmokeTextSent = true; MP::Writer writer(Kind::TextInput, Session, Epoch); writer.U8(0); writer.Text("Shop text test", 64); Net.Send(ServerAddress, writer.Data, Delivery::Control); }
	if (Smoke && SmokeStage == 1 && TextActive) LocalInput.Held |= uint64_t(1) << (GUIKeyFirst + 1);
	if (Now() - LastInputSent >= 8) { ++LocalInput.Sequence; LastInputSent = Now(); MP::Writer writer(Kind::Input, Session, Epoch); WriteInput(writer, LocalInput); Net.Send(ServerAddress, writer.Data, Delivery::Input); }
}

void MultiplayerMan::ApplyInputs() {
	if (!IsHostingMatch()) return;
	for (int i = 1; i < 4; ++i) if (IsRemotePlayer(i)) g_UInputMan.SetRemoteInput(i, m_Impl->Players[i].Inputs.Consume(Now()));
}

void MultiplayerMan::Impl::Tick() {
	if (CursorVerification) return;
	const uint64_t now = Now(); const double elapsed = std::min<uint64_t>(100, now - LastUpdate) / 1000.0; LastUpdate = now;
	if (!CursorVerification && State == Mode::Host && Playing && !g_ActivityMan.ActivitySetToRestart()) {
		if (!g_ActivityMan.IsInActivity()) HandleActivityExit();
		else if (const auto* activity = g_ActivityMan.GetActivity(); activity && activity->IsOver()) {
			if (!MatchEndedAt) MatchEndedAt = now;
			if (now - MatchEndedAt >= 5000) ReturnToLobby();
		} else MatchEndedAt = 0;
	}
	if (State == Mode::Client && Playing && g_UInputMan.KeyPressed(SDLK_ESCAPE)) { UI = !UI; g_UInputMan.TrapMousePos(!UI); }
	std::erase_if(Sounds, [](const auto& entry) { return !entry.second->IsBeingPlayed(); });
	for (const auto& event: Net.Poll()) {
		if (event.Kind == TransportEvent::Type::Data) Receive(event);
		else if (event.Kind == TransportEvent::Type::RoomCode) { Error.clear(); Verify("ROOM CODE: " + Relay::DisplayCode(event.Address)); if (Smoke && State == Mode::Host) { std::ofstream file("build-mp/room-code.txt"); file << event.Address; } }
		else if (event.Kind == TransportEvent::Type::ServiceStatus) Error = event.Error;
		else if (event.Kind == TransportEvent::Type::Connected) { if (State == Mode::Host) Pending[event.Address] = now; else if (State == Mode::Connecting || State == Mode::Reconnecting) Hello(event.Address); }
		else if (event.Kind == TransportEvent::Type::Discovered) { MP::Reader reader(event.Data); Header header; std::string room; uint8_t count, playing; if (ReadHeader(reader, header) && header.Type == Kind::Announcement && reader.Text(room, 63) && reader.U8(count) && count <= 4 && reader.U8(playing) && playing <= 1 && reader.Done()) Discovered[event.Address] = room + " (" + std::to_string(count) + "/4" + (playing ? ", in game)" : ", lobby)"); }
		else if (event.Kind == TransportEvent::Type::Failed) { Error = event.Error; if (State != Mode::Reconnecting) { Stop(); UI = true; } }
		else if (event.Kind == TransportEvent::Type::Disconnected) {
			if (State == Mode::Host) {
				Pending.erase(event.Address);
				for (int i = 1; i < 4; ++i) if (Players[i].Address == event.Address && Players[i].Connected) { Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.ReleaseControls(); ClearWorldPackets(Players[i]); g_UInputMan.ClearRemoteInput(i); Lobby(); }
			} else if (State == Mode::Client && event.Address == ServerAddress) { State = Mode::Reconnecting; ConnectStarted = now; NextRetry = now + 1000; UI = true; Error = "Connection interrupted. Reconnecting to your player slot..."; g_UInputMan.TrapMousePos(false); ClearSounds(); }
		}
	}
	if (State == Mode::Connecting && now - ConnectStarted > 10000) { Error = "Connection timed out. Check the host address and UDP port."; Stop(); UI = true; }
	if (State == Mode::Reconnecting) { if (now - ConnectStarted > 20000) { Error = "Unable to reconnect. Your host keeps the slot for 60 seconds."; Stop(); UI = true; } else if (now >= NextRetry) Join(true); }
	if (State == Mode::Host) {
		if (Playing && WarmingGuests && !g_ActivityMan.ActivitySetToRestart()) if (auto* activity = g_ActivityMan.GetActivity(); activity && g_SceneMan.GetScene()) {
			const bool ready = std::all_of(Players.begin() + 1, Players.end(), [](const auto& player) { return !player.Token || (player.Connected && player.AckID != 0); });
			activity->SetPaused(!ready);
			if (ready) { WarmingGuests = false; Verify("SCENE: every guest has the complete battlefield"); }
		}
		for (auto it = Pending.begin(); it != Pending.end();) { if (now - it->second > 5000) { Net.Close(it->first); it = Pending.erase(it); } else ++it; }
		for (int i = 1; i < 4; ++i) {
			auto& player = Players[i];
			if (!Playing && player.Token && !player.Connected && now >= player.ReservedUntil) { Net.Close(player.Address); player = Player(); Lobby(); }
			for (unsigned preload = 0; Playing && player.Connected && !player.SceneQueue.empty() && player.WorldPackets.size() < 512 && preload < 32; ++preload) {
				if (!QueueResource(player, player.SceneQueue.front())) break;
				player.SceneQueue.pop_front();
			}
			player.Budget = std::min(262144.0, player.Budget + elapsed * BandwidthMbps * 125000.0);
			while (!player.WorldPackets.empty() && player.Budget >= player.WorldPackets.front().first.size() && Net.QueuedBytes(player.Address) < 128 * 1024) {
				const auto& [bytes, delivery] = player.WorldPackets.front();
				if (delivery == Delivery::WorldResource && !player.ResourceFlight.CanSend(bytes.size())) break;
				if (!Net.Send(player.Address, bytes, delivery)) break;
				if (delivery == Delivery::State) player.SnapshotStarted = true;
				else if (delivery == Delivery::WorldResource) {
					MP::Reader packet(bytes); Header header; MP::World::Chunk chunk;
					if (ReadHeader(packet, header) && MP::World::ReadChunk(packet, chunk)) {
						player.ResourceFlight.Sent(chunk.ID, chunk.Index, bytes.size());
						if (auto queued = player.QueuedResourceChunks.find(chunk.ID); queued != player.QueuedResourceChunks.end() && !--queued->second) player.QueuedResourceChunks.erase(queued);
					}
				}
				player.Budget -= bytes.size(); player.WorldPackets.pop_front();
			}
			if (player.SnapshotStarted && std::none_of(player.WorldPackets.begin(), player.WorldPackets.end(), [](const auto& packet) { return packet.second == Delivery::State; })) player.SnapshotStarted = false;
			if (Playing && player.Token) SendAudio(i); else g_AudioMan.ClearSoundEvents(i);
		}
		g_AudioMan.ClearSoundEvents(0);
	}
	SampleInput();
	if (State == Mode::Client && Playing && (!PendingWorlds.empty() || !MissingResources.empty() || !SceneMissing.empty()) && now - LastResourceRequest > 1000) {
		LastResourceRequest = now;
		std::unordered_set<uint64_t> missing = MissingResources;
		for (const auto& snapshot : PendingWorlds) for (auto id : World.Missing(snapshot)) missing.insert(id);
		if (SceneReceived && now - SceneReceivedAt > 5000) missing.insert(SceneMissing.begin(), SceneMissing.end());
		for (auto id : ResourceRequests.Select(missing, now)) { MP::Writer request(Kind::WorldAck, Session, Epoch); request.U8(2); request.U64(id); Net.Send(ServerAddress, request.Data, Delivery::Control); }
	}
	SmokeTick();
	if (!CursorVerification && g_MenuMan.GetIsInMenuScreen()) UpdateMenu();
}

void MultiplayerMan::Impl::SmokeTick() {
	if (!Smoke) return; const auto now = Now();
	if (SmokeDeployment) { DeploymentTick(); return; }
	if (SmokeEncounter) { EncounterTick(); return; }
	if (State == Mode::Host && Playing && WarmingGuests) SmokeStageTime = now;
	if (State == Mode::Host && SmokeStage == 1 && SmokeReconnected && std::any_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return !player.Connected || !player.AckID; })) SmokeStageTime = now;
	if (SmokeCombatStress && State == Mode::Host && Playing && SmokeStage == 3 && now - SmokeStageTime > 3000 && now - SmokeStageTime < 9500 && now - SmokeLastExplosion >= 180) {
		SmokeLastExplosion = now;
		const auto* preset = g_PresetMan.GetEntityPreset("TDExplosive", "Frag Grenade", "Base.rte");
		if (!preset) { Verify("FAIL: combat grenade fixture missing"); System::SetQuit(); return; }
		for (int player = 1; player <= SmokeGuests; ++player) if (auto* actor = g_ActivityMan.GetActivity()->GetControlledActor(player)) {
			auto* grenade = dynamic_cast<MOSRotating*>(preset->Clone()); grenade->SetPos(actor->GetPos() + Vector(160, -60));
			grenade->GibThis(Vector(), actor); delete grenade; ++SmokeExplosions;
		}
	}
	if (Playing && now - SmokeLastStats > 3000) {
		SmokeLastStats = now;
		if (State == Mode::Host) Verify("TRANSPORT: " + Net.Diagnostics(Players[1].Address) + " warming=" + std::to_string(WarmingGuests) + " paused=" + std::to_string(g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsPaused()));
		else {
			const auto rendered = World.Rendered(), updated = World.Updates();
			if (SmokeRenderStatsTime && rendered >= SmokeRenderStatsCount && updated >= SmokeStateStatsCount) Verify("LOCAL RENDER: fps=" + std::to_string(1000.0 * (rendered - SmokeRenderStatsCount) / std::max<uint64_t>(1, now - SmokeRenderStatsTime)) + " state hz=" + std::to_string(1000.0 * (updated - SmokeStateStatsCount) / std::max<uint64_t>(1, now - SmokeRenderStatsTime)) + " intermediate=" + std::to_string(World.IntermediateFrames()));
			SmokeRenderStatsTime = now; SmokeRenderStatsCount = rendered; SmokeStateStatsCount = updated;
			Verify("TRANSPORT: " + Net.Diagnostics(ServerAddress) + " renders=" + std::to_string(rendered) + " state updates=" + std::to_string(updated) + " stage=" + std::to_string(SmokeStage) + " paused=" + std::to_string(World.Paused()));
		}
	}
	if (now - SmokeStarted > 180000) { Verify("FAIL: smoke timed out: " + Error); System::SetQuit(true); return; }
	if (State == Mode::Host) {
		if (SmokeStage == 1 && !SmokeLoop && !SmokeLoopStopped && now - SmokeStageTime > 1500) { for (int player = 1; player <= SmokeGuests; ++player) g_MultiplayerMan.SetTextInputActive(player, true); SmokeLoop = std::make_unique<SoundContainer>(); SmokeLoop->GetTopLevelSoundSet().AddSound("Base.rte/Sounds/Craft/ThrusterLoop.flac", false); SmokeLoop->SetLoopSetting(-1); SmokeLoop->SetImmobile(true); SmokeLoop->SetVolume(0.013f); if (!SmokeLoop->Play()) Verify("FAIL: looping sound setup"); }
		if (SmokeStage == 1) for (int player = 1; player <= SmokeGuests; ++player) SmokeGUIKeySeen[player] = SmokeGUIKeySeen[player] || g_UInputMan.KeyHeld(SDL_SCANCODE_BACKSPACE, player);
		if (SmokeStage == 1 && SmokeLoop && now - SmokeStageTime > 11500) { SmokeLoop->Stop(); SmokeLoop.reset(); SmokeLoopStopped = true; }
		if (SmokeStage == 1 && !SmokeReconnected && now - SmokeStageTime > 5000 && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.AckID > 0; })) { SmokeReconnected = true; for (int i = 1; i <= SmokeGuests; ++i) { Net.Close(Players[i].Address); Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.ReleaseControls(); ClearWorldPackets(Players[i]); } Lobby(); Verify("RECONNECT: interrupted guest connections"); }
		if (SmokeStage == 1 && !SmokeCapturedHost && now - SmokeStageTime > 9000) { SmokeCapturedHost = true; UI = true; g_UInputMan.TrapMousePos(false); SmokeCapture = true; }
		if (SmokeStage == 1 && SmokeCapturedHost && !SmokeSessionChecked && now - SmokeStageTime > 9700) {
			Menu->QueueVerificationClick("Resume"); UpdateMenu(); SmokeSessionChecked = true;
			Verify(UI ? "FAIL: host native session resume" : "SESSION: host native session overlay and resume");
		}
		if ((SmokeStage == 0 || SmokeStage == 2) && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.Ready; })) {
			if (!Menu) UpdateMenu(); Menu->QueueVerificationClick("Primary"); UpdateMenu();
			if (!Playing) { Verify("FAIL: native start button: " + StartBlock() + Error); System::SetQuit(true); return; }
			++SmokeStage; SmokeStageTime = now; Verify("MATCH: native start button " + std::to_string(SmokeStage) + " " + ActivityName + " guests=" + std::to_string(SmokeGuests));
		}
		if (SmokeStage == 1 && now - SmokeStageTime > 14000) {
			Verify("INPUT: " + std::to_string(Players[1].Inputs.LastSequence()) + " ACK: " + std::to_string(Players[1].AckID));
			const auto session = Session; const auto token = Players[1].Token; const auto code = Net.RoomCode();
			g_ActivityMan.EndActivity(); g_ActivityMan.SetInActivity(false); HandleActivityExit();
			const bool retained = !Playing && State == Mode::Host && Session == session && Players[1].Token == token && Net.RoomCode() == code && UI && Played;
			Verify(retained ? "LIFECYCLE: activity exit returned to the same room" : "FAIL: activity exit lost the room");
			SmokeCapture = true; SmokeStage = 2; SmokeStageTime = now; Quality = 1; BandwidthMbps = SmokeCombatStress ? 12 : 48;
			for (int i = 0; i < Activities.size(); ++i) if (Activities[i]->GetPresetName() == "One-Man Army") { ActivityIndex = i; LoadScenes(); break; } Lobby();
		}
		if (SmokeStage == 3) for (int player = 1; player <= SmokeGuests; ++player) if (auto* actor = g_ActivityMan.GetActivity()->GetControlledActor(player)) { if (!SmokeActorSeen[player]) { SmokeActorSeen[player] = true; SmokeActorStartX[player] = actor->GetPos().GetX(); } SmokeActorMoved[player] = SmokeActorMoved[player] || std::abs(actor->GetPos().GetX() - SmokeActorStartX[player]) > 5; SmokeFireSeen[player] = SmokeFireSeen[player] || actor->GetController()->IsState(ControlState::WEAPON_FIRE); }
		if (SmokeCombatStress && SmokeStage == 3 && now - SmokeStageTime > 12000) Verify(std::string(SmokeExplosions >= 20 ? "OK: " : "FAIL: ") + "combat explosions=" + std::to_string(SmokeExplosions) + " particles=" + std::to_string(g_MovableMan.GetParticleCount()));
		if (SmokeStage == 3 && now - SmokeStageTime > 12000) {
			bool passed = SmokeRejoins == SmokeGuests;
			for (int player = 1; player <= SmokeGuests; ++player) {
				const auto& peer = Players[player];
				passed &= peer.AckID && peer.Inputs.LastSequence() && SmokeActorSeen[player] && SmokeActorMoved[player] && SmokeFireSeen[player] && SmokeGUIKeySeen[player] && peer.SmokeTextSeen &&
					(!SmokeCombatStress || (peer.SmokeRadialKeyboardSeen && peer.SmokeRadialMouseSeen && peer.SmokeQCameraSeen && peer.SmokeQCameraMoved));
				Verify("PLAYER: " + std::to_string(player + 1) + " moved=" + std::to_string(SmokeActorMoved[player]) + " fired=" + std::to_string(SmokeFireSeen[player]) + " GUI key=" + std::to_string(SmokeGUIKeySeen[player]) + " text=" + std::to_string(peer.SmokeTextSeen) +
					" digital radial=" + std::to_string(peer.SmokeRadialKeyboardSeen) + " mouse radial=" + std::to_string(peer.SmokeRadialMouseSeen) + " Q selection=" + std::to_string(peer.SmokeQCameraSeen) + " Q camera moved=" + std::to_string(peer.SmokeQCameraMoved));
			}
			SmokeCapture = true;
			if (!passed) { Verify("FAIL: gameplay or reconnect acknowledgement missing"); System::SetQuit(true); return; }
			g_ActivityMan.EndActivity(); SmokeStage = 4; SmokeStageTime = now; Verify("LIFECYCLE: waiting for automatic game-over return");
		}
		if (SmokeStage == 4 && !Playing) {
			bool passed = State == Mode::Host && Played && UI && !g_AudioMan.IsInMultiplayerMode();
			for (int player = 1; player <= SmokeGuests; ++player) passed &= Players[player].Connected && Players[player].Token && !Players[player].Ready && !Players[player].Inputs.LastSequence() && Players[player].WorldPackets.empty() && Players[player].Loops.empty();
			Verify(passed ? "PASS: two matches, activity exit and automatic game-over return preserve the room, players and chat; ready, input, frame and audio state reset" : "FAIL: automatic game-over lobby reset");
			SmokeCapture = true; SmokeStage = 5; SmokeStageTime = now;
		}
		if (SmokeStage == 5 && now - SmokeStageTime > 1500) { Stop(); System::SetQuit(true); }
	} else if (State == Mode::Client) {
		if (!SmokeChatSent) { SmokeChatSent = true; MP::Writer chat(Kind::Chat, Session, Epoch); chat.Text("Guest room chat test", 192); Net.Send(ServerAddress, chat.Data, Delivery::Control); }
		if (!Playing && SmokeStage < 4 && !Players[LocalSlot].Ready && now - SmokeReadySent > 1000) {
			if (!Menu) UpdateMenu(); Menu->QueueVerificationClick("Primary"); UpdateMenu(); SmokeReadySent = now;
			Verify("MENU: native ready button");
		}
		if (Playing && Presented >= 20 && SmokeStage == 0) { SmokeStage = 1; SmokeStageTime = now; UI = true; g_UInputMan.TrapMousePos(false); SmokeCapture = true; Verify("FRAMES: " + std::to_string(Presented) + " FPS: " + std::to_string(FPS)); }
		if (SmokeStage == 1 && !SmokeSessionChecked && now - SmokeStageTime > 700) {
			Menu->QueueVerificationClick("Resume"); UpdateMenu(); SmokeSessionChecked = true;
			Verify(UI ? "FAIL: guest native session resume" : "SESSION: guest native session overlay and resume");
		}
		if (!Playing && SmokeStage == 1) { SmokeStage = 2; SmokeCapture = true; Verify("LOBBY: returned from match"); }
		if (Playing && World.Ready() && !World.Paused() && SmokeStage == 2) { SmokeStage = 3; SmokeStageTime = now; SmokeSecondStart = Presented; }
		if (SmokeCombatStress && SmokeStage == 3 && now - SmokeStageTime > (SmokeCombatStress ? 10000 : 6000) && Presented >= SmokeSecondStart + 20 && Texture) Verify(std::string(SmokeStressUpdates >= 20 && SmokeMaxUpdateGap < 1500 ? "OK: " : "FAIL: ") + "combat updates=" + std::to_string(SmokeStressUpdates) + " largest gap ms=" + std::to_string(SmokeMaxUpdateGap));
		if (SmokeStage == 3 && now - SmokeStageTime > (SmokeCombatStress ? 10000 : 6000) && Presented >= SmokeSecondStart + 20 && Texture) { SmokeStage = 4; SmokeCapture = true; const bool chat = std::any_of(Chat.begin(), Chat.end(), [](const auto& line) { return line.find("Guest room chat test") != std::string::npos; }); Verify(std::string(SmokeLoopPlays >= 2 && SmokeLoopStopped && chat ? "SECOND MATCH: " : "FAIL: audio replay, stop or chat missing. ") + "state updates=" + std::to_string(Presented - SmokeSecondStart) + " FPS=" + std::to_string(FPS) + " sounds=" + std::to_string(AudioReceived)); Verify(World.IntermediateFrames() > 20 && World.Rendered() > World.Updates() * 2 ? "PASS: guest draws changing intermediate frames independently of host state updates" : "FAIL: guest local rendering did not produce independent intermediate movement"); }
		if (SmokeStage == 4 && !Playing) { const bool passed = Played && UI && !Texture && !TextActive && Sounds.empty() && !Players[LocalSlot].Ready; Verify(passed ? "PASS: automatic game-over returned guest to the same lobby with readiness and gameplay cleared" : "FAIL: guest game-over lobby cleanup"); SmokeCapture = true; SmokeStage = 5; }
	} else if (SmokeStage == 5 && State == Mode::Idle) System::SetQuit(true);
}

void MultiplayerMan::Impl::EncounterTick() {
	const auto now = Now();
	if (Playing && now - SmokeLastStats > 1000) {
		SmokeLastStats = now;
		Verify("PERFORMANCE: fps=" + std::to_string(State == Mode::Client ? FPS : 1000.0f / std::max(.01f, g_PerformanceMan.GetMSPFAverage())) + " updates=" + std::to_string(World.Updates()));
	}
	if (State == Mode::Idle && SmokeStage == 2) { System::SetQuit(); return; }
	if (now - SmokeStarted > 120000) { Verify("FAIL: delivered soldier encounter timed out"); System::SetQuit(); return; }
	if (State == Mode::Client) {
		if (!Playing && SmokeStage == 0 && !Players[LocalSlot].Ready) SendReady(true, Players[LocalSlot].Team);
		if (Playing && World.Ready() && SmokeStage == 0) { SmokeStage = 1; Verify("ENCOUNTER: guest world loaded"); }
		if (!Playing && SmokeStage == 1) {
			if (SmokeNativeBaseline) Verify("PASS: native simulation performance comparison completed");
			else Verify(std::string(SmokeEncounterUpdates > 100 && SmokeEncounterGap < 500 ? "PASS: " : "FAIL: ") + "delivered soldier combat updates=" + std::to_string(SmokeEncounterUpdates) + " largest gap ms=" + std::to_string(SmokeEncounterGap));
			SmokeStage = 2;
		}
		if (State == Mode::Idle && SmokeStage == 2) System::SetQuit();
		return;
	}
	if (State != Mode::Host) return;
	if (SmokeStage == 0 && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.Ready; })) {
		ClearOrbit = false;
		if (!StartGame()) { Verify("FAIL: " + Error); System::SetQuit(); return; }
		SmokeStage = 1;
	}
	auto* game = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
	if (!game) return;
	if (SmokeStage == 1 && game->GetActivityState() == Activity::Editing) {
		for (int player = 0; player <= SmokeGuests; ++player) {
			auto* brain = dynamic_cast<SceneObject*>(g_PresetMan.GetEntityPreset("Actor", "Brain Case")->Clone());
			const auto* area = g_SceneMan.GetScene()->GetArea(game->GetTeamOfPlayer(player) == 0 ? "Red Brain" : "Green Brain");
			const Vector position = g_SceneMan.MovePointToGround(area->GetCenterPoint(), 25, 1);
			brain->SetPos(position); brain->SetTeam(game->GetTeamOfPlayer(player));
			g_SceneMan.GetScene()->SetResidentBrain(player, brain);
			game->GetEditorGUI(player)->SetCurrentObject(dynamic_cast<SceneObject*>(brain->Clone()));
			game->GetEditorGUI(player)->SetCursorPos(position);
			game->GetEditorGUI(player)->SetEditorGUIMode(SceneEditorGUI::DONEEDITING);
		}
		SmokeStage = 2; Verify("ENCOUNTER: placed brains in Cave Bunkers");
	}
	if ((SmokeStage == 1 || SmokeStage == 2) && game->GetActivityState() == Activity::Running && !WarmingGuests) {
		game->SetDeliveryDelay(1);
		for (int player = 0; player <= SmokeGuests; ++player) {
			game->SetLandingZone(Vector(1450 + player * 240, 0), player);
			game->AddOverridePurchase(dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("ACDropShip", "Dropship MK1", "Base.rte")), player);
			for (int soldier = 0; soldier < (SmokeCombatStress ? 6 : 1); ++soldier) {
				game->AddOverridePurchase(dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("AHuman", "Soldier Light", "Coalition.rte")), player);
				game->AddOverridePurchase(dynamic_cast<const SceneObject*>(g_PresetMan.GetEntityPreset("HDFirearm", "Assault Rifle", "Coalition.rte")), player);
			}
			Verify(game->CreateDelivery(player) ? "ENCOUNTER: native soldier delivery ordered" : "FAIL: native delivery order");
			game->SetObservationTarget(g_SceneMan.MovePointToGround(Vector(1570, 0), 40, 1), player); game->SetViewState(Activity::Observe, player);
		}
		SmokeStage = 3; SmokeEncounterStart = now; SmokeCapture = true;
		if (SmokeNativeBaseline) { World.Reset(); Verify("BASELINE: native AI battle with guest capture disabled"); }
	}
	if (SmokeStage == 3) {
		if (SmokeCombatStress && now - SmokeEncounterStart > 7000 && now - SmokeEncounterStart < 18000 && now - SmokeLastExplosion > 250) {
			SmokeLastExplosion = now;
			const auto* preset = g_PresetMan.GetEntityPreset("TDExplosive", "Frag Grenade", "Base.rte");
			auto* grenade = dynamic_cast<MOSRotating*>(preset->Clone()); grenade->SetPos(g_SceneMan.MovePointToGround(Vector(1570, 0), 100, 1)); grenade->GibThis(); delete grenade; ++SmokeExplosions;
		}
		for (int player = 0; player <= SmokeGuests; ++player) for (auto* actor : *g_MovableMan.GetTeamRoster(game->GetTeamOfPlayer(player))) if (actor->GetPresetName() == "Soldier Light") {
			SmokeEncounterSoldiers.insert(actor->GetUniqueID());
			if (actor->GetController()->IsState(ControlState::WEAPON_FIRE)) SmokeEncounterFired.insert(actor->GetUniqueID());
			if (actor->GetHealth() < 99) SmokeEncounterDamaged.insert(actor->GetUniqueID());
		}
		if (now - SmokeEncounterStart > 22000) {
			const bool passed = SmokeEncounterSoldiers.size() >= 2 && !SmokeEncounterFired.empty() && !SmokeEncounterDamaged.empty();
			Verify(std::string(passed ? "PASS: " : "FAIL: ") + "delivered soldiers=" + std::to_string(SmokeEncounterSoldiers.size()) + " firing=" + std::to_string(SmokeEncounterFired.size()) + " damaged=" + std::to_string(SmokeEncounterDamaged.size()));
			Verify("ENCOUNTER: peak nodes=" + std::to_string(SmokeEncounterPeakNodes) + " trails=" + std::to_string(SmokeEncounterPeakTrails) + " wire bytes=" + std::to_string(SmokeEncounterPeakBytes) + " explosions=" + std::to_string(SmokeExplosions));
			Verify("ENCOUNTER: peak compressed bytes=" + std::to_string(SmokeEncounterPeakPacked) + " mean compressed bytes=" + std::to_string(SmokeEncounterWireUpdates ? SmokeEncounterWireBytes / SmokeEncounterWireUpdates : 0));
			if (!SmokeCaptureTimes.empty()) {
				std::sort(SmokeCaptureTimes.begin(), SmokeCaptureTimes.end()); uint64_t total = 0; for (auto elapsed : SmokeCaptureTimes) total += elapsed;
				Verify("ENCOUNTER: capture mean ms=" + std::to_string(total / (1000.0 * SmokeCaptureTimes.size())) + " p95 ms=" + std::to_string(SmokeCaptureTimes[SmokeCaptureTimes.size() * 95 / 100] / 1000.0));
				const double divisor = 1000.0 * SmokeCaptureTimes.size();
				Verify("ENCOUNTER: capture parts ms GUI=" + std::to_string(SmokeCaptureParts[0] / divisor) + " scene=" + std::to_string(SmokeCaptureParts[1] / divisor) + " serialize/resources=" + std::to_string(SmokeCaptureParts[2] / divisor) + " compress/packet=" + std::to_string(SmokeCaptureParts[3] / divisor));
			}
			SmokeCapture = true; SmokeStage = 4; ReturnToLobby(); SmokeEncounterStart = now;
		}
	}
	if (SmokeStage == 4 && now - SmokeEncounterStart > 1500) { Stop(); System::SetQuit(); }
}
void MultiplayerMan::Impl::DeploymentTick() {
	const auto now = Now();
	if (State == Mode::Idle && SmokeStage == 5) { System::SetQuit(true); return; }
	if (now - SmokeStarted > 60000) { Verify("FAIL: deployment timed out: " + Error); System::SetQuit(true); return; }
	if (State == Mode::Client) {
		if (!Playing && SmokeStage == 0 && !Players[LocalSlot].Ready) SendReady(true, Players[LocalSlot].Team);
		if (Playing && World.Ready() && !World.Paused() && SmokeStage == 0) { SmokeStage = 1; SmokeStageTime = SmokeDeploymentStart = now; SmokeDeploymentCount = Presented; SmokeCapture = true; }
		if (Playing && Presented && SmokeStage > 0) {
			const int phase = std::clamp(int((now - SmokeStageTime) / 3000), 0, 3);
			if (phase + 1 > SmokeStage) { SmokeStage = phase + 1; SmokeCapture = true; Verify("DEPLOYMENT: phase=" + std::to_string(phase) + " frames=" + std::to_string(Presented) + " FPS=" + std::to_string(FPS)); }
		}
		if (!Playing && SmokeStage > 0 && SmokeStage < 5) { Verify("DEPLOYMENT: average FPS=" + std::to_string(1000.0 * (Presented - SmokeDeploymentCount) / std::max<uint64_t>(1, now - SmokeDeploymentStart))); Verify(SmokeDeploymentFailed ? "FAIL: deployment visibility" : "PASS: deployment world remains visible through brain placement, troop placement and waiting"); SmokeStage = 5; }
		return;
	}
	if (State != Mode::Host) return;
	if (SmokeStage == 0 && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.Ready; })) { if (!StartGame()) { Verify("FAIL: " + Error); System::SetQuit(true); return; } SmokeStage = 1; }
	auto* game = dynamic_cast<GameActivity*>(g_ActivityMan.GetActivity());
	if (Playing && WarmingGuests) SmokeStageTime = now;
	if (SmokeStage == 1 && game && game->GetPresetName() == "Test Activity" && game->GetActivityState() == Activity::Editing) {
		// Exercise activities that initialize an opaque fog layer before combat.
		// Placement must show the world without revealing that layer permanently.
		for (int team = 0; team < 4; ++team) g_SceneMan.MakeAllUnseen(Vector(4, 4), team);
		for (int player = 0; player <= SmokeGuests; ++player) {
			const Vector pos = g_SceneMan.MovePointToGround(Vector(600 + player * 300, 0), 25, 1);
			game->GetEditorGUI(player)->SetCurrentObject(dynamic_cast<SceneObject*>(g_PresetMan.GetEntityPreset("Actor", "Brain Case")->Clone()));
			game->GetEditorGUI(player)->SetCursorPos(pos); game->GetEditorGUI(player)->SetEditorGUIMode(SceneEditorGUI::INSTALLINGBRAIN);
		}
		SmokeStage = 2; SmokeStageTime = now; Verify("DEPLOYMENT: brain placement"); SmokeCapture = true;
	}
	if (SmokeStage == 2 && now - SmokeStageTime > 3000) {
		for (int player = 0; player <= SmokeGuests; ++player) {
			const Vector pos = g_SceneMan.MovePointToGround(Vector(600 + player * 300, 0), 25, 1);
			auto* brain = dynamic_cast<SceneObject*>(g_PresetMan.GetEntityPreset("Actor", "Brain Case")->Clone()); brain->SetPos(pos); brain->SetTeam(game->GetTeamOfPlayer(player)); g_SceneMan.GetScene()->SetResidentBrain(player, brain);
			auto* troop = dynamic_cast<SceneObject*>(g_PresetMan.GetEntityPreset("AHuman", "Soldier Light", "Coalition.rte")->Clone()); troop->SetPos(pos + Vector(50, 0)); troop->SetTeam(game->GetTeamOfPlayer(player)); g_SceneMan.GetScene()->AddPlacedObject(Scene::PLACEONLOAD, troop);
			game->GetEditorGUI(player)->SetCurrentObject(dynamic_cast<SceneObject*>(troop->Clone())); game->GetEditorGUI(player)->SetEditorGUIMode(SceneEditorGUI::ADDINGOBJECT);
		}
		SmokeStage = 3; Verify("DEPLOYMENT: troop placement"); SmokeCapture = true;
	}
	if (SmokeStage == 3 && now - SmokeStageTime > 6000) { for (int player = 1; player <= SmokeGuests; ++player) game->GetEditorGUI(player)->SetEditorGUIMode(SceneEditorGUI::DONEEDITING); SmokeStage = 4; Verify("DEPLOYMENT: guests ready, waiting for host"); SmokeCapture = true; }
	if (SmokeStage == 4 && now - SmokeStageTime > 9000) { game->GetEditorGUI(0)->SetEditorGUIMode(SceneEditorGUI::DONEEDITING); SmokeStage = 5; Verify("DEPLOYMENT: all ready, combat"); SmokeCapture = true; }
	if (SmokeStage == 5 && now - SmokeStageTime > 12000) { bool passed = !SmokeDeploymentFailed && game->GetActivityState() == Activity::Running; for (int player = 1; player <= SmokeGuests; ++player) { Verify("DEPLOYMENT: player=" + std::to_string(player) + " ack=" + std::to_string(Players[player].AckID)); passed &= Players[player].AckID > 20 && g_SceneMan.IsUnseen(10, g_SceneMan.GetSceneHeight() - 20, game->GetTeamOfPlayer(player)); } Verify(passed ? "PASS: deployment completed into combat with fog preserved" : "FAIL: deployment visibility, combat transition or fog preservation"); ReturnToLobby(); SmokeStage = 6; SmokeStageTime = now; }
	if (SmokeStage == 6 && now - SmokeStageTime > 1000) { Stop(); System::SetQuit(true); }
}

void MultiplayerMan::Impl::SendAudio(int player) {
	std::list<AudioMan::NetworkSoundData> events; g_AudioMan.GetSoundEvents(player, events);
	auto& peer = Players[player];
	std::vector<const AudioMan::NetworkSoundData*> ordered;
	std::map<std::pair<int, uint8_t>, size_t> updates;
	for (const auto& event: events) {
		if (event.State == AudioMan::SOUND_STOP || event.State == AudioMan::SOUND_FADE_OUT || event.State == AudioMan::SOUND_PLAY) {
			peer.Loops.erase(event.Channel);
			std::erase_if(updates, [&](const auto& entry) { return entry.first.first == event.Channel; });
		}
		if (event.State == AudioMan::SOUND_PLAY && event.Loops != 0 && peer.Loops.size() < 512) peer.Loops[event.Channel] = event;
		else if (auto loop = peer.Loops.find(event.Channel); loop != peer.Loops.end() && event.State != AudioMan::SOUND_SET_GLOBAL_PITCH) { loop->second = event; loop->second.State = AudioMan::SOUND_PLAY; }
		// Coalesce property changes while preserving play/stop ordering. Stops and
		// fades must never be discarded behind a burst of gunfire or frame data.
		if (event.State != AudioMan::SOUND_PLAY && event.State != AudioMan::SOUND_STOP && event.State != AudioMan::SOUND_FADE_OUT) {
			const auto key = std::make_pair(event.Channel, event.State);
			if (auto previous = updates.find(key); previous != updates.end()) ordered[previous->second] = nullptr;
			updates[key] = ordered.size();
		}
		ordered.push_back(&event);
	}
	std::erase_if(peer.Loops, [](const auto& entry) { FMOD::Channel* channel = nullptr; bool playing = false; auto* audio = g_AudioMan.GetAudioSystem(); return !audio || audio->getChannel(entry.first, &channel) != FMOD_OK || channel->isPlaying(&playing) != FMOD_OK || !playing; });
	if (!peer.Connected || !peer.AudioReady) return;
	if (Net.QueuedBytes(peer.Address) > 512 * 1024) { Net.Close(peer.Address); peer.Connected = peer.Ready = false; peer.ReservedUntil = Now() + 60000; peer.Inputs.ReleaseControls(); ClearWorldPackets(peer); g_UInputMan.ClearRemoteInput(player); Lobby(); return; }
	auto send = [&](const AudioMan::NetworkSoundData& event) {
		const auto path = event.State == AudioMan::SOUND_PLAY ? ContentFile::GetPathFromHash(event.SoundFileHash) : "";
		if (event.State == AudioMan::SOUND_PLAY && !AssetPath(path)) return;
		MP::Writer writer(Kind::Sound, Session, Epoch); writer.U8(event.State); writer.U32(static_cast<uint32_t>(event.Channel)); writer.Text(path, 240); writer.U8(event.Immobile); writer.U8(event.AffectedByGlobalPitch);
		writer.U32(static_cast<uint32_t>(event.Loops)); writer.U32(static_cast<uint32_t>(event.Priority)); writer.U32(static_cast<uint32_t>(event.FadeOutTime));
		writer.F32(event.AttenuationStartDistance); writer.F32(event.CustomPanValue); writer.F32(event.PanningStrengthMultiplier); writer.F32(event.Position[0]); writer.F32(event.Position[1]); writer.F32(event.Volume); writer.F32(event.Pitch);
		Net.Send(peer.Address, writer.Data, Delivery::Audio);
	};
	size_t oneShots = 0;
	for (const auto* event: ordered) {
		if (!event) continue;
		if (event->State == AudioMan::SOUND_PLAY) {
			if (event->Loops != 0 && peer.ReplayLoops) continue;
			if (event->Loops == 0 && (++oneShots > 256 || Net.QueuedBytes(peer.Address) > 128 * 1024)) continue;
		}
		send(*event);
	}
	if (peer.ReplayLoops) {
		for (const auto& [channel, loop]: peer.Loops) send(loop);
		AudioMan::NetworkSoundData pitch{}; pitch.State = AudioMan::SOUND_SET_GLOBAL_PITCH; pitch.Pitch = g_AudioMan.GetGlobalPitch(); send(pitch);
		peer.ReplayLoops = false;
	}
}

void MultiplayerMan::Impl::HandleAudio(MP::Reader& reader) {
	uint8_t state, immobile, global; uint32_t channel, loops, priority, fade; std::string path; std::array<float, 7> values;
	if (!reader.U8(state) || state > AudioMan::SOUND_FADE_OUT || !reader.U32(channel) || !reader.Text(path, 240) || !reader.U8(immobile) || immobile > 1 || !reader.U8(global) || global > 1 || !reader.U32(loops) || !reader.U32(priority) || priority > 256 || !reader.U32(fade) || fade > 600000) return;
	for (auto& value: values) if (!reader.F32(value)) return;
	if (!reader.Done() || values[0] < 0 || values[0] > 100000 || std::abs(values[1]) > 1 || values[2] < 0 || values[2] > 10 || values[5] < 0 || values[5] > 10 || values[6] < 0.01f || values[6] > 10) return;
	++AudioReceived;
	if (state == AudioMan::SOUND_SET_GLOBAL_PITCH) { g_AudioMan.SetGlobalPitch(values[6]); return; }
	const int key = std::bit_cast<int32_t>(channel);
	if (state == AudioMan::SOUND_PLAY) {
		if (!AssetPath(path) || (loops != 0xffffffffu && loops > 10000)) return;
		if (Sounds.size() >= 512 && !Sounds.contains(key)) return;
		if (!std::filesystem::is_regular_file(g_PresetMan.GetFullModulePath(path))) return;
		if (Smoke && path.ends_with("Base.rte/Sounds/Craft/ThrusterLoop.flac") && std::abs(values[5] - 0.013f) < 0.0001f) { SmokeLoopChannel = key; ++SmokeLoopPlays; Verify("AUDIO: looping sound play " + std::to_string(SmokeLoopPlays)); }
		if (auto found = Sounds.find(key); found != Sounds.end()) found->second->Stop();
		auto sound = std::make_unique<SoundContainer>(); sound->GetTopLevelSoundSet().AddSound(path, false);
		sound->SetImmobile(immobile != 0); sound->SetAffectedByGlobalPitch(global != 0); sound->SetLoopSetting(std::bit_cast<int32_t>(loops)); sound->SetPriority(static_cast<int>(priority));
		sound->SetAttenuationStartDistance(values[0]); sound->SetCustomPanValue(values[1]); sound->SetPanningStrengthMultiplier(values[2]); sound->SetPosition(Vector(values[3], values[4])); sound->SetVolume(values[5]); sound->SetPitch(values[6]); sound->Play(); Sounds[key] = std::move(sound); return;
	}
	auto found = Sounds.find(key); if (found == Sounds.end()) return;
	auto& sound = *found->second;
	switch (state) {
		case AudioMan::SOUND_STOP: if (Smoke && key == SmokeLoopChannel) { SmokeLoopStopped = true; Verify("AUDIO: looping sound stopped"); } sound.Stop(); Sounds.erase(found); break;
		case AudioMan::SOUND_SET_POSITION: sound.SetPosition(Vector(values[3], values[4])); break;
		case AudioMan::SOUND_SET_VOLUME: sound.SetVolume(values[5]); break;
		case AudioMan::SOUND_SET_CUSTOMPANVALUE: sound.SetCustomPanValue(values[1]); break;
		case AudioMan::SOUND_SET_PANNINGSTRENGTHMULTIPLIER: sound.SetPanningStrengthMultiplier(values[2]); break;
		case AudioMan::SOUND_SET_PITCH: sound.SetPitch(values[6]); break;
		case AudioMan::SOUND_FADE_OUT: sound.FadeOut(static_cast<int>(fade)); break;
		default: break;
	}
}

std::string MultiplayerMan::Impl::StartBlock() const {
	if (State != Mode::Host || Playing) return "";
	if (Online && !Net.IsRelayReady()) return "Connecting to the room server...";
	if (Activities.empty() || Scenes.empty()) return "Choose an activity with a compatible battlefield.";
	int count = 0;
	std::array<bool, 4> teams{};
	for (const auto& player: Players) {
		if (player.Token && !player.Connected) return "Waiting for a disconnected player. Release their slot to continue.";
		if (player.Connected) {
			if (!player.Ready) return "Waiting for every player to ready up.";
			if (player.Team >= 4 || !(AvailableTeams & (1 << player.Team))) return "Choose an available team for each player.";
			++count; teams[player.Team] = true;
		}
	}
	const auto* activity = Activities[ActivityIndex];
	if (count > activity->GetMaxPlayerSupport()) return "This activity supports up to " + std::to_string(activity->GetMaxPlayerSupport()) + " players.";
	if (const int cpu = activity->GetCPUTeam(); cpu >= 0 && cpu < 4) teams[cpu] = true;
	if (std::count(teams.begin(), teams.end(), true) < activity->GetMinTeamsRequired()) return "Choose at least " + std::to_string(activity->GetMinTeamsRequired()) + " different teams for this activity.";
	return "";
}

MultiplayerMenuGUI::View MultiplayerMan::Impl::MenuView() const {
	MultiplayerMenuGUI::View view;
	view.Host = State == Mode::Host; view.Online = Online; view.LocalSlot = LocalSlot; view.Played = Played;
	view.Page = State == Mode::Idle ? MultiplayerMenuGUI::Screen::Entry : State == Mode::Connecting || State == Mode::Reconnecting ? MultiplayerMenuGUI::Screen::Connecting : Playing ? UI ? MultiplayerMenuGUI::Screen::Session : MultiplayerMenuGUI::Screen::Loading : MultiplayerMenuGUI::Screen::Lobby;
	view.Name = Name; view.Room = Room; view.Password = Password; view.Service = ServiceAddress; view.DefaultService = DefaultService;
	view.Code = State == Mode::Idle || State == Mode::Connecting || State == Mode::Reconnecting ? JoinCode : Relay::DisplayCode(Net.RoomCode());
	view.Address = State == Mode::Idle ? HostAddress : ServerAddress;
	if (!Online && State == Mode::Host) { const auto addresses = Net.LocalAddresses(static_cast<uint16_t>(Port)); view.Address = addresses.empty() ? "127.0.0.1:" + std::to_string(Port) : addresses.front(); }
	view.Ready = Players[LocalSlot].Ready; view.Fog = Fog; view.Deploy = Deploy; view.ClearOrbit = ClearOrbit;
	view.Difficulty = Difficulty; view.Gold = Gold; view.Bandwidth = BandwidthMbps; view.Port = Port;
	view.Activity = ActivityName; view.SceneName = SceneName; view.Tech = Tech; view.CPUTeam = int(CPUTeam) - 1;
	view.Error = Error; view.Notice = Notice; view.StartBlock = StartBlock(); view.Chat = Chat; view.Factions = Factions;
	if (WarmingGuests) view.LoadingMessage = "Waiting for your group to finish loading the battlefield.";
	else if (State == Mode::Client && Playing && World.Paused()) view.LoadingMessage = "Waiting for your group to finish loading the battlefield.";
	else if (State == Mode::Client && Playing && !World.Ready()) {
		view.LoadingMessage = SceneReceived ? "Preparing the terrain and scenery: " + std::to_string(SceneManifest.size() - std::min(SceneManifest.size(), SceneMissing.size())) + " / " + std::to_string(SceneManifest.size()) : "Waiting for the battlefield from your host...";
	}
	view.Title = State == Mode::Idle ? "M U L T I P L A Y E R" : Playing ? "M A T C H   S E S S I O N" : "M U L T I P L A Y E R   L O B B Y";
	view.Subtitle = State == Mode::Idle ? "Gather your group. Choose a battlefield. Stay together between rounds." : State == Mode::Reconnecting ? "Connection interrupted - restoring your original player slot." : State == Mode::Connecting ? "Joining your host..." : RoomName + (view.Host ? "  /  You are the host" : "  /  " + std::to_string(Net.Ping(ServerAddress)) + " ms");
	if (State == Mode::Client && Playing) view.Subtitle += "  /  " + std::to_string(static_cast<int>(FPS)) + " fps";
	for (const auto* activity: Activities) view.Activities.push_back(activity->GetPresetName());
	for (const auto* scene: Scenes) view.Scenes.push_back(scene->GetPresetName());
	view.ActivityIndex = ActivityIndex; view.SceneIndex = SceneIndex;
	const GameActivity* selected = nullptr;
	if (State == Mode::Host && !Activities.empty()) selected = Activities[ActivityIndex];
	else { std::list<Entity*> activities; g_PresetMan.GetAllOfType(activities, "GameActivity"); for (const auto* entity: activities) if (entity->GetPresetName() == ActivityName) { selected = dynamic_cast<const GameActivity*>(entity); break; } }
	for (int team = 0; team < 4; ++team) {
		view.TeamNames[team] = selected ? selected->GetTeamName(team) : "Team " + std::to_string(team + 1);
		if (view.TeamNames[team].empty()) view.TeamNames[team] = "Team " + std::to_string(team + 1);
		if (AvailableTeams & (1 << team)) view.HumanTeams.push_back(team);
	}
	if (State == Mode::Client) {
		view.Activities = {ActivityName}; view.Scenes = {SceneName}; view.ActivityIndex = view.SceneIndex = 0;
	}
	view.SelectedScene = State == Mode::Host && !Scenes.empty() ? Scenes[SceneIndex] : dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", SceneName));
	view.Description = selected ? selected->GetDescription() : "";
	if (view.Factions.empty()) { view.Factions = {"-All-"}; for (int i = 0; i < g_PresetMan.GetTotalModuleCount(); ++i) if (g_PresetMan.GetDataModule(i)->IsFaction()) view.Factions.push_back(g_PresetMan.GetDataModuleName(i)); }
	for (const auto& faction: view.Factions) {
		const int id = g_PresetMan.GetModuleID(faction);
		view.FactionLabels.push_back(id >= 0 ? g_PresetMan.GetDataModule(id)->GetFriendlyName() : "All factions");
	}
	for (int i = 0; i < 4; ++i) {
		const auto& player = Players[i]; auto& slot = view.Players[i];
		slot.Name = player.Name; slot.Team = player.Team; slot.TeamName = view.TeamNames[player.Team];
		slot.Occupied = player.Token != 0; slot.Connected = player.Connected; slot.Ready = player.Ready;
		slot.Editable = !Playing && player.Connected && (view.Host || i == LocalSlot);
		slot.Status = !player.Token ? "Invite a friend" : !player.Connected ? "Reconnecting..." : Playing ? "In match" : player.Ready ? "READY" : "Not ready";
	}
	for (const auto& [address, room]: Discovered) view.LANRooms.push_back(room);
	return view;
}

void MultiplayerMan::Impl::UpdateMenu() {
    const bool visible = UI || State == Mode::Connecting || State == Mode::Reconnecting || (State == Mode::Client && Playing && (!Texture || World.Paused()));
	if (!visible) { if (Menu) Menu->Hide(); return; }
	if (!Menu) Menu = std::make_unique<MultiplayerMenuGUI>();
	const auto view = MenuView();
	for (const auto& event: Menu->Update(view)) {
		const auto& control = event.Control;
		auto copy = [&](auto& target) { std::snprintf(target, sizeof(target), "%s", event.Text.c_str()); };
		auto number = [&](int& target, int minimum, int maximum) {
			int value; const auto parsed = std::from_chars(event.Text.data(), event.Text.data() + event.Text.size(), value);
			if (parsed.ec == std::errc() && parsed.ptr == event.Text.data() + event.Text.size()) target = std::clamp(value, minimum, maximum);
		};
		if (control == "Name") copy(Name);
		else if (control == "Room") copy(Room);
		else if (control == "Password") copy(Password);
		else if (control == "Code") copy(JoinCode);
		else if (control == "Address") copy(HostAddress);
		else if (control == "Service") copy(ServiceAddress);
		else if (control == "Bandwidth") number(BandwidthMbps, 6, 48);
		else if (control == "Port") number(Port, 1, 65535);
		else if (control == "Quality") Quality = std::clamp(event.Value, 0, 1);
		else if (control == "Online") { Online = event.Value == 0; Error.clear(); }
		else if (control == "SaveService" || control == "DefaultService") {
			if (control == "DefaultService") std::snprintf(ServiceAddress, sizeof(ServiceAddress), "%s", DefaultService.c_str());
			std::string host; uint16_t port;
			if (Address(ServiceAddress, host, port, 8001)) { SaveService(); Error.clear(); Notice = "Room server saved."; }
			else Error = "Enter the room server hostname or IP address, optionally followed by :port.";
		} else if (control == "Join") { if (Join()) Notice.clear(); }
		else if (control == "Create") Host();
		else if (control == "Discover") { Discovered.clear(); if (Net.Start(false, 0, "", Error)) Net.Discover(8000); }
		else if (control == "LANRooms" && event.Value >= 0 && event.Value < Discovered.size()) { auto room = Discovered.begin(); std::advance(room, event.Value); std::snprintf(HostAddress, sizeof(HostAddress), "%s", room->first.c_str()); }
		else if (control == "EntryBack") { Stop(); UI = false; g_MenuMan.SetMultiplayerMenuBackground(false); }
		else if (control == "Cancel") { Stop(); UI = true; }
		else if (control == "Copy") { SDL_SetClipboardText((Online ? Relay::DisplayCode(Net.RoomCode()) : view.Address).c_str()); Notice = "Invitation copied."; }
		else if (control == "Primary") { if (State == Mode::Host) StartGame(); else if (State == Mode::Client && !Playing) SendReady(!Players[LocalSlot].Ready, Players[LocalSlot].Team); }
		else if (control == "Resume") { UI = false; Menu->Hide(); g_UInputMan.TrapMousePos(true); }
		else if (control == "Session") { UI = true; g_UInputMan.TrapMousePos(false); }
		else if (control == "Return") ReturnToLobby();
		else if (control == "ConfirmLeave") {
			if (State == Mode::Host && g_ActivityMan.GetActivity()) { g_ActivityMan.EndActivity(); g_ActivityMan.PauseActivity(); }
			Stop(); UI = true;
		} else if (control == "Send" && !event.Text.empty() && Now() - LastChatSent >= 250) {
			LastChatSent = Now();
			if (State == Mode::Host) AddChat(0, event.Text);
			else { MP::Writer writer(Kind::Chat, Session, Epoch); writer.Text(event.Text, 192); Net.Send(ServerAddress, writer.Data, Delivery::Control); }
		} else if (control.starts_with("Team") && control.size() == 5 && event.Value >= 0 && event.Value < view.HumanTeams.size() && !Playing) {
			const int slot = control.back() - '0'; if (slot < 0 || slot >= 4 || !Players[slot].Connected) continue;
			const uint8_t team = static_cast<uint8_t>(view.HumanTeams[event.Value]);
			if (State == Mode::Host) { Players[slot].Team = team; Players[slot].Ready = slot == 0; Error.clear(); Lobby(); }
			else if (slot == LocalSlot) SendReady(false, team);
		} else if (State == Mode::Host && !Playing) {
			bool changed = false;
			if (control == "Activity" && event.Value >= 0 && event.Value < Activities.size()) { ActivityIndex = event.Value; LoadScenes(); changed = true; }
			else if (control == "Scene" && event.Value >= 0 && event.Value < Scenes.size()) { SceneIndex = event.Value; SceneName = Scenes[SceneIndex]->GetPresetName(); changed = true; }
			else if (control == "Gold") { const int old = Gold; number(Gold, 0, 1000000); changed = old != Gold; }
			else if (control == "Difficulty") { const int old = Difficulty; number(Difficulty, 0, 100); changed = old != Difficulty; }
			else if (control == "Fog") { Fog = event.Value != 0; changed = true; }
			else if (control == "Deploy") { Deploy = event.Value != 0; changed = true; }
			else if (control == "Orbit") { ClearOrbit = event.Value != 0; changed = true; }
			else if (control.starts_with("Faction") && control.size() == 8 && event.Value >= 0 && event.Value < Factions.size()) { const int team = control.back() - '0'; if (team >= 0 && team < 4) { Tech[team] = Factions[event.Value]; changed = true; } }
			else if (control.starts_with("Release") && control.size() == 8) { const int slot = control.back() - '0'; if (slot > 0 && slot < 4 && !Players[slot].Connected) { Net.Close(Players[slot].Address); Players[slot] = Player(); changed = true; } }
			if (changed) { Error.clear(); Notice = "Match settings changed. Everyone must ready up again."; SettingsChanged(); }
		}
	}
}

void MultiplayerMan::Impl::Draw() {
    if (std::getenv("CCCP_MPSMOKE_PRESENTATION")) {
        std::ofstream log("build-mp/presentation-smoke.log");
        const bool presentation = World.VerifyPresentation(log);
        const bool flow = VerifyWorldFlow(log);
        const bool passed = presentation && flow;
        log << (passed ? "PASS: native multiplayer HUD presentation" : "FAIL: native multiplayer HUD presentation") << '\n';
        System::SetQuit(); return;
    }
	if (CursorVerification) {
		const auto& test = CursorCases[CursorStage]; State = test.State; UI = test.UI; Playing = test.Playing;
		LocalSlot = State == Mode::Client ? 1 : 0;
		if (CursorPreparedStage != CursorStage) { CursorFrames = 0; if (CursorStage == 0) g_MenuMan.SetMultiplayerMenuBackground(true); }
		if (Activities.empty()) LoadActivities();
		RoomName = "Multiplayer lobby"; Players[0].Name = "Host"; Players[0].Token = 1; Players[0].Connected = Players[0].Ready = true;
	}
	ImGui::GetIO().MouseDrawCursor = false;
	if (State == Mode::Client && Playing && World.Ready()) {
		World.SetLocalInput(LocalInput, !UI && Controls && !World.Paused());
		Texture = World.Render(Now()); TextureWidth = World.Width(); TextureHeight = World.Height();
		const uint64_t now = Now(); if (LastRender && now > LastRender) FPS = FPS * 0.9f + 100.0f / float(now - LastRender); LastRender = now;
	}
	if (State == Mode::Client && Playing && Texture) {
		const auto* viewport = ImGui::GetMainViewport(); const float scale = std::min(viewport->Size.x / TextureWidth, viewport->Size.y / TextureHeight);
		const ImVec2 size(TextureWidth * scale, TextureHeight * scale), position(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f, viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f);
		auto* draw = ImGui::GetBackgroundDrawList(); draw->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(0, 0, 0, 255)); draw->AddImage(static_cast<ImTextureID>(Texture), position, ImVec2(position.x + size.x, position.y + size.y), ImVec2(0, 1), ImVec2(1, 0));
	}
	const bool visible = UI || WarmingGuests || State == Mode::Connecting || State == Mode::Reconnecting || (!CursorVerification && State == Mode::Client && Playing && (!Texture || World.Paused()));
	if (!visible) { if (Menu) Menu->Hide(); if (CursorVerification) CursorPreparedStage = static_cast<int>(CursorStage); return; }
	if (!Menu) Menu = std::make_unique<MultiplayerMenuGUI>();
	auto view = MenuView();
	if (CursorVerification) {
		const auto& test = CursorCases[CursorStage];
		if (CursorPreparedStage != CursorStage) Menu->SetVerificationPage(test.Page);
		CursorPreparedStage = static_cast<int>(CursorStage);
		view.Code = "ABCD-EFGH-JK";
		view.Error.clear(); view.StartBlock = "Waiting for every player to ready up.";
		view.Players[0].Editable = view.Host;
		view.Players[1] = {"Player 2", "Team 2", "Not ready", 1, true, true, false, view.Host || State == Mode::Client};
		view.Players[2] = {"Player 3", "Team 1", "READY", 0, true, true, true, view.Host};
		view.Chat = {"Host: Welcome to the lobby.", "Player 2: Ready for another round!"};
		view.Played = std::string(test.Name) == "post-match-lobby";
		if (view.Played) view.Notice = "Round complete. Ready up for the next match.";
		Menu->Update(view);
	}
	Menu->Draw(view);
}
