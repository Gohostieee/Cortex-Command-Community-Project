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
#include "SLTerrain.h"
#include "ThreadMan.h"
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
#include "lz4hc.h"


#include <charconv>
#include <cstdlib>

#include <random>
#include <unordered_map>
#include <fstream>
#include <filesystem>
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
#include "TimerMan.h"
#include "GUI/GUI.h"
#include "BuyMenuGUI.h"
#include "GUI/GUICollectionBox.h"
#include "GUI/GUIListBox.h"

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
		std::unordered_set<int> AudioChannels;
		std::map<std::pair<int, int>, AudioMan::NetworkSoundData> PendingSoundChanges;
		std::pair<int, int> AudioCursor{};
		double AudioBudget = 0;
		uint64_t LastAudioBudget = 0, LastAudioChanges = 0;
		float SentGlobalPitch = -1;
		uint8_t Team = 0;
		// The newest lobby edit from this player's client the host has applied
		// or rejected; it lets the client retire exactly the edits answered.
		uint32_t AppliedEdit = 0;
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
		// Snapshots follow a fixed real-time schedule; re-arming from the capture
		// time instead lost a 16.7 ms tick in every other interval (about 17 Hz).
		uint64_t NextWorld = 0;
		uint64_t LastPump = 0, LastObsoleteSweep = 0;
		uint64_t BytesSent = 0;
		// The configured upload is a ceiling; the actual rate follows the link.
		MP::World::SendRate Rate;
		uint32_t QueuedSnapshotFrame = 0;
		std::array<std::pair<uint32_t, uint64_t>, 16> SnapshotSentAt{};
		// Layer tiles the guest holds, from its manifest or acknowledged snapshots.
		// A tile repeats in every snapshot until one carrying it is acknowledged.
		std::unordered_map<uint64_t, uint64_t> AckedLayers;
		struct SentLayerFrame { uint32_t Frame = 0; std::vector<std::pair<uint64_t, uint64_t>> Layers; };
		std::array<SentLayerFrame, 32> SentLayers;
		// Scene preload resources stay queued even when no snapshot references them.
		std::unordered_set<uint64_t> SceneResources;
		// States as sent, by frame; an acknowledged one is the next delta baseline.
		std::array<MP::World::Snapshot, 32> SentStates;
		bool SnapshotStarted = false;
		bool SceneSent = false;
		std::deque<uint64_t> SceneQueue;
		// Tiles the guest's current view lacks overtake the rest of the map.
		std::deque<uint64_t> UrgentQueue;
		std::unordered_set<uint64_t> Urgent;
		// Resources the latest state shows that the guest lacks, rebuilt each
		// capture. They follow view tiles and precede the rest of the map.
		std::deque<uint64_t> StateQueue;
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
	// Opt-in benchmark traces contain counters and timings, never session credentials.
	bool Benchmark = std::getenv("CCCP_MPBENCHMARK") != nullptr;
	bool BenchmarkUploadRequested = false;
	std::ofstream BenchmarkLog;
	uint64_t BenchmarkLastSample = 0;
	void BenchmarkRecord(const char* event, uint64_t now, uint64_t delta) {
		if (!Benchmark) return;
		if (!BenchmarkLog.is_open()) {
			BenchmarkLog.open(SmokeLogDirectory + "/" + SmokeRole + "-benchmark.csv");
			BenchmarkLog << "event,time_ms,epoch,stage,playing,ready,paused,delta_ms,updates,rendered,ping_ms,world_hz,upload_mbps\n";
		}
		BenchmarkLog << event << ',' << now << ',' << Epoch << ',' << SmokeStage << ',' << Playing << ',' << World.Ready() << ',' << World.Paused() << ',' << delta << ',' << World.Updates() << ',' << World.Rendered() << ',' << Net.Ping(ServerAddress) << ',' << RecentWorldUpdates.size() << ',' << BandwidthMbps << '\n';
	}
	// Per-second attribution of the authority's single main loop. Every stage
	// that competes with simulation time is measured separately.
	struct ServerProfile {
		enum Stage { Objects, View, Compose, Encode, Queue, StageCount };
		uint64_t WindowStart = 0, Loops = 0, SimulationUpdates = 0, Captures = 0, CaptureLoops = 0;
		uint64_t NetworkUs = 0, SimulationUs = 0, DrawUs = 0, IdleUs = 0, LoopMaxUs = 0;
		long long SimulationTicks = 0;
		std::array<uint64_t, PerformanceMan::PerfCounterCount> SimulationStageUs{};
		std::array<uint64_t, StageCount> CaptureUs{};
		uint64_t PackedBytes = 0, StageStart = 0;
	} Profile;
	std::ofstream ProfileLog;
	void ProfileStage(ServerProfile::Stage stage, uint64_t start) { if (Benchmark) Profile.CaptureUs[stage] += NowMicros() - start; }
	void WriteProfile(uint64_t now) {
		auto& p = Profile;
		if (!ProfileLog.is_open()) {
			const auto directory = Dedicated ? (std::filesystem::path(System::GetUserdataDirectory()) / "verification").string() : SmokeLogDirectory;
			std::filesystem::create_directories(directory);
			ProfileLog.open(directory + "/server-benchmark.csv");
			ProfileLog << "time_ms,playing,guests,loops,sim_updates,sim_speed,loop_ms,loop_max_ms,network_ms,simulation_ms,draw_ms,idle_ms,update_ms";
			for (const char* name : {"ai_ms", "actor_travel_ms", "actor_update_ms", "particle_travel_ms", "particle_update_ms", "activity_ms", "scripts_ms"}) ProfileLog << ',' << name;
			ProfileLog << ",snapshots,objects_ms,view_ms,compose_ms,encode_ms,queue_ms,snapshot_bytes,sent_kbps,particles,actors,moids,rate_kbps,queue_delay_ms,base_rtt_ms,compose_layers_ms,compose_trails_ms,compose_objects_ms,compose_foreground_ms,compose_canvas_ms,compose_bound_ms\n";
		}
		double rate = 0; uint64_t queueDelay = 0, baseRtt = 0; int rated = 0;
		for (const auto& player : Players) if (player.Connected && player.Rate.Cap()) { rate += player.Rate.Rate(); queueDelay = std::max(queueDelay, player.Rate.QueueDelay()); if (player.Rate.BaseRtt() != UINT64_MAX) baseRtt = std::max(baseRtt, player.Rate.BaseRtt()); ++rated; }
		const double seconds = std::max<uint64_t>(1, now - p.WindowStart) / 1000.0;
		const double loops = double(std::max<uint64_t>(1, p.Loops)), updates = double(std::max<uint64_t>(1, p.SimulationUpdates)), captures = double(std::max<uint64_t>(1, p.Captures));
		uint64_t sent = 0; int guests = 0;
		for (auto& player : Players) if (player.Connected) { sent += player.BytesSent; player.BytesSent = 0; ++guests; }
		ProfileLog << now << ',' << Playing << ',' << guests << ',' << p.Loops << ',' << p.SimulationUpdates << ',' << (g_TimerMan.GetSimTickCount() - p.SimulationTicks) / 1000.0 / (seconds * 1000.0)
			<< ',' << (p.NetworkUs + p.SimulationUs + p.DrawUs + p.IdleUs) / 1000.0 / loops << ',' << p.LoopMaxUs / 1000.0 << ',' << p.NetworkUs / 1000.0 / loops << ',' << p.SimulationUs / 1000.0 / loops << ',' << p.DrawUs / 1000.0 / loops << ',' << p.IdleUs / 1000.0 / loops
			<< ',' << p.SimulationStageUs[PerformanceMan::SimTotal] / 1000.0 / updates;
		for (int counter = PerformanceMan::ActorsAI; counter < PerformanceMan::PerfCounterCount; ++counter) ProfileLog << ',' << p.SimulationStageUs[counter] / 1000.0 / updates;
		ProfileLog << ',' << p.Captures << ',' << p.CaptureUs[ServerProfile::Objects] / 1000.0 / double(std::max<uint64_t>(1, p.CaptureLoops));
		for (int stage = ServerProfile::View; stage < ServerProfile::StageCount; ++stage) ProfileLog << ',' << p.CaptureUs[stage] / 1000.0 / captures;
		ProfileLog << ',' << p.PackedBytes / captures << ',' << sent * 8 / 1000.0 / seconds << ',' << g_MovableMan.GetParticleCount() << ',' << g_MovableMan.GetActorCount() << ',' << g_MovableMan.GetMOIDCount()
			<< ',' << (rated ? rate / rated * 8 / 1000.0 : 0) << ',' << queueDelay << ',' << baseRtt;
		for (const auto stage : MultiplayerWorld::TakeComposeTimes()) ProfileLog << ',' << stage / 1000.0 / captures;
		ProfileLog << '\n';
		ProfileLog.flush();
		p = ServerProfile(); p.WindowStart = now; p.SimulationTicks = g_TimerMan.GetSimTickCount();
	}
	char Name[32] = "Player", Room[64] = "Cortex room", HostAddress[256] = "127.0.0.1:8000", Password[64] = "";
	char ServiceAddress[256] = "", JoinCode[32] = "";
	bool Online = true, Hosted = true, Dedicated = false;
	int OwnerSlot = 0;
	bool OwnerRegistered = false, HostedEndpoint = false;
	uint64_t CloseStarted = 0, CloseRetry = 0;
	struct ResumeSlot { uint64_t Token, Expires, Session; uint32_t Epoch; Input Inputs; };
	bool ResumingSlot = false;
	std::map<std::string, ResumeSlot> ResumeSlots;
	std::string ResumeKey() const { return std::string(ServiceAddress) + "/" + Relay::NormalizeCode(JoinCode); }
	std::string OwnerCredential, DedicatedStatus;
	uint64_t DedicatedLastOccupied = 0, DedicatedLastStatus = 0, OwnerClaimDeadline = 0;
	unsigned DedicatedIdleSeconds = 600;
	int FirstRemote() const { return Dedicated ? 0 : 1; }
	bool Admin() const { return State == Mode::Host || (Hosted && State == Mode::Client && LocalSlot == OwnerSlot); }
	void ElectOwner() { if (Dedicated && OwnerRegistered && !Players[OwnerSlot].Connected) for (int i = 0; i < 4; ++i) if (Players[i].Connected) { OwnerSlot = i; if (!Playing) Players[i].Ready = true; break; } }
	void SendControl(const std::string& control, int value = 0, const std::string& text = "");
	void ControlRoom(int slot, const std::string& control, int value, const std::string& text);
	void HostedSmokeTick();
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
	int Port = 8000, Quality = 0, BandwidthMbps = 3, Difficulty = 50, Gold = 5000;
	int ActivityIndex = 0, SceneIndex = 0;
	bool Fog = false, Deploy = false, ClearOrbit = false;
	uint8_t AvailableTeams = 15, CPUTeam = 0;
	std::array<std::string, 4> Tech;
	std::vector<std::string> Factions;
	std::vector<GameActivity*> Activities;
	std::vector<Scene*> Scenes;
	Input LocalInput;
	uint64_t LastSampledInputRevision = UINT64_MAX;
	float MouseRemainderX = 0, MouseRemainderY = 0;
	MultiplayerWorld World;
	MP::World::Assembler SnapshotAssembly, ResourceAssembly{30000, 1024, 32 * 1024 * 1024};
	std::deque<MP::World::Snapshot> PendingWorlds;
	// Recently decoded states by ID; the host encodes deltas against one this guest acknowledged.
	std::array<MP::World::Snapshot, 32> DecodedStates;
	std::unordered_set<uint64_t> MissingResources, ReportedResources;
	std::vector<uint64_t> SceneManifest;
	std::unordered_set<uint64_t> SceneMissing;
	bool SceneReceived = false;
	uint64_t SceneReceivedAt = 0;
	MP::World::ResourceRequests ResourceRequests;
	void ResetSceneTransfer() { SceneManifest.clear(); SceneMissing.clear(); SceneReceived = false; ResourceRequests.Reset(); RecentWorldUpdates.clear(); LastFrameReceived = 0; DecodedStates = {}; }
	uint64_t LastRender = 0;
	uint64_t LastResourceRequest = 0;
	unsigned Texture = 0;
	int TextureWidth = 0, TextureHeight = 0;
	uint64_t BytesReceived = 0;
	float FPS = 0;
	std::deque<uint64_t> RecentWorldUpdates;
	uint64_t LastNetworkLog = 0;
	unsigned NetworkLogSamples = 0;
	bool Smoke = false, SmokeCapture = false;
	bool SmokeWorldLoss = false;
	bool SmokeCombatStress = false;
	bool SmokeExplosionBurst = false;
	bool SmokeEncounter = false;
	bool SmokeNativeBaseline = false;
	uint64_t SmokeEncounterStart = 0, SmokeEncounterLastUpdate = 0, SmokeEncounterGap = 0;
	uint64_t SmokeEncounterUpdates = 0;
	size_t SmokeEncounterPeakNodes = 0, SmokeEncounterPeakTrails = 0, SmokeEncounterPeakBytes = 0;
	size_t SmokeEncounterPeakPacked = 0;
	size_t SmokeEncounterPeakPixels = 0, SmokeEncounterPeakSprites = 0;
	uint64_t SmokeEncounterWireBytes = 0, SmokeEncounterWireUpdates = 0;
	uint64_t SnapshotChunksReceived = 0, SnapshotsAssembled = 0;
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
	bool SmokeGuestInput = false, SmokeGuestInputChecked = false;
	bool SmokeDeploymentFailed = false;
	uint64_t SmokeDeploymentStart = 0, SmokeDeploymentCount = 0;
	bool CursorVerification = false;
	size_t CursorStage = 0;
	int CursorFrames = 0;
	int CursorPreparedStage = -1;
	struct CursorCase { const char* Name; Mode State; bool UI, Playing, Visible; int Page; bool Hosted, Owner; };
	static constexpr std::array<CursorCase, 29> CursorCases{{
		{"menu", Mode::Idle, true, false, true, 0, false, false}, {"main-menu", Mode::Idle, false, false, false, 0, false, false},
		{"host-lobby", Mode::Host, true, false, true, 0, false, false}, {"host-gameplay", Mode::Host, false, true, false, 0, false, false},
		{"host-session", Mode::Host, true, true, true, 0, false, false}, {"host-resume", Mode::Host, false, true, false, 0, false, false},
		{"guest-lobby", Mode::Client, true, false, true, 0, false, false}, {"guest-gameplay", Mode::Client, false, true, false, 0, false, false},
		{"guest-session", Mode::Client, true, true, true, 0, false, false}, {"connecting", Mode::Connecting, false, false, true, 0, false, false},
		{"reconnecting", Mode::Reconnecting, false, false, true, 0, false, false}, {"closed-menu", Mode::Idle, false, false, false, 0, false, false},
		{"create-menu", Mode::Idle, true, false, true, 1, false, false}, {"connection-settings", Mode::Idle, true, false, true, 2, false, false},
		{"host-rules", Mode::Host, true, false, true, 3, false, false}, {"host-factions", Mode::Host, true, false, true, 4, false, false},
		{"guest-rules", Mode::Client, true, false, true, 3, false, false}, {"room-chat", Mode::Host, true, false, true, 5, false, false},
		{"post-match-lobby", Mode::Host, true, false, true, 0, false, false}, {"stream-settings", Mode::Idle, true, false, true, 3, false, false},
		{"host-session-chat", Mode::Host, true, true, true, 5, false, false}, {"guest-session-chat", Mode::Client, true, true, true, 5, false, false},
		{"close-room-confirmation", Mode::Host, true, false, true, 6, false, false}, {"leave-room-confirmation", Mode::Client, true, true, true, 6, false, false},
		{"hosted-owner-lobby", Mode::Client, true, false, true, 0, true, true},
		{"hosted-owner-session", Mode::Client, true, true, true, 0, true, true},
		{"hosted-owner-leave-confirmation", Mode::Client, true, false, true, 6, true, true},
		{"hosted-owner-close-confirmation", Mode::Client, true, false, true, 7, true, true},
		{"hosted-player-lobby", Mode::Client, true, false, true, 0, true, false}
	}};
	bool SmokeReconnected = false, SmokeCapturedHost = false;
	bool SmokeHostedContinued = false;
	bool SmokeHostedLeft = false, SmokeHostedReturned = false;
	bool SmokeHostedIndependent = false;
	bool SmokeHostedCombatCaptured = false;
	uint64_t SmokeHostedLeaveAt = 0;
	uint32_t SmokeHostedLeaveSequence = 0;
	uint32_t SmokeHostedOtherFrame = 0;
	uint64_t SmokeHostedSession = 0;
	bool SmokeSessionChecked = false;
	int SmokeStage = 0;
	int SmokeRejoins = 0;
	int SmokeGuests = 1;
	std::string SmokeRole;
	std::string SmokeLogDirectory = "build-mp";
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
	// Owner lobby edit: 0 not sent, 1 pending, 2 confirmed.
	int SmokeLobbyEdit = 0, SmokeLobbyEditTarget = 0;
	bool SmokeTextSent = false;
	std::unique_ptr<SoundContainer> SmokeLoop;
	int SmokeLoopPlays = 0;
	int SmokeLoopChannel = -1;
	bool SmokeLoopStopped = false;
	void Verify(const std::string& message) { if (Smoke) { std::ofstream log(SmokeLogDirectory + "/" + SmokeRole + "-smoke.log", std::ios::app); log << message << '\n'; } }
	void SmokeTick();
	void DeploymentTick();
	void GuestInputChecks(GameActivity* game);
	void EncounterTick();

	void ClearSounds() { for (auto& [channel, sound]: Sounds) sound->Stop(); Sounds.clear(); }
	static void ClearWorldPackets(Player& player) {
		player.WorldPackets.clear(); player.ResourceFlight.Reset(); player.ResourceMessages.clear(); player.QueuedResourceChunks.clear(); player.SnapshotStarted = false;
		player.AckID = 0; player.NextWorld = 0; player.LastPump = 0; player.Rate = {}; player.QueuedSnapshotFrame = 0; player.SnapshotSentAt = {};
		player.AckedLayers.clear(); player.SentLayers = {}; player.SceneResources.clear(); player.SentStates = {};
		player.AudioChannels.clear(); player.PendingSoundChanges.clear(); player.AudioCursor = {}; player.AudioBudget = 0; player.LastAudioBudget = player.LastAudioChanges = 0; player.SentGlobalPitch = -1;
		player.SceneSent = false; player.SceneQueue.clear(); player.UrgentQueue.clear(); player.Urgent.clear(); player.StateQueue.clear();
	}
	void Stop() {
		Menu.reset();
		if (TextActive && State != Mode::Host && !Dedicated) SDL_StopTextInput(g_WindowMan.GetWindow()); TextActive = false;
		if (SmokeLoop) { SmokeLoop->Stop(); SmokeLoop.reset(); }
		if (State == Mode::Host) { MP::Writer leave(Kind::Leave, Session, Epoch); for (size_t i = FirstRemote(); i < Players.size(); ++i) if (Players[i].Connected) Net.Send(Players[i].Address, leave.Data, Delivery::Control); }
		if (State == Mode::Client || State == Mode::Connecting || State == Mode::Reconnecting) { MP::Writer leave(Kind::Leave, Session, Epoch); Net.Send(ServerAddress, leave.Data, Delivery::Control); }
		Net.Stop();
		ServerAddress.clear(); HostedEndpoint = ResumingSlot = false; NextRetry = CloseStarted = CloseRetry = 0; OwnerRegistered = false;
		for (auto& player: Players) { player = Player(); }
		// A guest keeps its content-addressed resources, so leaving and rejoining
		// a room reuses the downloaded map instead of loading it again.
		const bool hosting = State == Mode::Host;
		Pending.clear(); Chat.clear(); ClearSounds(); if (hosting) World.Reset(); else World.ResetPresentation(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); LocalInput = {}; Session = ReconnectToken = 0; OwnerCredential.clear(); OwnerSlot = 0; Epoch = 0; MouseRemainderX = MouseRemainderY = 0;
		Texture = 0; TextureWidth = TextureHeight = 0;
		RecentWorldUpdates.clear(); LastFrameReceived = LastNetworkLog = 0; NetworkLogSamples = 0;
		State = Mode::Idle; Playing = Launch = Played = false; MatchEndedAt = 0; Notice.clear(); g_AudioMan.SetMultiplayerMode(false); g_UInputMan.TrapMousePos(false);
		WarmingGuests = false;
		g_AudioMan.SetStreamListener(Vector(), false);
		for (int player = 0; player < 4; ++player) g_UInputMan.ClearRemoteInput(player);
	}
	void Advertise() {
		if (State != Mode::Host) return;
		MP::Writer writer(Kind::Announcement); writer.Text(RoomName, 63); uint8_t count = 0; for (const auto& player: Players) if (player.Connected) ++count;
		writer.U8(count); writer.U8(Playing); Net.Advertise(writer.Data);
	}
	void Lobby() {
		ElectOwner();
		MP::Writer writer(Kind::Lobby, Session, Epoch); writer.U8(Playing); writer.U8(Dedicated); writer.U8(uint8_t(OwnerSlot)); writer.Text(RoomName, 63); writer.Text(ActivityName); writer.Text(SceneName);
		writer.U8(static_cast<uint8_t>(Difficulty)); writer.U32(static_cast<uint32_t>(Gold)); writer.U8(Fog); writer.U8(Deploy); writer.U8(ClearOrbit); writer.U8(AvailableTeams); writer.U8(CPUTeam);
		for (const auto& tech: Tech) writer.Text(tech);
		for (const auto& player: Players) { writer.U8(player.Token != 0); writer.U8(player.Connected); writer.U8(player.Ready); writer.U8(player.Team); writer.Text(player.Name, 31); writer.U32(player.AppliedEdit); }
		for (size_t i = FirstRemote(); i < Players.size(); ++i) if (Players[i].Connected) Net.Send(Players[i].Address, writer.Data, Delivery::Control);
		Advertise();
	}
	void Reject(const std::string& address, const std::string& message) { MP::Writer writer(Kind::Reject); writer.Text(message); Net.Send(address, writer.Data, Delivery::Control); Pending[address] = Now() - 4500; }
	void Welcome(int slot) {
		Players[slot].AppliedEdit = 0;
		MP::Writer writer(Kind::Welcome, Session, Epoch); writer.U8(static_cast<uint8_t>(slot)); writer.U64(Players[slot].Token); writer.Text(c_GameVersion.str()); writer.U8(Dedicated);
		Net.Send(Players[slot].Address, writer.Data, Delivery::Control); Lobby();
	}
	bool Host() {
		std::string service; uint16_t servicePort = 8001;
		if (Online && !Address(ServiceAddress, service, servicePort, 8001)) { Error = "Set the room service address under Connection settings."; return false; }
		if (!Online && (Port < 1 || Port > 65535)) { Error = "Choose a UDP port from 1 to 65535."; return false; }
		if (Online && Hosted) {
			Stop(); ConnectStarted = Now(); RoomName = Room[0] ? Room : "Cortex room";
			if (!Net.Start(false, 0, "", Error) || !Net.ConnectRelay(true, service, servicePort, "", Password, Error, true, RoomName)) return false;
			State = Mode::Connecting; UI = true; Error.clear(); SaveService(); return true;
		}
		Stop(); if (!Net.Start(!Online, static_cast<uint16_t>(Port), Online ? "" : Password, Error)) return false;
		if (Online && !Net.ConnectRelay(true, service, servicePort, "", Password, Error)) { Net.Stop(); return false; }
		if (Online) SaveService();
		State = Mode::Host; Session = Token(); RoomName = Room[0] ? Room : "Cortex room";
		Players[0].Name = Name[0] ? Name : "Host"; Players[0].Token = Token(); Players[0].Connected = Players[0].Ready = true;
		Error.clear(); UI = true; LoadActivities(); Lobby(); return true;
	}
	bool Join(bool retry = false) {
		std::string host; uint16_t port;
		if (retry && HostedEndpoint && !ServerAddress.empty()) {
			if (!Address(ServerAddress, host, port)) return false;
			Net.Close(ServerAddress); Net.Connect(host, port, Password, Error); NextRetry = Now() + 4000; return true;
		}
		if (Online) { if (!Address(ServiceAddress, host, port, 8001)) { Error = "Set the room service address under Connection settings."; return false; } if (Relay::NormalizeCode(JoinCode).empty()) { Error = "Enter the ten-character room code from your host."; return false; } }
		else if (!Address(HostAddress, host, port)) { Error = "Enter a hostname or IPv4 address, optionally followed by :port."; return false; }
		if (!retry) { Stop(); ConnectStarted = Now(); if (Online) { const auto resume = ResumeSlots.find(ResumeKey()); if (resume != ResumeSlots.end() && resume->second.Expires > Now()) { ReconnectToken = resume->second.Token; Session = resume->second.Session; Epoch = resume->second.Epoch; LocalInput = resume->second.Inputs; ResumingSlot = true; } } }
		else if (!Online) Net.Stop();
		if (Online && retry) { if (!Net.ReconnectRelay(Error)) return false; }
		else { if (!Net.Start(false, 0, "", Error) || !(Online ? Net.ConnectRelay(false, host, port, JoinCode, Password, Error) : Net.Connect(host, port, Password, Error))) return false; }
		if (Online) SaveService();
		State = retry ? Mode::Reconnecting : Mode::Connecting; NextRetry = Now() + 3000; UI = true; return true;
	}
	void Hello(const std::string& address) {
		ServerAddress = address; MP::Writer writer(Kind::Hello); writer.Text(Name[0] ? Name : "Player", 31); writer.U64(ReconnectToken); writer.Text(c_GameVersion.str()); writer.U16(uint16_t(std::clamp(g_WindowMan.GetResX(), 320, 3840))); writer.U16(uint16_t(std::clamp(g_WindowMan.GetResY(), 180, 2160))); writer.Text(Hosted ? OwnerCredential : "", Relay::TokenLength); Net.Send(address, writer.Data, Delivery::Control);
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
	void SettingsChanged() { for (size_t i = FirstRemote(); i < Players.size(); ++i) Players[i].Ready = Dedicated && int(i) == OwnerSlot; Lobby(); }
	void SendReady(bool ready, uint8_t team) {
		const uint32_t edit = ++EditSequence; PendingEdits["Ready"] = {edit, ready}; PendingEdits["Team" + std::to_string(LocalSlot)] = {edit, team};
		MP::Writer writer(Kind::Ready, Session, Epoch); writer.U8(ready); writer.U8(team); writer.U32(edit); Net.Send(ServerAddress, writer.Data, Delivery::Control);
	}
	// Lobby edits this client sent that the host has not answered yet. The menu
	// shows them instead of the older confirmed state, so a selection does not
	// snap back while its request is in flight.
	struct PendingEdit { uint32_t Sequence = 0; int Value = 0; std::string Text; };
	std::map<std::string, PendingEdit> PendingEdits;
	uint32_t EditSequence = 0;
	const PendingEdit* FindEdit(const std::string& control) const { const auto found = PendingEdits.find(control); return found == PendingEdits.end() ? nullptr : &found->second; }
	// Typed numbers are committed on Enter or after a pause, not per digit.
	struct NumberDraft { std::string Control, Text; uint64_t Changed = 0; } Draft;
	void CommitDraft();
	void AddChat(int player, const std::string& message) {
		Chat.push_back(Players[player].Name + ": " + message); if (Chat.size() > 64) Chat.erase(Chat.begin());
		Verify("CHAT: " + message);
		if (State == Mode::Host) { MP::Writer writer(Kind::Chat, Session, Epoch); writer.U8(static_cast<uint8_t>(player)); writer.Text(message, 192); for (int i = FirstRemote(); i < 4; ++i) if (Players[i].Connected) Net.Send(Players[i].Address, writer.Data, Delivery::Control); }
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
			player.Ready = int(i) == OwnerSlot; player.Inputs.Reset(); ClearWorldPackets(player); player.WorldResources.clear(); player.PendingResources.clear(); player.Text.clear(); player.TextActive = player.AudioReady = false; player.Loops.clear(); player.ReplayLoops = true;
			if (Dedicated || i > 0) g_UInputMan.ClearRemoteInput(static_cast<int>(i));
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
	bool QueueWorld(Player& player, Kind kind, std::span<const uint8_t> payload, Delivery delivery, uint64_t resource = 0, std::span<const uint8_t> prepared = {});
	size_t QueueResource(Player& player, uint64_t id, bool scene);
	// Commits view tiles, then the current state's resources, then the rest of the map.
	void FillResources(Player& player);
	// Content-addressed resources are packed once with high compression and
	// reused for every guest and reconnect instead of compressed per guest.
	static constexpr size_t PackedResourceLimit = 96 * 1024 * 1024;
	std::unordered_map<uint64_t, std::vector<uint8_t>> PackedResources;
	size_t PackedResourceBytes = 0;
	std::span<const uint8_t> PackedResource(uint64_t id);
	// Guests captured in the current draw pass, encoded together when it ends.
	struct Capture { int Player = 0; MP::World::Snapshot State; const MP::World::Snapshot* Baseline = nullptr; uint64_t Captured = 0, Composed = 0; size_t RawBytes = 0; std::vector<uint8_t> Packed; };
	std::vector<Capture> Captures;
	void EncodeCapture(Capture& capture);
	void SendCapture(Capture& capture);
	void FinishCaptures();
	// Set while the simulation runs well below real time, measured over
	// half-second windows of simulated against real time.
	bool Overloaded = false;
	uint64_t SpeedWindowStart = 0; long long SpeedWindowTicks = 0;
	void MeasureSimulationSpeed(uint64_t now);
	void PumpWorld(Player& player, uint64_t now);
	void ReceiveWorld(Kind kind, MP::Reader& reader);
	void PresentWorld(MP::World::Snapshot snapshot);
	void PresentPendingWorlds();
	bool VerifyWorldFlow(std::ostream& log);
	void Draw();
};

MultiplayerMan::MultiplayerMan(): m_Impl(std::make_unique<Impl>()) {}
MultiplayerMan::~MultiplayerMan() = default;
bool MultiplayerMan::IsDedicated() const { return m_Impl->Dedicated; }
bool MultiplayerMan::StartDedicated(const std::string& configPath) {
	if (!g_WindowMan.IsHeadless()) return false;
	std::ifstream file(configPath);
	std::map<std::string, std::string> config;
	std::string line;
	while (std::getline(file, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); const auto separator = line.find('='); if (separator != std::string::npos) config[line.substr(0, separator)] = line.substr(separator + 1); }
	auto integer = [&](const std::string& key, unsigned minimum, unsigned maximum, unsigned fallback) {
		const auto found = config.find(key); if (found == config.end()) return fallback;
		unsigned value = 0; const auto parsed = std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
		return parsed.ec == std::errc() && parsed.ptr == found->second.data() + found->second.size() && value >= minimum && value <= maximum ? value : 0;
	};
	const auto port = integer("port", 1024, 65535, 0), idle = integer("idle_seconds", 1, 86400, 600);
	const auto owner = config["owner_token"];
	if (!port || !idle || owner.size() != Relay::TokenLength || owner.find_first_not_of("0123456789abcdef") != std::string::npos || config["password"].size() > 63 || config["room_name"].size() > 63 || config["status_file"].empty()) return false;
	auto& impl = *m_Impl; impl.Stop(); impl.Dedicated = impl.Hosted = true; impl.Online = false; impl.UI = false;
	impl.OwnerSlot = 0; impl.OwnerRegistered = false; impl.OwnerCredential = owner; impl.DedicatedStatus = config["status_file"]; impl.DedicatedIdleSeconds = idle;
	impl.DedicatedLastOccupied = Now(); impl.OwnerClaimDeadline = impl.DedicatedLastOccupied + 120000; impl.Port = int(port); impl.RoomName = config["room_name"].empty() ? "Cortex room" : config["room_name"];
	if (!impl.Net.Start(true, uint16_t(port), config["password"], impl.Error)) return false;
	impl.State = Impl::Mode::Host; impl.Session = Token(); impl.LoadActivities();
	impl.Smoke = std::getenv("CCCP_MPSMOKE_HOSTED") != nullptr;
	if (impl.Smoke) {
		impl.SmokeRole = "dedicated"; impl.SmokeStarted = impl.SmokeStageTime = Now();
		impl.SmokeGuests = 3; if (const char* count = std::getenv("CCCP_MPSMOKE_GUESTS"); count && count[0] >= '1' && count[0] <= '3' && !count[1]) impl.SmokeGuests = count[0] - '0';
		impl.SmokeCombatStress = std::getenv("CCCP_MPSMOKE_COMBAT") != nullptr;
		impl.SmokeLogDirectory = (std::filesystem::path(System::GetUserdataDirectory()) / "verification").string();
		std::filesystem::create_directories(impl.SmokeLogDirectory); std::ofstream(impl.SmokeLogDirectory + "/dedicated-smoke.log") << "START: dedicated authority, four remote human slots, no display\n";
		for (size_t i = 0; i < impl.Activities.size(); ++i) if (impl.Activities[i]->GetPresetName() == "Test Activity") { impl.ActivityIndex = int(i); impl.LoadScenes(); break; }
		impl.BandwidthMbps = 12;
	}
	std::ofstream status(impl.DedicatedStatus, std::ios::trunc); status << "ready=1\nconnected=0\nplaying=0\nheadless=1\n";
	return bool(status);
}

void MultiplayerMan::Impl::SendControl(const std::string& control, int value, const std::string& text) {
	const uint32_t edit = ++EditSequence; PendingEdits[control] = {edit, value, text};
	MP::Writer writer(Kind::RoomControl, Session, Epoch); writer.Text(control, 32); writer.U32(uint32_t(value)); writer.Text(text, 192); writer.U32(edit); Net.Send(ServerAddress, writer.Data, Delivery::Control);
}

void MultiplayerMan::Impl::ControlRoom(int slot, const std::string& control, int value, const std::string& text) {
	if (!Dedicated || slot != OwnerSlot || !Players[slot].Connected) return;
	auto report = [&]() { if (Error.empty()) return; MP::Writer writer(Kind::RoomControl, Session, Epoch); writer.Text("Error", 32); writer.U32(0); writer.Text(Error, 192); Net.Send(Players[slot].Address, writer.Data, Delivery::Control); Error.clear(); };
	if (control == "Close") { if (!CloseStarted) { CloseStarted = Now(); MP::Writer leave(Kind::Leave, Session, Epoch); for (const auto& player: Players) if (player.Connected) Net.Send(player.Address, leave.Data, Delivery::Control); } return; }
	if (control == "Return") { ReturnToLobby(); return; }
	if (Playing) return;
	if (control == "Start") { StartGame(); report(); return; }
	if (!text.empty() && (control == "Gold" || control == "Difficulty" || control == "Bandwidth")) {
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value); if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return;
	}
	bool changed = false;
	if (control == "Activity") {
		if (!text.empty()) { value = -1; for (int i = 0; i < Activities.size(); ++i) if (Activities[i]->GetPresetName() == text) value = i; }
		if (value >= 0 && value < Activities.size()) { ActivityIndex = value; LoadScenes(); changed = true; }
	} else if (control == "Scene") {
		if (!text.empty()) { value = -1; for (int i = 0; i < Scenes.size(); ++i) if (Scenes[i]->GetPresetName() == text) value = i; }
		if (value >= 0 && value < Scenes.size()) { SceneIndex = value; SceneName = Scenes[value]->GetPresetName(); changed = true; }
	} else if (control == "Gold" && value >= 0 && value <= 1000000) { Gold = value; changed = true; }
	else if (control == "Difficulty" && value >= 0 && value <= 100) { Difficulty = value; changed = true; }
	else if (control == "Bandwidth" && value >= 1 && value <= 48) { BandwidthMbps = value; changed = true; }
	else if (control == "Fog" && value >= 0 && value <= 1) { Fog = value != 0; changed = true; }
	else if (control == "Deploy" && value >= 0 && value <= 1) { Deploy = value != 0; changed = true; }
	else if (control == "Orbit" && value >= 0 && value <= 1) { ClearOrbit = value != 0; changed = true; }
	else if (control.starts_with("Faction") && control.size() == 8 && value >= 0 && value < Factions.size()) { const int team = control.back() - '0'; if (team >= 0 && team < 4) { Tech[team] = Factions[value]; changed = true; } }
	else if (control.starts_with("Team") && control.size() == 5 && value >= 0 && value < 4 && (AvailableTeams & (1 << value))) { const int player = control.back() - '0'; if (player >= 0 && player < 4 && Players[player].Connected) { Players[player].Team = uint8_t(value); changed = true; } }
	else if (control.starts_with("Release") && control.size() == 8) { const int player = control.back() - '0'; if (player >= 0 && player < 4 && !Players[player].Connected) { Net.Close(Players[player].Address); Players[player] = Player(); changed = true; } }
	if (changed) SettingsChanged();
}
void MultiplayerMan::Open() {
	m_Impl->UI = true; g_UInputMan.TrapMousePos(false);
	if (!m_Impl->Playing) g_MenuMan.SetMultiplayerMenuBackground(true);
}
void MultiplayerMan::Update() { m_Impl->Tick(); }
void MultiplayerMan::UpdateMenu() { if (!m_Impl->CursorVerification) m_Impl->UpdateMenu(); }
void MultiplayerMan::DrawUI() { m_Impl->Draw(); }
void MultiplayerMan::Stop() { m_Impl->Stop(); }
void MultiplayerMan::HandleActivityExit() { m_Impl->HandleActivityExit(); }
bool MultiplayerMan::StartRoom(bool host, const std::string& address, bool smokeTest, bool hosted) {
	auto& impl = *m_Impl; impl.UI = true; impl.Smoke = smokeTest;
	impl.SmokeWorldLoss = smokeTest && std::getenv("CCCP_MPSMOKE_WORLD_LOSS") && std::string(std::getenv("CCCP_MPSMOKE_WORLD_LOSS")) == "1";
	impl.SmokeCombatStress = smokeTest && std::getenv("CCCP_MPSMOKE_COMBAT");
	impl.SmokeExplosionBurst = smokeTest && std::getenv("CCCP_MPSMOKE_BURST");
	impl.SmokeEncounter = smokeTest && std::getenv("CCCP_MPSMOKE_ENCOUNTER");
	impl.SmokeNativeBaseline = impl.SmokeEncounter && std::getenv("CCCP_MPSMOKE_BASELINE");
	impl.Hosted = hosted || (smokeTest && std::getenv("CCCP_MPSMOKE_HOSTED")); impl.Online = impl.Hosted || std::getenv("CCCP_MP_SERVICE") != nullptr;
	if (!host && impl.Online) std::snprintf(impl.JoinCode, sizeof(impl.JoinCode), "%s", address.c_str());
	if (smokeTest) {
		impl.SmokeDeployment = std::getenv("CCCP_MPSMOKE_DEPLOYMENT") != nullptr;
		impl.SmokeGuestInput = std::getenv("CCCP_MPSMOKE_GUEST_INPUT") != nullptr;
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
		if (host && started && !impl.Hosted) { for (int i = 0; i < impl.Activities.size(); ++i) if (impl.Activities[i]->GetPresetName() == (impl.SmokeEncounter ? "Brain vs Brain" : "Test Activity")) { impl.ActivityIndex = i; impl.LoadScenes(); break; } if (impl.SmokeDeployment || impl.SmokeEncounter) { for (int i = 0; i < impl.Scenes.size(); ++i) if (impl.Scenes[i]->GetPresetName() == (impl.SmokeEncounter ? "Cave Bunkers" : "Grasslands")) impl.SceneIndex = i; impl.SceneName = impl.Scenes[impl.SceneIndex]->GetPresetName(); } impl.Lobby(); }
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
bool MultiplayerMan::IsRemotePlayer(int player) const { return IsHostingMatch() && player >= m_Impl->FirstRemote() && player < 4 && m_Impl->Players[player].Token != 0; }
void MultiplayerMan::SetTextInputActive(int player, bool active) {
	if (!IsRemotePlayer(player)) return; auto& peer = m_Impl->Players[player]; peer.TextActive = active; peer.Text.clear();
	MP::Writer writer(Kind::TextInput, m_Impl->Session, m_Impl->Epoch); writer.U8(1); writer.U8(active); if (peer.Connected) m_Impl->Net.Send(peer.Address, writer.Data, Delivery::Control);
}
bool MultiplayerMan::TakeTextInput(int player, std::string& text) {
	if (!IsRemotePlayer(player)) { text.clear(); return false; } text = std::move(m_Impl->Players[player].Text); m_Impl->Players[player].Text.clear(); return !text.empty();
}
bool MultiplayerMan::TakeLaunchRequest() { const bool launch = m_Impl->Launch; m_Impl->Launch = false; return launch; }
int MultiplayerMan::ViewWidth(int screen) const { const auto* activity = g_ActivityMan.GetActivity(); const int player = activity && screen >= 0 ? activity->PlayerOfScreen(screen) : 0; return IsHostingMatch() && player >= m_Impl->FirstRemote() ? m_Impl->Players[player].ViewWidth : g_WindowMan.GetResX(); }
int MultiplayerMan::ViewHeight(int screen) const { const auto* activity = g_ActivityMan.GetActivity(); const int player = activity && screen >= 0 ? activity->PlayerOfScreen(screen) : 0; return IsHostingMatch() && player >= m_Impl->FirstRemote() ? m_Impl->Players[player].ViewHeight : g_WindowMan.GetResY(); }
bool MultiplayerMan::WantsState(int player) const {
	if (m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3) return false;
	if (!IsRemotePlayer(player)) return false; const auto& peer = m_Impl->Players[player];
	return peer.Connected && Now() >= peer.NextWorld;
}
void MultiplayerMan::BeginWorldCapture() { if (IsHostingMatch() && !(m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3)) { m_Impl->World.BeginObjects(); m_Impl->Profile.StageStart = NowMicros(); } }
bool MultiplayerMan::GuestView(int player, float& x, float& y) const {
	if (!IsRemotePlayer(player)) return false;
	const auto& input = m_Impl->Players[player].Inputs.LastInput(); if (!input.ViewValid) return false;
	x = input.ViewX; y = input.ViewY; return true;
}
bool MultiplayerMan::GuestCursor(int player, int mode, float& x, float& y) const {
	if (!IsRemotePlayer(player)) return false;
	const auto& input = m_Impl->Players[player].Inputs.LastInput(); if (!input.CursorValid || input.CursorMode != mode) return false;
	x = input.CursorX; y = input.CursorY; return true;
}
bool MultiplayerMan::GuestPointer(int player, float& x, float& y) const {
	if (!IsRemotePlayer(player)) return false;
	const auto& input = m_Impl->Players[player].Inputs.LastInput(); if (!input.PointerValid) return false;
	x = input.PointerX; y = input.PointerY; return true;
}
void MultiplayerMan::BeginWorldTrails() { if (IsHostingMatch() && !(m_Impl->SmokeNativeBaseline && m_Impl->SmokeStage == 3)) m_Impl->World.BeginTrails(); }
void MultiplayerMan::EndWorldCapture() {
	m_Impl->World.EndObjects();
	if (m_Impl->Profile.StageStart && IsHostingMatch()) { m_Impl->ProfileStage(Impl::ServerProfile::Objects, m_Impl->Profile.StageStart); ++m_Impl->Profile.CaptureLoops; m_Impl->Profile.StageStart = 0; }
}
void MultiplayerMan::BeginGuestView(BITMAP* gui, float cameraX, float cameraY) { if (m_Impl->SmokeEncounter) m_Impl->SmokeCaptureStarted = NowMicros(); m_Impl->Profile.StageStart = NowMicros(); m_Impl->World.BeginView(gui, Vector(cameraX, cameraY)); }
void MultiplayerMan::RecordSimulationUpdate() {
	auto& impl = *m_Impl; if (!impl.Benchmark || impl.State != Impl::Mode::Host) return;
	++impl.Profile.SimulationUpdates;
	for (int counter = 0; counter < PerformanceMan::PerfCounterCount; ++counter) impl.Profile.SimulationStageUs[counter] += g_PerformanceMan.GetCurrentSample(static_cast<PerformanceMan::PerformanceCounters>(counter));
}
void MultiplayerMan::RecordServerLoop(long long networkUs, long long simulationUs, int simulationUpdates, long long drawUs, long long idleUs) {
	auto& impl = *m_Impl; if (!impl.Benchmark || impl.State != Impl::Mode::Host) return;
	auto& p = impl.Profile; const uint64_t now = Now();
	if (!p.WindowStart) { p.WindowStart = now; p.SimulationTicks = g_TimerMan.GetSimTickCount(); }
	++p.Loops; p.NetworkUs += uint64_t(std::max(0LL, networkUs)); p.SimulationUs += uint64_t(std::max(0LL, simulationUs)); p.DrawUs += uint64_t(std::max(0LL, drawUs)); p.IdleUs += uint64_t(std::max(0LL, idleUs));
	p.LoopMaxUs = std::max<uint64_t>(p.LoopMaxUs, uint64_t(std::max(0LL, networkUs + simulationUs + drawUs + idleUs)));
	(void)simulationUpdates;
	if (now - p.WindowStart >= 1000) impl.WriteProfile(now);
}
void MultiplayerMan::EndGuestView(int player) {
	auto& impl = *m_Impl; auto& peer = impl.Players[player];
	if (std::any_of(impl.Captures.begin(), impl.Captures.end(), [player](const auto& capture) { return capture.Player == player; })) impl.FinishCaptures();
	peer.LastWorld = Now();
	impl.ProfileStage(Impl::ServerProfile::View, impl.Profile.StageStart);
	// Advance the schedule by whole intervals. A capture that is more than a
	// full interval late restarts the phase rather than sending a burst.
	// While the simulation cannot keep real time, capture less often: every
	// capture competes with the simulation for the same thread.
	const uint64_t interval = !peer.AckID ? 250 : impl.Overloaded ? MP::World::SnapshotIntervalMS * 4 / 3 : MP::World::SnapshotIntervalMS;
	if (!peer.NextWorld) peer.NextWorld = peer.LastWorld + interval + uint64_t(player) * interval / 4;
	else peer.NextWorld = peer.LastWorld - peer.NextWorld < interval ? peer.NextWorld + interval : peer.LastWorld + interval;
	++impl.Profile.Captures;
	const uint64_t captured = NowMicros();
	if (!++peer.FrameID) ++peer.FrameID;
	auto snapshot = impl.World.EndView(player, peer.FrameID, peer.Inputs.LastInput().Sequence, peer.LastWorld);
	snapshot.Paused = snapshot.Paused || impl.WarmingGuests;
	const uint64_t composed = NowMicros();
	impl.ProfileStage(Impl::ServerProfile::Compose, captured);
	snapshot.MouseX = peer.Inputs.LastInput().MouseX; snapshot.MouseY = peer.Inputs.LastInput().MouseY;
	if (!peer.SceneSent) {
		if (impl.SceneManifest.empty()) impl.SceneManifest = impl.World.PrepareScene();
		const auto map = impl.World.PrepareSceneMap(snapshot.Width, snapshot.Height);
		const auto assets = MP::World::ManifestResources(impl.SceneManifest, map);
		MP::Writer manifest(Kind::WorldManifest); MP::World::WriteManifest(manifest, assets, map);
		if (impl.QueueWorld(peer, Kind::WorldManifest, manifest.Data, Delivery::WorldResource)) {
			peer.SceneSent = true; const auto ordered = MP::World::NearestResources(assets, map, snapshot.Width, snapshot.Height); peer.SceneQueue = {ordered.begin(), ordered.end()};
			// The reliable manifest delivers the whole map; it is the guest's layer baseline.
			peer.AckedLayers.clear(); for (const auto& node : map.Nodes) peer.AckedLayers[node.ID] = node.Asset;
		}
	}
	// Retained layer tiles the guest already holds are omitted. Changed tiles,
	// and fog before its first acknowledgement, repeat until a snapshot carrying
	// them is acknowledged. Unchanged scenery no longer fills every update.
	{
		// The capture holds the tiles around the guest's current view; any it
		// still lacks go ahead of the nearest-first map stream. The camera
		// usually moves to the player's actor after the manifest is sent.
		for (const auto& node : snapshot.Nodes) if (node.Asset && MP::World::LayerOrdinal(node) && !peer.WorldResources.contains(node.Asset) && peer.Urgent.insert(node.Asset).second) peer.UrgentQueue.push_back(node.Asset);
		auto& sent = peer.SentLayers[peer.FrameID % peer.SentLayers.size()]; sent.Frame = peer.FrameID; sent.Layers.clear();
		std::erase_if(snapshot.Nodes, [&](const MP::World::Node& node) {
			if (!MP::World::LayerOrdinal(node)) return false;
			if (const auto acked = peer.AckedLayers.find(node.ID); acked != peer.AckedLayers.end() && acked->second == node.Asset) return true;
			// A changed tile stays needed after its update is acknowledged.
			sent.Layers.emplace_back(node.ID, node.Asset); if (node.Asset) peer.SceneResources.insert(node.Asset); return false;
		});
	}
	// Encode against the newest acknowledged state while acknowledgements are
	// current; otherwise send a complete state the guest can always decode.
	const MP::World::Snapshot* baseline = nullptr;
	if (peer.AckID && Now() - peer.LastAck < 1000 && peer.FrameID - peer.AckID < peer.SentStates.size()) {
		if (const auto& stored = peer.SentStates[peer.AckID % peer.SentStates.size()]; stored.ID == peer.AckID) baseline = &stored;
	}
	// Encoding, compression and the stored baseline touch only this guest's
	// state, so the guests captured in one pass encode in parallel.
	impl.Captures.push_back({player, std::move(snapshot), baseline, captured, composed});
}
void MultiplayerMan::FinishCaptures() { m_Impl->FinishCaptures(); }
void MultiplayerMan::Impl::EncodeCapture(Capture& capture) {
	auto& peer = Players[capture.Player]; auto& snapshot = capture.State;
	// About three quarters of each interval's measured send rate carries the
	// state; the rest leaves room for resources, audio and retransmission.
	const double rate = peer.Rate.Cap() ? peer.Rate.Rate() : BandwidthMbps * 125000.0 * .85;
	const size_t poseBudget = std::max<size_t>(4096, size_t(rate * MP::World::SnapshotIntervalMS / 1000.0 * 0.76));
	const auto baselineIndex = capture.Baseline ? MP::World::IndexNodes(*capture.Baseline) : MP::World::NodeIndex();
	MP::World::FitSnapshot(snapshot, poseBudget, [&](const MP::World::Snapshot& state) {
		MP::Writer writer(Kind::WorldSnapshot); MP::World::WriteSnapshot(writer, state, capture.Baseline, &baselineIndex);
		MP::Writer packed(Kind::WorldSnapshot); packed.U32(uint32_t(writer.Data.size())); const size_t prefix = packed.Data.size();
		packed.Data.resize(prefix + LZ4_compressBound(int(writer.Data.size())));
		const int size = LZ4_compress_default(reinterpret_cast<const char*>(writer.Data.data()), reinterpret_cast<char*>(packed.Data.data() + prefix), int(writer.Data.size()), int(packed.Data.size() - prefix));
		if (size <= 0) return std::numeric_limits<size_t>::max();
		capture.RawBytes = writer.Data.size(); packed.Data.resize(prefix + size); capture.Packed = std::move(packed.Data); return capture.Packed.size();
	});
	auto& sent = peer.SentStates[snapshot.ID % peer.SentStates.size()]; sent = snapshot; sent.CriticalNodes.reset();
}
void MultiplayerMan::Impl::FinishCaptures() {
	if (Captures.empty()) return;
	const uint64_t start = NowMicros();
	if (Captures.size() == 1) EncodeCapture(Captures.front());
	else g_ThreadMan.GetPriorityThreadPool().parallelize_loop(size_t(0), Captures.size(), [this](size_t first, size_t last) { for (size_t i = first; i < last; ++i) EncodeCapture(Captures[i]); }, Captures.size()).get();
	ProfileStage(ServerProfile::Encode, start);
	for (auto& capture : Captures) if (Players[capture.Player].Connected) SendCapture(capture);
	Captures.clear();
}
void MultiplayerMan::Impl::SendCapture(Capture& capture) {
	auto& impl = *this; auto& peer = Players[capture.Player]; const auto& snapshot = capture.State;
	const uint64_t captured = capture.Captured, composed = capture.Composed;
	impl.Profile.PackedBytes += capture.Packed.size();
	const uint64_t encoded = NowMicros();
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
	// ahead of the resources actually needed by the current view. The sweep
	// scans every queued resource, so it runs a few times a second.
	if (peer.AckID && peer.LastWorld - peer.LastObsoleteSweep >= 250) {
		peer.LastObsoleteSweep = peer.LastWorld;
		std::unordered_set<uint64_t> needed; for (const auto& node : snapshot.Nodes) if (node.Asset) needed.insert(node.Asset);
		std::unordered_set<uint32_t> obsolete;
		std::erase_if(peer.ResourceMessages, [&](const auto& entry) {
			// Scene preload is not referenced by delta snapshots but is still needed.
			if (needed.contains(entry.first) || peer.SceneResources.contains(entry.first) || peer.ResourceFlight.Contains(entry.second)) return false;
			obsolete.insert(entry.second); peer.PendingResources.erase(entry.first); peer.QueuedResourceChunks.erase(entry.second); return true;
		});
		std::erase_if(peer.WorldPackets, [&](const auto& packet) {
			if (packet.second != Delivery::WorldResource) return false;
			MP::Reader reader(packet.first); Header header; MP::World::Chunk chunk;
			return ReadHeader(reader, header) && MP::World::ReadChunk(reader, chunk) && obsolete.contains(chunk.ID);
		});
	}
	peer.StateQueue.clear();
	{ std::unordered_set<uint64_t> listed; for (const auto& node : snapshot.Nodes) if (node.Asset && !peer.WorldResources.contains(node.Asset) && listed.insert(node.Asset).second) peer.StateQueue.push_back(node.Asset); }
	impl.FillResources(peer);
	const uint64_t serialized = impl.SmokeEncounter ? NowMicros() : 0;
	if (impl.SmokeEncounter) { impl.SmokeEncounterPeakNodes = std::max(impl.SmokeEncounterPeakNodes, snapshot.Nodes.size()); impl.SmokeEncounterPeakTrails = std::max(impl.SmokeEncounterPeakTrails, size_t(std::count_if(snapshot.Nodes.begin(), snapshot.Nodes.end(), [](const auto& node) { return node.StartTime != 0; }))); impl.SmokeEncounterPeakBytes = std::max(impl.SmokeEncounterPeakBytes, capture.RawBytes); }
	if (impl.SmokeEncounter) {
		impl.SmokeEncounterPeakPixels = std::max(impl.SmokeEncounterPeakPixels, size_t(std::count_if(snapshot.Nodes.begin(), snapshot.Nodes.end(), [](const auto& node) { return node.Type == MP::World::Shape::Pixel; })));
		impl.SmokeEncounterPeakSprites = std::max(impl.SmokeEncounterPeakSprites, size_t(std::count_if(snapshot.Nodes.begin(), snapshot.Nodes.end(), [](const auto& node) { return node.Type == MP::World::Shape::Sprite; })));
	}
	if (!capture.Packed.empty() && impl.QueueWorld(peer, Kind::WorldSnapshot, {}, Delivery::State, 0, capture.Packed)) peer.QueuedSnapshotFrame = snapshot.ID;
	// Send the fresh state now. Waiting for the next loop's network update
	// added a whole simulation update (more under load) to every snapshot.
	impl.PumpWorld(peer, Now());
	impl.ProfileStage(Impl::ServerProfile::Queue, encoded);
	if (impl.SmokeEncounter && impl.SmokeStage == 3) {
		const uint64_t queued = NowMicros(); impl.SmokeCaptureTimes.push_back(queued - impl.SmokeCaptureStarted);
		impl.SmokeCaptureParts[0] += captured - impl.SmokeCaptureStarted; impl.SmokeCaptureParts[1] += composed - captured;
		impl.SmokeCaptureParts[2] += serialized - composed; impl.SmokeCaptureParts[3] += queued - serialized;
	}
}
size_t MultiplayerMan::Impl::QueueResource(Player& player, uint64_t id, bool scene) {
	// Scene resources stay queued even when no current state references them.
	if (scene) player.SceneResources.insert(id);
	if (player.WorldResources.contains(id)) return 0;
	if (auto pending = player.PendingResources.find(id); pending != player.PendingResources.end() && Now() - pending->second < 5000) return 0;
	if (auto message = player.ResourceMessages.find(id); message != player.ResourceMessages.end() && (player.QueuedResourceChunks.contains(message->second) || player.ResourceFlight.Contains(message->second))) return 0;
	const auto packed = PackedResource(id); if (packed.empty()) return 0;
	if (!QueueWorld(player, Kind::WorldResource, {}, Delivery::WorldResource, id, packed)) return 0;
	player.PendingResources[id] = Now(); return packed.size();
}
void MultiplayerMan::Impl::FillResources(Player& player) {
	// Only a short backlog is committed to the send queue, so a tile the view
	// needs is never behind seconds of particle sprites from a busy battle.
	const double limit = std::max(64.0 * 1024, player.Rate.Rate() * .25);
	double backlog = 0; for (const auto& packet : player.WorldPackets) if (packet.second == Delivery::WorldResource) backlog += packet.first.size();
	while (backlog < limit) {
		auto* queue = !player.UrgentQueue.empty() ? &player.UrgentQueue : !player.StateQueue.empty() ? &player.StateQueue : !player.SceneQueue.empty() ? &player.SceneQueue : nullptr;
		if (!queue) break;
		const uint64_t id = queue->front(); queue->pop_front();
		backlog += QueueResource(player, id, queue != &player.StateQueue);
	}
}
std::span<const uint8_t> MultiplayerMan::Impl::PackedResource(uint64_t id) {
	if (auto found = PackedResources.find(id); found != PackedResources.end()) return found->second;
	const auto* resource = World.FindResource(id); if (!resource) return {};
	MP::Writer payload(Kind::WorldResource); MP::World::WriteResource(payload, *resource);
	MP::Writer packed(Kind::WorldResource); packed.U32(uint32_t(payload.Data.size())); const size_t prefix = packed.Data.size();
	packed.Data.resize(prefix + LZ4_compressBound(int(payload.Data.size())));
	const int size = LZ4_compress_HC(reinterpret_cast<const char*>(payload.Data.data()), reinterpret_cast<char*>(packed.Data.data() + prefix), int(payload.Data.size()), int(packed.Data.size() - prefix), LZ4HC_CLEVEL_DEFAULT);
	if (size <= 0) return {};
	packed.Data.resize(prefix + size); packed.Data.shrink_to_fit();
	if (PackedResourceBytes + packed.Data.size() > PackedResourceLimit) { PackedResources.clear(); PackedResourceBytes = 0; }
	PackedResourceBytes += packed.Data.size();
	return PackedResources.emplace(id, std::move(packed.Data)).first->second;
}
bool MultiplayerMan::Impl::QueueWorld(Player& player, Kind kind, std::span<const uint8_t> payload, Delivery delivery, uint64_t resource, std::span<const uint8_t> prepared) {
	if (prepared.empty() && (payload.empty() || payload.size() > MP::World::MaxPayload)) return false;
	MP::Writer packed(kind);
	if (!prepared.empty()) packed.Data.assign(prepared.begin(), prepared.end());
	else {
		packed.U32(uint32_t(payload.size())); const size_t prefix = packed.Data.size(); packed.Data.resize(prefix + LZ4_compressBound(int(payload.size())));
		const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(payload.data()), reinterpret_cast<char*>(packed.Data.data() + prefix), int(payload.size()), int(packed.Data.size() - prefix));
		if (compressed <= 0) return false; packed.Data.resize(prefix + compressed);
	}
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
	if (kind == Kind::WorldSnapshot) ++SnapshotChunksReceived;
	if (Smoke && SmokeWorldLoss && kind == Kind::WorldSnapshot && !chunk.Parity && chunk.Index % ParityGroup == 0) return;
	// A relay's transport ACK covers only the first network leg. Guest receipts
	// bound reliable resources across both legs without delaying scene poses.
	if (kind != Kind::WorldSnapshot) { MP::Writer receipt(Kind::WorldAck, Session, Epoch); receipt.U8(3); receipt.U32(chunk.ID); receipt.U16(chunk.Index); Net.Send(ServerAddress, receipt.Data, Delivery::Receipt); }
	auto& assembly = kind != Kind::WorldSnapshot ? ResourceAssembly : SnapshotAssembly;
	auto complete = assembly.Push(chunk, Now()); if (!complete) return;
	if (kind == Kind::WorldSnapshot) ++SnapshotsAssembled;
	MP::Reader packed(*complete); Header header; uint32_t size;
	if (!ReadHeader(packed, header) || header.Type != kind || !packed.U32(size) || !size || size > MP::World::MaxPayload) return;
	std::vector<uint8_t> payload(size);
	if (LZ4_decompress_safe(reinterpret_cast<const char*>(packed.Rest().data()), reinterpret_cast<char*>(payload.data()), int(packed.Remaining()), int(size)) != int(size)) return;
	MP::Reader data(payload); if (!ReadHeader(data, header) || header.Type != kind) return;
	if (kind == Kind::WorldManifest) {
		std::unordered_set<uint64_t> assets; MP::World::SceneMap map; if (!MP::World::ReadManifest(data, assets, &map)) return;
		World.InstallSceneMap(map);
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
		MP::World::Snapshot snapshot;
		if (!MP::World::ReadSnapshot(data, snapshot, [&](uint32_t id) -> const MP::World::Snapshot* { const auto& stored = DecodedStates[id % DecodedStates.size()]; return stored.ID == id ? &stored : nullptr; })) return;
		DecodedStates[snapshot.ID % DecodedStates.size()] = snapshot;
		const auto missing = World.Missing(snapshot); MissingResources = {missing.begin(), missing.end()};
		// A content-addressed asset remains valid across rounds and reconnects.
		// Tell the host which cached assets need no second reliable transfer.
		for (const auto& node : snapshot.Nodes) if (node.Asset && World.FindResource(node.Asset) && ReportedResources.insert(node.Asset).second) {
			MP::Writer ack(Kind::WorldAck, Session, Epoch); ack.U8(1); ack.U64(node.Asset); Net.Send(ServerAddress, ack.Data, Delivery::Control);
		}
		// The first state waits for the scene map and every resource it shows,
		// which includes the terrain around the guest's view. The rest of the map
		// keeps streaming nearest-first instead of holding the whole match.
		if (!World.Ready() && (!SceneReceived || !World.SceneryReady(snapshot))) {
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
	if (!SceneReceived) return;
	for (size_t i = PendingWorlds.size(); i > 0; --i) if (World.SceneryReady(PendingWorlds[i - 1])) {
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
	BenchmarkRecord("world", now, LastFrameReceived && now >= LastFrameReceived ? now - LastFrameReceived : 0);
	LastFrameReceived = now;
	RecentWorldUpdates.push_back(now);
	while (RecentWorldUpdates.size() > 64) RecentWorldUpdates.pop_front();
	if (SmokeEncounter) {
		if (SmokeEncounterLastUpdate) SmokeEncounterGap = std::max(SmokeEncounterGap, now - SmokeEncounterLastUpdate);
		SmokeEncounterLastUpdate = now; ++SmokeEncounterUpdates;
	}
	std::erase_if(PendingWorlds, [id](const auto& pending) { return !Newer(pending.ID, id); });
	TextureWidth = width; TextureHeight = height; ++Presented;
	if (World.Updates() == 1) g_AudioMan.SetStreamListener(Vector(cameraX + width / 2, cameraY + height / 2), true);
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
	// Startup waits for scenery in view (a terrain tile here); objects never block it.
	MP::World::Node node; node.ID = (uint64_t(1) << 62) | (uint64_t(100) << 48); node.Flags = MP::World::Layer | MP::World::ScreenSpace; node.Asset = resource.ID; node.SourceWidth = node.SourceHeight = 1; snapshot.Nodes.push_back(node);
	bool passed = receive(snapshot);
	++snapshot.ID; ++snapshot.Time; snapshot.Nodes[0].Asset = resource.ID + 1; passed &= receive(snapshot);
	const bool held = !World.Ready(); passed &= held;
	World.Install(resource); PresentPendingWorlds();
	const bool baseline = held && World.Ready(); passed &= baseline;
	log << (baseline ? "OK: " : "FAIL: ") << "an attainable startup state survives newer missing resources\n";
	snapshot.Nodes[0] = {}; snapshot.Nodes[0].ID = 1; snapshot.Nodes[0].SourceWidth = snapshot.Nodes[0].SourceHeight = 1;
	for (int burst = 0; burst < 200; ++burst) { ++snapshot.ID; ++snapshot.Time; snapshot.Nodes[0].Asset = resource.ID + 2 + burst; snapshot.Nodes[0].X = float(burst); passed &= receive(snapshot); }
	const bool moving = World.Updates() == 201; passed &= moving;
	log << (moving ? "OK: " : "FAIL: ") << "200 combat updates continue while every new effect resource is delayed; updates=" << World.Updates() << '\n';
	World.Reset(); PendingWorlds.clear(); SceneReceived = false; SceneMissing.clear();
	World.Install(resource); snapshot.Nodes[0].Asset = resource.ID; ++snapshot.ID; ++snapshot.Time;
	passed &= receive(snapshot);
	const bool waiting = !World.Ready(); passed &= waiting;
	auto scenery = resource; scenery.Pixels[0] ^= 1; scenery.ID = MP::World::ResourceHash(scenery);
	MP::World::SceneMap map;
	auto tile = node; tile.ID = (uint64_t(1) << 62) | (uint64_t(100) << 48); tile.Flags = MP::World::Layer | MP::World::ScreenSpace; tile.Asset = scenery.ID;
	map.Nodes = {tile};
	const auto sceneAssets = MP::World::ManifestResources(std::vector<uint64_t>{resource.ID}, map);
	MP::Writer manifest(Kind::WorldManifest); MP::World::WriteManifest(manifest, sceneAssets, map);
	passed &= QueueWorld(sender, Kind::WorldManifest, manifest.Data, Delivery::WorldResource);
	while (!sender.WorldPackets.empty()) { auto packet = std::move(sender.WorldPackets.front().first); sender.WorldPackets.pop_front(); MP::Reader reader(packet); Header header; passed &= ReadHeader(reader, header); ReceiveWorld(header.Type, reader); }
	// This changed tile is in the guest's view, so it holds the first state.
	const bool streaming = !World.Ready() && SceneMissing.contains(scenery.ID); passed &= streaming;
	MP::Writer terrain(Kind::WorldResource); MP::World::WriteResource(terrain, scenery);
	passed &= QueueWorld(sender, Kind::WorldResource, terrain.Data, Delivery::WorldResource);
	while (!sender.WorldPackets.empty()) { auto packet = std::move(sender.WorldPackets.front().first); sender.WorldPackets.pop_front(); MP::Reader reader(packet); Header header; passed &= ReadHeader(reader, header); ReceiveWorld(header.Type, reader); }
	const bool warmed = World.Ready() && SceneMissing.empty(); passed &= warmed;
	log << (waiting && streaming && warmed ? "OK: " : "FAIL: ") << "guest waits for the scene map and its view, then streams the remaining scenery\n";
	log << (streaming && warmed ? "OK: " : "FAIL: ") << "reconnect map includes changed terrain resources absent from the original manifest\n";
	World.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear();
	ResetSceneTransfer();
	return passed;
}
void MultiplayerMan::Impl::Receive(const TransportEvent& event) {
	MP::Reader reader(event.Data); Header header;
	if (!ReadHeader(reader, header)) { if (State == Mode::Host && Pending.contains(event.Address)) Reject(event.Address, "Incompatible multiplayer protocol. Install the same game build as the host."); return; }
	if (State == Mode::Host) {
		if (header.Type == Kind::Hello && Pending.contains(event.Address)) {
			if (Dedicated && CloseStarted) { Reject(event.Address, "The room was closed."); return; }
			std::string name, version, owner; uint64_t token; uint16_t width, height;
			if (!reader.Text(name, 31) || name.empty() || !reader.U64(token) || !reader.Text(version) || !reader.U16(width) || width < 320 || width > 3840 || !reader.U16(height) || height < 180 || height > 2160 || !reader.Text(owner, Relay::TokenLength) || !reader.Done()) { Reject(event.Address, "Invalid player registration."); return; }
			if (version != c_GameVersion.str()) { Reject(event.Address, "Game version differs from the host. Install the same build."); return; }
			int slot = -1;
			if (token) for (int i = FirstRemote(); i < 4; ++i) if (Players[i].Token == token && !Players[i].Connected && Players[i].ReservedUntil > Now()) slot = i;
			if (slot < 0 && !Playing) {
				if (Dedicated && !OwnerRegistered && !owner.empty() && owner == OwnerCredential && !Players[0].Token) { slot = 0; OwnerRegistered = true; }
				else for (int i = Dedicated && OwnerRegistered ? 0 : 1; i < 4; ++i) if (!Players[i].Token) { slot = i; break; }
			}
			if (slot < 0) { Reject(event.Address, Playing ? "Match already started. Rejoin with your original player slot or wait for the lobby." : "All player slots are occupied or reserved for reconnecting players."); return; }
			if (Smoke && token && Players[slot].Token == token) { ++SmokeRejoins; Verify("REJOIN: player " + std::to_string(slot + 1)); }
			auto& player = Players[slot]; player.Address = event.Address; player.Name = name; player.Connected = true; player.Ready = Dedicated && slot == OwnerSlot; player.ReservedUntil = 0;
			if (!player.Token) player.Token = Token();
			player.ViewWidth = width; player.ViewHeight = height; if (Playing && token == player.Token) player.Inputs.ReleaseControls(); else player.Inputs.Reset(); ClearWorldPackets(player); player.WorldResources.clear(); player.PendingResources.clear(); player.LastWorld = 0; player.LastAck = Now(); player.AudioReady = false; player.ReplayLoops = true; Pending.erase(event.Address); Welcome(slot); return;
		}
		int slot = -1; for (int i = FirstRemote(); i < 4; ++i) if (Players[i].Connected && Players[i].Address == event.Address) slot = i;
		if (slot < 0 || header.Session != Session || (header.Epoch != Epoch && header.Type != Kind::Chat)) return;
		auto& player = Players[slot];
		if (header.Type == Kind::Input && Playing) { Input input; if (ReadInput(reader, input)) player.Inputs.Push(input, Now()); }
		else if (header.Type == Kind::Ready && !Playing) {
			uint8_t ready, team; uint32_t edit;
			if (reader.U8(ready) && ready <= 1 && reader.U8(team) && team < 4 && reader.U32(edit) && reader.Done()) {
				// A rejected team still answers the edit, so the guest stops showing it.
				if (AvailableTeams & (1 << team)) { player.Ready = ready != 0; player.Team = team; }
				player.AppliedEdit = edit; Lobby();
			}
		}
		else if (header.Type == Kind::Chat) { std::string message; if (reader.Text(message, 192) && !message.empty() && reader.Done() && Now() - player.LastChat > 250) { player.LastChat = Now(); AddChat(slot, message); } }
		else if (header.Type == Kind::WorldAck) {
			uint8_t type; if (!reader.U8(type)) return;
			if (type == 1) { uint64_t id; if (reader.U64(id) && reader.Done() && World.FindResource(id)) { player.PendingResources.erase(id); player.WorldResources.insert(id); } }
			else if (type == 3) { uint32_t message; uint16_t index; uint64_t sent = 0; if (reader.U32(message) && reader.U16(index) && reader.Done() && player.ResourceFlight.Receipt(message, index, &sent) && sent) player.Rate.Sample(Now() - sent); }
			else if (type == 2) { uint64_t id; if (reader.U64(id) && reader.Done() && World.FindResource(id)) {
				// A guest requests only what its view lacks, so it goes first, unless it is already on its way.
				const auto message = player.ResourceMessages.find(id);
				if (message == player.ResourceMessages.end() || !(player.QueuedResourceChunks.contains(message->second) || player.ResourceFlight.Contains(message->second))) { player.WorldResources.erase(id); player.PendingResources.erase(id); player.Urgent.insert(id); player.UrgentQueue.push_front(id); }
			} }
			else if (type == 0) { uint32_t id; if (reader.U32(id) && reader.Done() && !Newer(id, player.FrameID) && Newer(id, player.AckID)) {
				if (const auto& sent = player.SnapshotSentAt[id % player.SnapshotSentAt.size()]; sent.first == id && sent.second) player.Rate.Sample(Now() - sent.second);
				if (const auto& layers = player.SentLayers[id % player.SentLayers.size()]; layers.Frame == id) for (const auto& [node, asset] : layers.Layers) player.AckedLayers[node] = asset;
				player.AckID = id; player.LastAck = Now(); if (!player.AudioReady) g_MultiplayerMan.SetTextInputActive(slot, player.TextActive); player.AudioReady = true; } }
		}
		else if (header.Type == Kind::TextInput && Playing) { uint8_t direction; std::string text; if (reader.U8(direction) && direction == 0 && reader.Text(text, 64) && reader.Done() && player.TextActive && player.Text.size() + text.size() <= 256) { player.Text += text; if (Smoke && text == "Shop text test") { player.SmokeTextSeen = true; Verify("GUI TEXT: player " + std::to_string(slot + 1)); } } }
		else if (header.Type == Kind::RoomControl && Dedicated && slot == OwnerSlot) {
			std::string control, text; uint32_t value, edit;
			if (reader.Text(control, 32) && reader.U32(value) && reader.Text(text, 192) && reader.U32(edit) && reader.Done()) {
				player.AppliedEdit = edit; ControlRoom(slot, control, std::bit_cast<int32_t>(value), text);
				// Every answered edit is confirmed, including rejected and unchanged ones.
				if (control != "Close" && Players[slot].Connected) Lobby();
			}
		}
		else if (header.Type == Kind::Leave && reader.Done()) { if (Smoke && Dedicated && slot == 0 && SmokeStage == 3) SmokeHostedLeaveSequence = player.Inputs.LastSequence(); Net.Close(event.Address); player.Connected = false; player.Ready = false; player.ReservedUntil = Dedicated || Playing ? Now() + 60000 : 0; player.Inputs.ReleaseControls(); g_UInputMan.ClearRemoteInput(slot); Lobby(); }
		return;
	}
	if (event.Address != ServerAddress) return;
	if (header.Type == Kind::Reject && (State == Mode::Connecting || State == Mode::Reconnecting)) { std::string message; if (reader.Text(message) && reader.Done()) { Error = message; Stop(); UI = true; } return; }
	if (header.Type == Kind::Welcome && (State == Mode::Connecting || State == Mode::Reconnecting)) {
		uint8_t slot, dedicated; uint64_t token; std::string version;
		if (!header.Session || !reader.U8(slot) || slot > 3 || !reader.U64(token) || !token || !reader.Text(version) || version != c_GameVersion.str() || !reader.U8(dedicated) || dedicated > 1 || (!dedicated && slot == 0) || !reader.Done()) return;
		const bool sameMatch = (State == Mode::Reconnecting || ResumingSlot) && Session == header.Session && Epoch == header.Epoch && ReconnectToken == token;
		ResumingSlot = false;
		if (sameMatch) {
			for (size_t i = 0; i < InputCount; ++i) if (LocalInput.Held & (uint64_t(1) << i)) ++LocalInput.Releases[i];
			for (size_t i = 0; i < 3; ++i) if (LocalInput.MouseHeld & (1 << i)) ++LocalInput.MouseReleases[i];
			LocalInput.Held = 0; LocalInput.MouseHeld = 0; LocalInput.AimX = LocalInput.AimY = LocalInput.MoveX = LocalInput.MoveY = 0;
		} else LocalInput = {};
		Hosted = dedicated != 0; Session = header.Session; Epoch = header.Epoch; LocalSlot = slot; ReconnectToken = token; State = Mode::Client; PendingEdits.clear(); EditSequence = 0; Draft = {}; World.ResetPresentation(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); Texture = 0; LastRender = 0; ClearSounds(); UI = !Playing; Error.clear(); LastFrameReceived = Now(); return;
	}
	if (State != Mode::Client || header.Session != Session) return;
	if (header.Type == Kind::Chat) { uint8_t player; std::string message; if (reader.U8(player) && player < 4 && reader.Text(message, 192) && reader.Done()) AddChat(player, message); return; }
	if (header.Type == Kind::RoomControl) { std::string control, text; uint32_t value; if (reader.Text(control, 32) && reader.U32(value) && reader.Text(text, 192) && reader.Done() && control == "Error") Error = text; return; }
	if (header.Type == Kind::Leave && reader.Done()) { ResumeSlots.erase(ResumeKey()); const bool requested = CloseStarted != 0; Stop(); Error = requested ? "" : "The room was closed."; Notice = requested ? "Room closed." : ""; UI = true; return; }
	if (header.Type == Kind::Lobby && (header.Epoch == Epoch || Newer(header.Epoch, Epoch))) {
		uint8_t playing, dedicated, ownerSlot; std::string room, activity, scene;
		if (!reader.U8(playing) || playing > 1 || !reader.U8(dedicated) || dedicated > 1 || !reader.U8(ownerSlot) || ownerSlot > 3 || !reader.Text(room, 63) || !reader.Text(activity) || !reader.Text(scene)) return;
		uint8_t difficulty, fog, deploy, clearOrbit, teams, cpu; uint32_t gold; std::array<std::string, 4> tech;
		if (!reader.U8(difficulty) || difficulty > 100 || !reader.U32(gold) || gold > 1000000 || !reader.U8(fog) || fog > 1 || !reader.U8(deploy) || deploy > 1 || !reader.U8(clearOrbit) || clearOrbit > 1 || !reader.U8(teams) || teams > 15 || !reader.U8(cpu) || cpu > 4) return;
		for (auto& faction: tech) if (!reader.Text(faction)) return;
		struct Slot { uint8_t Occupied, Connected, Ready, Team; std::string Name; uint32_t AppliedEdit; }; std::array<Slot, 4> slots;
		for (auto& slot: slots) if (!reader.U8(slot.Occupied) || slot.Occupied > 1 || !reader.U8(slot.Connected) || slot.Connected > 1 || !reader.U8(slot.Ready) || slot.Ready > 1 || !reader.U8(slot.Team) || slot.Team > 3 || !reader.Text(slot.Name, 31) || !reader.U32(slot.AppliedEdit)) return;
		if (!reader.Done()) return;
		const bool changedMatch = Epoch != header.Epoch || Playing != (playing != 0);
		if (Epoch != header.Epoch) { Epoch = header.Epoch; LocalInput = {}; ClearSounds(); TextActive = false; LastFrameReceived = Now(); World.ResetPresentation(); ResetSceneTransfer(); SnapshotAssembly.Reset(); ResourceAssembly.Reset(); PendingWorlds.clear(); MissingResources.clear(); ReportedResources.clear(); Texture = 0; TextureWidth = TextureHeight = 0; }
		if (changedMatch && !playing && Playing) { Played = true; Notice = "Round complete. Ready up for the next match."; }
		if (changedMatch && playing) Notice.clear();
		Hosted = dedicated != 0; OwnerSlot = ownerSlot; Playing = playing != 0; RoomName = room; ActivityName = activity; SceneName = scene; if (!Playing || changedMatch) UI = !Playing;
		if (Hosted) {
			if (Activities.empty()) LoadActivities();
			for (int i = 0; i < Activities.size(); ++i) if (Activities[i]->GetPresetName() == activity) { ActivityIndex = i; LoadScenes(); break; }
			for (int i = 0; i < Scenes.size(); ++i) if (Scenes[i]->GetPresetName() == scene) SceneIndex = i;
			ActivityName = activity; SceneName = scene;
		}
		Difficulty = difficulty; Gold = static_cast<int>(gold); Fog = fog; Deploy = deploy; ClearOrbit = clearOrbit; AvailableTeams = teams; CPUTeam = cpu; Tech = std::move(tech);
		if (!Playing) g_AudioMan.SetStreamListener(Vector(), false);
		if (changedMatch && !Playing) { ClearSounds(); TextActive = false; SDL_StopTextInput(g_WindowMan.GetWindow()); }
		if (Smoke) Verify("LOBBY UPDATE: epoch=" + std::to_string(Epoch) + " playing=" + std::to_string(Playing));
		for (size_t i = 0; i < slots.size(); ++i) { Players[i].Token = slots[i].Occupied; Players[i].Connected = slots[i].Connected; Players[i].Ready = slots[i].Ready; Players[i].Team = slots[i].Team; Players[i].Name = slots[i].Name; }
		// Edits are answered in order, so everything up to the applied one is settled.
		const uint32_t applied = slots[LocalSlot].AppliedEdit; std::erase_if(PendingEdits, [applied](const auto& edit) { return edit.second.Sequence <= applied; });
		if (changedMatch) PendingEdits.clear();
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
	// Retransmissions may run between simulation steps. Accumulate each input
	// revision once while continuing to send the latest cumulative snapshot.
	if (LastSampledInputRevision != g_UInputMan.GetInputStateRevision()) {
		LastSampledInputRevision = g_UInputMan.GetInputStateRevision();
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
	}
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
	World.ExportLocalView(LocalInput, enabled);
	if (Now() - LastInputSent >= 8) { ++LocalInput.Sequence; LastInputSent = Now(); MP::Writer writer(Kind::Input, Session, Epoch); WriteInput(writer, LocalInput); Net.Send(ServerAddress, writer.Data, Delivery::Input); }
}

void MultiplayerMan::ApplyInputs() {
	if (!IsHostingMatch()) return;
	for (int i = m_Impl->FirstRemote(); i < 4; ++i) if (IsRemotePlayer(i)) g_UInputMan.SetRemoteInput(i, m_Impl->Players[i].Inputs.Consume(Now()));
}

void MultiplayerMan::Impl::MeasureSimulationSpeed(uint64_t now) {
	const Activity* activity = State == Mode::Host && Playing ? g_ActivityMan.GetActivity() : nullptr;
	if (!activity || activity->IsPaused()) { Overloaded = false; SpeedWindowStart = 0; return; }
	if (!SpeedWindowStart) { SpeedWindowStart = now; SpeedWindowTicks = g_TimerMan.GetSimTickCount(); return; }
	if (now - SpeedWindowStart < 500) return;
	const double simulatedMS = double(g_TimerMan.GetSimTickCount() - SpeedWindowTicks) * 1000.0 / double(g_TimerMan.GetTicksPerSecond());
	const double speed = simulatedMS / std::max(.01f, g_TimerMan.GetTimeScale()) / double(now - SpeedWindowStart);
	Overloaded = speed < .85 || (Overloaded && speed < .95);
	SpeedWindowStart = now; SpeedWindowTicks = g_TimerMan.GetSimTickCount();
}
void MultiplayerMan::Impl::Tick() {
	if (CursorVerification) return;
	FinishCaptures();
	const uint64_t now = Now(); LastUpdate = now;
	MeasureSimulationSpeed(now);
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
		else if (event.Kind == TransportEvent::Type::HostedRoom) {
			Hosted = HostedEndpoint = true; OwnerCredential.assign(event.Data.begin(), event.Data.end()); ServerAddress = event.Address;
			// The benchmark proxy forwards the actual AWS worker's UDP traffic.
			if (Benchmark && Smoke) if (const char* proxy = std::getenv("CCCP_MPBENCH_PROXY_BASE")) {
				std::string workerHost; uint16_t workerPort;
				const int base = std::atoi(proxy);
				if (base > 0 && base < 65535 && Address(ServerAddress, workerHost, workerPort) && workerPort >= 8100 && workerPort <= 8101)
					ServerAddress = "127.0.0.1:" + std::to_string(base + workerPort - 8100);
			}
			std::snprintf(JoinCode, sizeof(JoinCode), "%s", Net.RoomCode().c_str());
			std::string host; uint16_t port;
			if (!Address(ServerAddress, host, port) || !Net.Connect(host, port, Password, Error)) { Stop(); UI = true; }
			else { State = Mode::Connecting; ConnectStarted = now; NextRetry = now + 4000; Error.clear(); if (Smoke && SmokeRole == "host") { std::ofstream file("build-mp/room-code.txt"); file << JoinCode; } }
		}
		else if (event.Kind == TransportEvent::Type::RoomCode) { Error.clear(); Verify("ROOM CODE: " + Relay::DisplayCode(event.Address)); if (Smoke && State == Mode::Host) { std::ofstream file("build-mp/room-code.txt"); file << event.Address; } }
		else if (event.Kind == TransportEvent::Type::ServiceStatus) Error = event.Error;
		else if (event.Kind == TransportEvent::Type::Connected) { if (State == Mode::Host) Pending[event.Address] = now; else if (State == Mode::Connecting || State == Mode::Reconnecting) Hello(event.Address); }
		else if (event.Kind == TransportEvent::Type::Discovered) { MP::Reader reader(event.Data); Header header; std::string room; uint8_t count, playing; if (ReadHeader(reader, header) && header.Type == Kind::Announcement && reader.Text(room, 63) && reader.U8(count) && count <= 4 && reader.U8(playing) && playing <= 1 && reader.Done()) Discovered[event.Address] = room + " (" + std::to_string(count) + "/4" + (playing ? ", in game)" : ", lobby)"); }
		else if (event.Kind == TransportEvent::Type::Failed && HostedEndpoint && State == Mode::Connecting && event.Error.starts_with("Host did not respond.") && now - ConnectStarted < 120000) { Error = "Connecting to your AWS match server..."; NextRetry = now + 500; }
		else if (event.Kind == TransportEvent::Type::Failed) { Error = event.Error; if (State != Mode::Reconnecting) { Stop(); UI = true; } }
		else if (event.Kind == TransportEvent::Type::Disconnected) {
			if (State == Mode::Host) {
				Pending.erase(event.Address);
				for (int i = FirstRemote(); i < 4; ++i) if (Players[i].Address == event.Address && Players[i].Connected) { Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.ReleaseControls(); ClearWorldPackets(Players[i]); g_UInputMan.ClearRemoteInput(i); Lobby(); }
			} else if (State == Mode::Client && event.Address == ServerAddress) { State = Mode::Reconnecting; ConnectStarted = now; NextRetry = now + 1000; UI = true; Error = "Connection interrupted. Reconnecting to your player slot..."; g_UInputMan.TrapMousePos(false); ClearSounds(); }
		}
	}
	if (State == Mode::Connecting && HostedEndpoint && now >= NextRetry) Join(true);
	if (CloseStarted && State == Mode::Client) { if (now - CloseStarted > 10000) { CloseStarted = 0; Error = "The server has not confirmed closing the room. Try again."; } else if (now >= CloseRetry) { SendControl("Close"); CloseRetry = now + 1000; } }
	if (CloseStarted && Dedicated && now - CloseStarted > 3000) { Stop(); System::SetQuit(); return; }
	std::erase_if(ResumeSlots, [now](const auto& entry) { return entry.second.Expires <= now; });
	if (State == Mode::Connecting && now - ConnectStarted > (Hosted ? 120000 : 10000)) { Error = "Connection timed out. Check the host address and UDP port."; Stop(); UI = true; }
	if (State == Mode::Reconnecting) { if (now - ConnectStarted > 20000) { Error = "Unable to reconnect. Your host keeps the slot for 60 seconds."; Stop(); UI = true; } else if (now >= NextRetry) Join(true); }
	if (State == Mode::Host) {
		if (Playing && WarmingGuests && !g_ActivityMan.ActivitySetToRestart()) if (auto* activity = g_ActivityMan.GetActivity(); activity && g_SceneMan.GetScene()) {
			const bool ready = std::all_of(Players.begin() + FirstRemote(), Players.end(), [this](const auto& player) { return !player.Token || (Dedicated && !player.Connected) || (player.Connected && player.AckID != 0); });
			activity->SetPaused(!ready);
			if (ready) { WarmingGuests = false; Verify("SCENE: every guest has the complete battlefield"); }
		}
		for (auto it = Pending.begin(); it != Pending.end();) { if (now - it->second > 5000) { Net.Close(it->first); it = Pending.erase(it); } else ++it; }
		for (int i = FirstRemote(); i < 4; ++i) {
			auto& player = Players[i];
			if (!Playing && player.Token && !player.Connected && now >= player.ReservedUntil) { Net.Close(player.Address); player = Player(); Lobby(); }
			if (Playing && player.Connected) FillResources(player);
			PumpWorld(player, now);
			if (Playing && player.Token) SendAudio(i); else g_AudioMan.ClearSoundEvents(i);
		}
		if (!Dedicated) g_AudioMan.ClearSoundEvents(0);
	}
	SampleInput();
	while (!RecentWorldUpdates.empty() && now >= RecentWorldUpdates.front() && now - RecentWorldUpdates.front() >= 1000) RecentWorldUpdates.pop_front();
	if (Playing && now - LastNetworkLog >= 2000) {
		LastNetworkLog = now;
		// Bounded diagnostics contain timing and counters only, never room codes,
		// passwords, peer addresses or reconnect credentials.
		const bool host = State == Mode::Host;
		std::ofstream log(System::GetUserdataDirectory() + (host ? "LogMultiplayer-host.txt" : "LogMultiplayer-guest.txt"), NetworkLogSamples++ % 1800 ? std::ios::app : std::ios::trunc);
		log << "time_ms=" << now << " playing=" << Playing;
		if (host) {
			for (int i = FirstRemote(); i < 4; ++i) if (Players[i].Connected) log << " guest=" << i << " ack_age_ms=" << (now >= Players[i].LastAck ? now - Players[i].LastAck : 0) << " resource_flight=" << Players[i].ResourceFlight.Bytes() << ' ' << Net.Diagnostics(Players[i].Address);
		} else log << " render_fps=" << FPS << " world_hz=" << RecentWorldUpdates.size() << " update_age_ms=" << (LastFrameReceived && now >= LastFrameReceived ? now - LastFrameReceived : 0) << " transport_ping_ms=" << Net.Ping(ServerAddress) << " snapshot_chunks=" << SnapshotChunksReceived << " assembled=" << SnapshotsAssembled << ' ' << Net.Diagnostics(ServerAddress);
		log << '\n';
	}
	if (State == Mode::Client && Playing && (!PendingWorlds.empty() || !MissingResources.empty() || !SceneMissing.empty()) && now - LastResourceRequest > 1000) {
		LastResourceRequest = now;
		std::unordered_set<uint64_t> missing = MissingResources, scenery;
		for (const auto& snapshot : PendingWorlds) { for (auto id : World.Missing(snapshot)) missing.insert(id); for (auto id : World.MissingScenery(snapshot)) scenery.insert(id); }
		if (SceneReceived && now - SceneReceivedAt > 5000) missing.insert(SceneMissing.begin(), SceneMissing.end());
		// Requests repair what the host's own schedule has not delivered; the
		// scenery holding the first view goes ahead of objects and distant tiles.
		for (auto id : ResourceRequests.Select(missing, now, 2000, &scenery)) { MP::Writer request(Kind::WorldAck, Session, Epoch); request.U8(2); request.U64(id); Net.Send(ServerAddress, request.Data, Delivery::Control); }
	}
	if (Dedicated) {
		if (!OwnerRegistered && now >= OwnerClaimDeadline && std::any_of(Players.begin(), Players.end(), [](const auto& player) { return player.Connected; })) { OwnerRegistered = true; OwnerCredential.clear(); Lobby(); }
		if (std::any_of(Players.begin(), Players.end(), [](const auto& player) { return player.Connected; })) DedicatedLastOccupied = now;
		if (now - DedicatedLastOccupied >= uint64_t(DedicatedIdleSeconds) * 1000) { Stop(); System::SetQuit(); return; }
		if (now - DedicatedLastStatus >= 1000 && !DedicatedStatus.empty()) { DedicatedLastStatus = now; std::ofstream status(DedicatedStatus, std::ios::trunc); status << "ready=1\nconnected=" << std::count_if(Players.begin(), Players.end(), [](const auto& player) { return player.Connected; }) << "\nplaying=" << Playing << "\nheadless=" << g_WindowMan.IsHeadless() << '\n'; }
	}
	SmokeTick();
	if (Benchmark && now - BenchmarkLastSample >= 100) {
		BenchmarkLastSample = now;
		BenchmarkRecord("sample", now, LastFrameReceived && now >= LastFrameReceived ? now - LastFrameReceived : 0);
		BenchmarkLog.flush();
	}
	if (!CursorVerification && g_MenuMan.GetIsInMenuScreen()) UpdateMenu();
}

void MultiplayerMan::Impl::PumpWorld(Player& player, uint64_t now) {
	// Keep the upload limit meaningful on short WAN queues too. Saving
	// 256 KB of idle credit let one render tick flood a home uplink even
	// when its average send rate was below the configured limit. Credit
	// accrues only for elapsed time, but a slow loop may spend everything that
	// accrued since the previous pump; discarding it held a 44 ms loop to less
	// than half of the configured rate.
	const double cap = BandwidthMbps * 125000.0 * .85;
	if (!player.Rate.Cap()) player.Rate.Reset(cap, now); else player.Rate.SetCap(cap);
	player.Rate.Update(now, Net.PacketLoss(player.Address));
	const double worldRate = player.Rate.Rate();
	const double elapsed = player.LastPump ? std::min<uint64_t>(100, now - player.LastPump) / 1000.0 : .020;
	player.LastPump = now;
	const double burstBytes = std::max({2400.0, worldRate * .020, worldRate * elapsed});
	player.Budget = std::min(burstBytes, player.Budget + elapsed * worldRate);
	// The transport queue is read once; this pump's own sends are added to it.
	uint64_t transportQueue = Net.QueuedBytes(player.Address);
	while (!player.WorldPackets.empty() && transportQueue < 128 * 1024) {
		const auto& [bytes, delivery] = player.WorldPackets.front();
		if (player.Budget < bytes.size() + 80) { player.Rate.Limited(); break; }
		if (delivery == Delivery::WorldResource && !player.ResourceFlight.CanSend(bytes.size())) break;
		if (!Net.Send(player.Address, bytes, delivery)) break;
		if (delivery == Delivery::State) player.SnapshotStarted = true;
		else if (delivery == Delivery::WorldResource) {
			MP::Reader packet(bytes); Header header; MP::World::Chunk chunk;
			if (ReadHeader(packet, header) && MP::World::ReadChunk(packet, chunk)) {
				player.ResourceFlight.Sent(chunk.ID, chunk.Index, bytes.size(), now);
				if (auto queued = player.QueuedResourceChunks.find(chunk.ID); queued != player.QueuedResourceChunks.end() && !--queued->second) player.QueuedResourceChunks.erase(queued);
			}
		}
		transportQueue += bytes.size();
		player.Budget -= bytes.size() + 80; player.BytesSent += bytes.size() + 80; player.WorldPackets.pop_front();
	}
	if (player.SnapshotStarted && std::none_of(player.WorldPackets.begin(), player.WorldPackets.end(), [](const auto& packet) { return packet.second == Delivery::State; })) {
		// The whole snapshot has left; its acknowledgement measures the round trip.
		player.SnapshotStarted = false;
		if (player.QueuedSnapshotFrame) player.SnapshotSentAt[player.QueuedSnapshotFrame % player.SnapshotSentAt.size()] = {player.QueuedSnapshotFrame, now};
	}
}

void MultiplayerMan::Impl::SmokeTick() {
	if (!Smoke) return; if (Dedicated || Hosted) { HostedSmokeTick(); return; } const auto now = Now();
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
		if (SmokeStage == 1 && !SmokeReconnected && now - SmokeStageTime > 5000 && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.AckID > 0; })) {
			if (SmokeCombatStress) {
				auto* terrain = g_SceneMan.GetScene()->GetTerrain();
				const int y = g_SceneMan.GetSceneHeight() - 16;
				terrain->SetFGColorPixel(32, y, terrain->GetFGColorPixel(32, y) == g_MaskColor ? g_WhiteColor : g_MaskColor);
				const auto map = World.PrepareSceneMap(Players[1].ViewWidth, Players[1].ViewHeight);
				const std::unordered_set<uint64_t> original(SceneManifest.begin(), SceneManifest.end());
				const bool changed = std::any_of(map.Nodes.begin(), map.Nodes.end(), [&](const auto& tile) { return tile.Asset && !original.contains(tile.Asset); });
				Verify(changed ? "OK: terrain revision before reconnect is absent from the original manifest" : "FAIL: changed terrain reconnect fixture did not create a new resource");
			}
			SmokeReconnected = true;
			for (int i = 1; i <= SmokeGuests; ++i) { Net.Close(Players[i].Address); Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.ReleaseControls(); ClearWorldPackets(Players[i]); }
			Lobby(); Verify("RECONNECT: interrupted guest connections");
		}
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

void MultiplayerMan::Impl::HostedSmokeTick() {
	const auto now = Now();
	if (now - SmokeStarted > 240000) { Verify("FAIL: hosted multiplayer timed out: " + Error); System::SetQuit(true); return; }
	if (Dedicated) {
		if (Playing && WarmingGuests && now - SmokeLastStats > 3000) {
			SmokeLastStats = now;
			for (int player = 0; player < 4; ++player) if (const auto& peer = Players[player]; peer.Connected) Verify("WARMING: player " + std::to_string(player) + " ack=" + std::to_string(peer.AckID) + " scene_queue=" + std::to_string(peer.SceneQueue.size()) + " packets=" + std::to_string(peer.WorldPackets.size()) + " flight=" + std::to_string(peer.ResourceFlight.Bytes()) + " rate_kbps=" + std::to_string(int(peer.Rate.Rate() * 8 / 1000)) + " queue_ms=" + std::to_string(peer.Rate.QueueDelay()) + " " + Net.Diagnostics(peer.Address));
		}
		if (Playing && WarmingGuests) { SmokeStageTime = now; return; }
		if (SmokeStage == 0 && Playing) { SmokeStage = 1; SmokeStageTime = now; Verify("HOSTED: dedicated server started first match with every human remote"); }
		if (SmokeStage == 1 || SmokeStage == 3) {
			if (SmokeStage == 1) for (int player = 0; player <= SmokeGuests; ++player) SmokeGUIKeySeen[player] |= g_UInputMan.KeyHeld(SDL_SCANCODE_BACKSPACE, player);
			for (int player = 0; player <= SmokeGuests; ++player) if (auto* actor = g_ActivityMan.GetActivity()->GetControlledActor(player)) {
				if (!SmokeActorSeen[player]) { SmokeActorSeen[player] = true; SmokeActorStartX[player] = actor->GetPos().GetX(); }
				SmokeActorMoved[player] |= std::abs(actor->GetPos().GetX() - SmokeActorStartX[player]) > 5;
				SmokeFireSeen[player] |= actor->GetController()->IsState(ControlState::WEAPON_FIRE);
			}
			if (!SmokeLoop && !SmokeLoopStopped && now - SmokeStageTime > 1500) {
				for (int player = 0; player <= SmokeGuests; ++player) g_MultiplayerMan.SetTextInputActive(player, true);
				SmokeLoop = std::make_unique<SoundContainer>(); SmokeLoop->GetTopLevelSoundSet().AddSound("Base.rte/Sounds/Craft/ThrusterLoop.flac", false); SmokeLoop->SetLoopSetting(-1); SmokeLoop->SetImmobile(true); SmokeLoop->SetVolume(.013f); SmokeLoop->Play();
			}
			if (SmokeLoop && now - SmokeStageTime > 10000) { SmokeLoop->Stop(); SmokeLoop.reset(); SmokeLoopStopped = true; }
			if (SmokeCombatStress && SmokeStage == 3 && now - SmokeStageTime > 3000 && now - SmokeStageTime < 9500 && now - SmokeLastExplosion >= 250) {
				SmokeLastExplosion = now;
				if (const auto* preset = g_PresetMan.GetEntityPreset("TDExplosive", "Frag Grenade", "Base.rte")) for (int player = 0; player <= SmokeGuests; ++player) if (auto* actor = g_ActivityMan.GetActivity()->GetControlledActor(player)) { auto* grenade = dynamic_cast<MOSRotating*>(preset->Clone()); grenade->SetPos(actor->GetPos() + Vector(180, -60)); grenade->GibThis(Vector(), actor); delete grenade; ++SmokeExplosions; }
			}
		}
		if (SmokeStage == 1 && !SmokeReconnected && now - SmokeStageTime > 5000 && Players[0].Connected && Players[0].AckID) {
			SmokeReconnected = true; SmokeHostedSession = Session; SmokeHostedOtherFrame = Players[1].FrameID;
			Net.Close(Players[0].Address); Players[0].Connected = Players[0].Ready = false; Players[0].ReservedUntil = now + 60000; Players[0].Inputs.ReleaseControls(); ClearWorldPackets(Players[0]); g_UInputMan.ClearRemoteInput(0); Lobby();
			Verify("HOSTED: interrupted creator connection while server and other players stay in combat");
		}
		if (SmokeStage == 1 && SmokeReconnected && !Players[0].Connected && Players[1].FrameID > SmokeHostedOtherFrame + 5 && Session == SmokeHostedSession && Playing) SmokeHostedContinued = true;
		if (SmokeStage == 1 && now - SmokeStageTime > 16000) {
			if (!SmokeHostedContinued || SmokeRejoins < 1 || !Players[0].Connected || Session != SmokeHostedSession) { Verify("FAIL: owner disconnect interrupted authority or creator failed to reconnect"); System::SetQuit(true); return; }
			Verify("PASS: owner disconnect preserves the match; server keeps sending states and creator rejoins original slot");
			ReturnToLobby(); SmokeStage = 2; SmokeStageTime = now;
			for (int i = 0; i < Activities.size(); ++i) if (Activities[i]->GetPresetName() == "One-Man Army") { ActivityIndex = i; LoadScenes(); break; }
			SmokeActorSeen.fill(false); SmokeActorMoved.fill(false); SmokeFireSeen.fill(false); SmokeLoopStopped = false; SettingsChanged();
		}
		if (SmokeStage == 2 && Playing) { SmokeStage = 3; SmokeStageTime = now; Verify("HOSTED: room owner started rematch on independent server"); }
		if (SmokeStage == 3 && now - SmokeStageTime > 35000) {
			bool passed = g_WindowMan.IsHeadless() && g_WindowMan.GetWindow() == nullptr && OwnerSlot != 0 && Players[OwnerSlot].Connected && SmokeRejoins >= 2 && SmokeHostedLeaveSequence && Players[0].Inputs.LastSequence() > SmokeHostedLeaveSequence + 20;
			for (int player = 0; player <= SmokeGuests; ++player) {
				const auto& peer = Players[player];
				passed &= peer.Connected && peer.AckID && peer.Inputs.LastSequence() && SmokeActorMoved[player] && SmokeFireSeen[player] && SmokeGUIKeySeen[player] && peer.SmokeTextSeen && (!SmokeCombatStress || (peer.SmokeRadialKeyboardSeen && peer.SmokeRadialMouseSeen && peer.SmokeQCameraSeen && peer.SmokeQCameraMoved));
				Verify("REMOTE PLAYER: " + std::to_string(player) + " input=" + std::to_string(Players[player].Inputs.LastSequence()) + " moved=" + std::to_string(SmokeActorMoved[player]) + " fired=" + std::to_string(SmokeFireSeen[player]));
				Verify("REMOTE CONTROLS: " + std::to_string(player) + " GUI key=" + std::to_string(SmokeGUIKeySeen[player]) + " text=" + std::to_string(peer.SmokeTextSeen) + " digital radial=" + std::to_string(peer.SmokeRadialKeyboardSeen) + " mouse radial=" + std::to_string(peer.SmokeRadialMouseSeen) + " Q selection=" + std::to_string(peer.SmokeQCameraSeen) + " Q camera moved=" + std::to_string(peer.SmokeQCameraMoved));
			}
			if (!passed) { Verify("FAIL: dedicated runtime, remote controls or combat simulation"); System::SetQuit(true); return; }
			g_ActivityMan.EndActivity(); SmokeStage = 4; SmokeStageTime = now;
		}
		if (SmokeStage == 4 && !Playing) { Verify("PASS: headless dedicated authority, all remote inputs, combat, owner transfer, reconnect and two-match lobby lifecycle"); SmokeStage = 5; }
		return;
	}
	if (State == Mode::Client) {
		if (Playing && !World.Ready() && now - SmokeLastStats > 3000) {
			SmokeLastStats = now; std::unordered_set<uint64_t> pending, scenery; for (const auto& snapshot : PendingWorlds) { for (auto id : World.Missing(snapshot)) pending.insert(id); for (auto id : World.MissingScenery(snapshot)) scenery.insert(id); }
			Verify("LOADING: scene_received=" + std::to_string(SceneReceived) + " scene_missing=" + std::to_string(SceneMissing.size()) + " pending_states=" + std::to_string(PendingWorlds.size()) + " state_missing=" + std::to_string(pending.size()) + " scenery_missing=" + std::to_string(scenery.size()) + " " + Net.Diagnostics(ServerAddress));
		}
		if (!SmokeChatSent) { SmokeChatSent = true; MP::Writer chat(Kind::Chat, Session, Epoch); chat.Text("Hosted room chat test", 192); Net.Send(ServerAddress, chat.Data, Delivery::Control); }
		if (!Playing && (SmokeStage == 0 || SmokeStage == 2) && now - SmokeReadySent > 1000) {
			if (Benchmark && Admin()) if (const char* upload = std::getenv("CCCP_MPBENCH_UPLOAD")) {
				// The lobby does not report the worker's upload setting, so request it
				// once instead of comparing with this client's unrelated default.
				const int target = std::clamp(std::atoi(upload), 1, 48);
				if (!BenchmarkUploadRequested) { BenchmarkUploadRequested = true; BandwidthMbps = target; SendControl("Bandwidth", target); SmokeReadySent = now; Verify("BENCHMARK: requested upload Mbps=" + std::to_string(target)); return; }
			}
			if (!Menu) UpdateMenu();
			// The owner's typed setting shows at once and stays until the server answers it.
			if (Admin() && SmokeStage == 0 && SmokeLobbyEdit == 0) {
				SmokeLobbyEditTarget = Difficulty == 100 ? 99 : Difficulty + 1; Draft = {"Difficulty", std::to_string(SmokeLobbyEditTarget), now}; CommitDraft();
				if (MenuView().Difficulty != SmokeLobbyEditTarget) { Verify("FAIL: owner lobby edit is not shown while pending"); System::SetQuit(); return; }
				SmokeLobbyEdit = 1; SmokeReadySent = now; return;
			}
			if (SmokeLobbyEdit == 1) {
				if (!PendingEdits.empty() || Difficulty != SmokeLobbyEditTarget) { SmokeReadySent = now; return; }
				SmokeLobbyEdit = 2; Verify("LOBBY EDIT: owner setting shown while pending and confirmed by the server");
			}
			if (Admin()) { if (std::count_if(Players.begin(), Players.end(), [](const auto& peer) { return peer.Connected && peer.Ready; }) == SmokeGuests + 1) { Menu->QueueVerificationClick("Primary"); UpdateMenu(); Verify("MENU: native hosted owner start button"); } }
			else if (!Players[LocalSlot].Ready) { Menu->QueueVerificationClick("Primary"); UpdateMenu(); Verify("MENU: native hosted player ready button"); }
			SmokeReadySent = now;
		}
		if (Playing && World.Ready() && !World.Paused() && SmokeStage == 0) { SmokeStage = 1; SmokeStageTime = now; SmokeCapture = true; Verify("HOSTED: client renders server-authoritative match"); }
		if (SmokeStage == 1 && !Playing) { SmokeStage = 2; SmokeCapture = true; Verify("HOSTED: returned to same lobby after creator reconnect"); }
		if (SmokeStage == 2 && Playing && World.Ready() && !World.Paused()) { SmokeStage = 3; SmokeStageTime = now; SmokeSecondStart = Presented; SmokeCapture = true; }
		if (SmokeStage == 3 && Playing) SmokeHostedIndependent |= World.Rendered() > World.Updates() && World.IntermediateFrames() > 20;
		if (SmokeStage == 3 && Playing && World.Ready() && !SmokeHostedCombatCaptured && now - SmokeStageTime > 3500) { SmokeHostedCombatCaptured = SmokeCapture = true; }
		if (SmokeStage == 3 && SmokeRole == "host" && !SmokeHostedLeft && now - SmokeStageTime > 4000) {
			UI = true; UpdateMenu(); Menu->QueueVerificationClick("Leave"); UpdateMenu(); Menu->QueueVerificationClick("ConfirmLeave"); UpdateMenu();
			SmokeHostedLeft = true; SmokeHostedLeaveAt = now; Verify("LEAVE: creator used native leave confirmation during combat"); return;
		}
		if (SmokeHostedLeft && !SmokeHostedReturned && Playing && World.Ready() && !World.Paused()) { SmokeHostedReturned = true; SmokeCapture = true; SmokeStageTime = now; SmokeHostedCombatCaptured = false; SmokeRadialKeyboardCaptured = SmokeRadialMouseCaptured = SmokeQCameraCaptured = false; SmokeAutoHeld = 0; Verify(LocalSlot == 0 ? "PASS: native leave and room-code join restores creator's original slot during combat" : "FAIL: native leave and join lost player slot"); }
		if (SmokeStage == 3 && !Playing) {
			const bool passed = Played && Presented > SmokeSecondStart + 20 && SmokeHostedIndependent && (SmokeRole != "host" || (SmokeHostedReturned && SmokeLobbyEdit == 2));
			Verify(passed ? "PASS: hosted client received two matches, combat updates and persistent lobby" : "FAIL: hosted client world updates or rematch");
			SmokeStage = 4; SmokeStageTime = now; SmokeCapture = true;
		}
		if (SmokeStage == 4 && !Playing && now - SmokeStageTime > 2000) {
			SmokeStage = 5;
			if (Admin()) { Menu->QueueVerificationClick("CloseRoom"); UpdateMenu(); Menu->QueueVerificationClick("ConfirmCloseRoom"); UpdateMenu(); Verify("CLOSE: owner requested explicit hosted shutdown"); }
		}
	} else if (State == Mode::Idle && SmokeStage == 3 && SmokeHostedLeft && now - SmokeHostedLeaveAt > 1000) { Join(); Verify("JOIN: creator used room-code join after native leave"); }
	else if (State == Mode::Idle && SmokeStage >= 4) { if (Notice == "Room closed.") Verify("PASS: owner explicitly closes hosted room"); System::SetQuit(); }
}

void MultiplayerMan::Impl::EncounterTick() {
	const auto now = Now();
	// A stall that lasts through disconnect or lobby return has no next frame
	// to close its interval. Include that time in the delivery assertion too.
	if (State != Mode::Host && SmokeStage == 1 && SmokeEncounterLastUpdate) SmokeEncounterGap = std::max(SmokeEncounterGap, now - SmokeEncounterLastUpdate);
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
			else {
				Verify(std::string(SmokeEncounterUpdates > 100 && SmokeEncounterGap < 500 ? "PASS: " : "FAIL: ") + "delivered soldier combat updates=" + std::to_string(SmokeEncounterUpdates) + " largest gap ms=" + std::to_string(SmokeEncounterGap));
				Verify(std::string(AudioReceived > 20 ? "PASS: " : "FAIL: ") + "combat network audio remains enabled; messages=" + std::to_string(AudioReceived));
			}
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
		if (const char* upload = std::getenv("CCCP_MPSMOKE_UPLOAD")) BandwidthMbps = std::clamp(std::atoi(upload), 1, 48);
		if (SmokeExplosionBurst) { BandwidthMbps = std::getenv("CCCP_MPSMOKE_BURST_UPLOAD") ? std::clamp(std::atoi(std::getenv("CCCP_MPSMOKE_BURST_UPLOAD")), 1, 24) : 4; Verify("BURST: combat upload Mbps=" + std::to_string(BandwidthMbps)); }
		if (SmokeNativeBaseline) { World.Reset(); Verify("BASELINE: native AI battle with guest capture disabled"); }
	}
	if (SmokeStage == 3) {
		if (SmokeCombatStress && now - SmokeEncounterStart > 7000 && now - SmokeEncounterStart < 18000 && now - SmokeLastExplosion > (SmokeExplosionBurst ? 3000 : 250)) {
			SmokeLastExplosion = now;
			const auto* preset = g_PresetMan.GetEntityPreset("TDExplosive", "Frag Grenade", "Base.rte");
			for (int item = 0; item < (SmokeExplosionBurst ? 12 : 1); ++item) {
				auto* grenade = dynamic_cast<MOSRotating*>(preset->Clone()); grenade->SetPos(g_SceneMan.MovePointToGround(Vector(1570 + item * 8, 0), 100, 1)); grenade->GibThis(); delete grenade; ++SmokeExplosions;
			}
			if (SmokeExplosionBurst) {
				const auto* craft = g_PresetMan.GetEntityPreset("ACDropShip", "Dropship MK1", "Base.rte");
				for (int item = 0; item < 2; ++item) { auto* wreck = dynamic_cast<MOSRotating*>(craft->Clone()); wreck->SetPos(g_SceneMan.MovePointToGround(Vector(1540 + item * 120, 0), 180, 1)); wreck->GibThis(); delete wreck; ++SmokeExplosions; }
				Verify("BURST: destroyed twelve grenades and two dropships");
			}
		}
		for (int player = 0; player <= SmokeGuests; ++player) for (auto* actor : *g_MovableMan.GetTeamRoster(game->GetTeamOfPlayer(player))) if (actor->GetPresetName() == "Soldier Light") {
			SmokeEncounterSoldiers.insert(actor->GetUniqueID());
			if (actor->GetController()->IsState(ControlState::WEAPON_FIRE)) SmokeEncounterFired.insert(actor->GetUniqueID());
			if (actor->GetHealth() < 99) SmokeEncounterDamaged.insert(actor->GetUniqueID());
		}
		if (now - SmokeEncounterStart > 22000) {
			// Burst waves may destroy the soldiers before their AI fires. This
			// fixture proves explosion delivery/damage; ordinary battles require firing.
			const bool combat = SmokeExplosionBurst ? SmokeExplosions == 56 && SmokeEncounterSoldiers.size() == size_t((SmokeGuests + 1) * 6) : !SmokeEncounterFired.empty();
			const bool passed = SmokeEncounterSoldiers.size() >= 2 && combat && !SmokeEncounterDamaged.empty();
			Verify(std::string(passed ? "PASS: " : "FAIL: ") + "delivered soldiers=" + std::to_string(SmokeEncounterSoldiers.size()) + " firing=" + std::to_string(SmokeEncounterFired.size()) + " damaged=" + std::to_string(SmokeEncounterDamaged.size()));
			Verify("ENCOUNTER: peak nodes=" + std::to_string(SmokeEncounterPeakNodes) + " trails=" + std::to_string(SmokeEncounterPeakTrails) + " wire bytes=" + std::to_string(SmokeEncounterPeakBytes) + " explosions=" + std::to_string(SmokeExplosions));
			Verify("ENCOUNTER: peak compressed bytes=" + std::to_string(SmokeEncounterPeakPacked) + " mean compressed bytes=" + std::to_string(SmokeEncounterWireUpdates ? SmokeEncounterWireBytes / SmokeEncounterWireUpdates : 0));
			Verify("ENCOUNTER: peak particle pixels=" + std::to_string(SmokeEncounterPeakPixels) + " sprites=" + std::to_string(SmokeEncounterPeakSprites));
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
void MultiplayerMan::Impl::GuestInputChecks(GameActivity* game) {
	if (SmokeGuestInputChecked) return;
	SmokeGuestInputChecked = true;
	if (State == Mode::Client) {
		// Render frames can outnumber simulation steps. Repeated sampling before
		// EndFrame must not turn one physical click into several network clicks.
		const auto presses = LocalInput.MousePresses[0], releases = LocalInput.MouseReleases[0];
		SDL_Event event{}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		event.button.button = SDL_BUTTON_LEFT; event.button.down = true;
		g_UInputMan.HandleInputEvent(event); SampleInput(); SampleInput();
		g_UInputMan.EndFrame();
		event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.down = false;
		g_UInputMan.HandleInputEvent(event); SampleInput(); SampleInput();
		g_UInputMan.EndFrame();
		const bool passed = uint16_t(LocalInput.MousePresses[0] - presses) == 1 && uint16_t(LocalInput.MouseReleases[0] - releases) == 1;
		Verify(passed ? "PASS: one physical guest click sampled across multiple renders stays one click" : "FAIL: one physical guest click was sampled multiple times");
		SmokeDeploymentFailed |= !passed;
		return;
	}
	// Exercise the real remote-input/controller/editor chain while deployment
	// fog is intact. Unlike the visibility fixture, no placed actor is injected.
	const int player = 1, team = game->GetTeamOfPlayer(player);
	auto* editor = game->GetEditorGUI(player); auto* controller = game->GetPlayerController(player);
	const Vector position = g_SceneMan.MovePointToGround(Vector(950, 0), 60, 1);
	InputReceiver receiver; Input input; input.Device = uint8_t(InputDevice::DEVICE_MOUSE_KEYB);
	auto step = [&] {
		++input.Sequence; receiver.Push(input, Now()); g_UInputMan.SetRemoteInput(player, receiver.Consume(Now()));
		controller->Update(); editor->Update(); g_UInputMan.EndFrame();
	};
	for (const auto* preset : {g_PresetMan.GetEntityPreset("AHuman", "Soldier Light", "Coalition.rte"), g_PresetMan.GetEntityPreset("HDFirearm", "Assault Rifle", "Coalition.rte")}) {
		editor->SetCurrentObject(dynamic_cast<SceneObject*>(preset->Clone())); editor->SetCursorPos(position);
		editor->SetEditorGUIMode(SceneEditorGUI::ADDINGOBJECT); step();
		const float funds = game->GetTeamFunds(team);
		input.MouseHeld = 1; ++input.MousePresses[0]; step(); step();
		input.MouseHeld = 0; ++input.MouseReleases[0]; step(); step();
		const bool passed = game->GetTeamFunds(team) < funds && g_SceneMan.IsUnseen(position.GetFloorIntX(), position.GetFloorIntY(), team);
		Verify(std::string(passed ? "PASS: guest placed " : "FAIL: guest could not place ") + preset->GetPresetName() + " during deployment with fog preserved");
		SmokeDeploymentFailed |= !passed;
	}
	class CartFixture : public BuyMenuGUI {
	public:
		Vector Prepare(const Entity* preset) {
			SetEnabled(true); m_pParentBox->SetPositionAbs(0, 0);
			m_pCartList->ClearList();
			for (int i = 0; i < 3; ++i) m_pCartList->AddItem(preset->GetPresetName(), "", nullptr, preset);
			return Vector(m_pCartList->GetXPos() + 12, m_pCartList->GetYPos() + 8);
		}
		size_t Count() { return m_pCartList->GetItemList()->size(); }
	};
	CartFixture cart; cart.Create(controller);
	const Vector pointer = cart.Prepare(g_PresetMan.GetEntityPreset("HDFirearm", "Assault Rifle", "Coalition.rte"));
	receiver.Reset(); input = {}; input.Device = uint8_t(InputDevice::DEVICE_MOUSE_KEYB);
	auto cartStep = [&] {
		++input.Sequence; receiver.Push(input, Now()); g_UInputMan.SetRemoteInput(player, receiver.Consume(Now()));
		g_UInputMan.SetAbsoluteMousePosition(pointer * g_WindowMan.GetResMultiplier(), player);
		controller->Update(); cart.Update(); g_UInputMan.EndFrame();
	};
	cartStep(); cartStep();
	input.MouseHeld = 1; input.Held = uint64_t(1) << INPUT_FIRE; ++input.MousePresses[0]; ++input.Presses[INPUT_FIRE]; cartStep();
	input.MouseHeld = 0; input.Held = 0; ++input.MouseReleases[0]; ++input.Releases[INPUT_FIRE]; cartStep(); cartStep();
	const bool cartPassed = cart.Count() == 2;
	Verify(std::string(cartPassed ? "PASS: " : "FAIL: ") + "one guest cart click leaves " + std::to_string(cart.Count()) + " of three items (expected two)");
	SmokeDeploymentFailed |= !cartPassed;
	cart.Destroy(); g_UInputMan.ClearRemoteInput(player);
}

void MultiplayerMan::Impl::DeploymentTick() {
	const auto now = Now();
	if (State == Mode::Idle && SmokeStage == 5) { System::SetQuit(true); return; }
	if (now - SmokeStarted > 60000) { Verify("FAIL: deployment timed out: " + Error); System::SetQuit(true); return; }
	if (State == Mode::Client) {
		if (!Playing && SmokeStage == 0 && !Players[LocalSlot].Ready) SendReady(true, Players[LocalSlot].Team);
		if (Playing && World.Ready() && !World.Paused() && SmokeStage == 0) { SmokeStage = 1; SmokeStageTime = SmokeDeploymentStart = now; SmokeDeploymentCount = Presented; SmokeCapture = true; }
		if (SmokeGuestInput && Playing && World.Ready() && !World.Paused() && SmokeStage > 0) GuestInputChecks(nullptr);
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
		if (SmokeGuestInput) GuestInputChecks(game);
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
	const uint64_t now = Now();
	const double elapsed = peer.LastAudioBudget ? std::min<uint64_t>(100, now - peer.LastAudioBudget) / 1000.0 : .050;
	peer.LastAudioBudget = now;
	peer.AudioBudget = std::min(2400.0, peer.AudioBudget + elapsed * std::min(48000.0, BandwidthMbps * 125000.0 * .10));
	auto send = [&](const AudioMan::NetworkSoundData& event, bool essential = false) {
		const auto path = event.State == AudioMan::SOUND_PLAY ? ContentFile::GetPathFromHash(event.SoundFileHash) : "";
		if (event.State == AudioMan::SOUND_PLAY && (!AssetPath(path) || (peer.AudioChannels.size() >= 512 && !peer.AudioChannels.contains(event.Channel)))) return false;
		MP::Writer writer(Kind::Sound, Session, Epoch); writer.U8(event.State); writer.U32(static_cast<uint32_t>(event.Channel)); writer.Text(path, 240); writer.U8(event.Immobile); writer.U8(event.AffectedByGlobalPitch);
		writer.U32(static_cast<uint32_t>(event.Loops)); writer.U32(static_cast<uint32_t>(event.Priority)); writer.U32(static_cast<uint32_t>(event.FadeOutTime));
		writer.F32(event.AttenuationStartDistance); writer.F32(event.CustomPanValue); writer.F32(event.PanningStrengthMultiplier); writer.F32(event.Position[0]); writer.F32(event.Position[1]); writer.F32(event.Volume); writer.F32(event.Pitch);
		const double cost = writer.Data.size() + 80;
		if (!essential && (peer.AudioBudget < cost || Net.QueuedBytes(peer.Address) > 16 * 1024)) return false;
		if (!Net.Send(peer.Address, writer.Data, Delivery::Audio)) return false;
		peer.AudioBudget -= cost;
		if (event.State == AudioMan::SOUND_PLAY) peer.AudioChannels.insert(event.Channel);
		if (event.State == AudioMan::SOUND_SET_GLOBAL_PITCH) peer.SentGlobalPitch = event.Pitch;
		return true;
	};
	auto clearChanges = [&](int channel) { std::erase_if(peer.PendingSoundChanges, [&](const auto& entry) { return entry.first.first == channel && entry.first.second != AudioMan::SOUND_SET_GLOBAL_PITCH; }); };
	for (const auto* event: ordered) {
		if (!event) continue;
		if (event->State == AudioMan::SOUND_PLAY) {
			clearChanges(event->Channel);
			if (event->Loops != 0 && peer.ReplayLoops && !peer.AudioChannels.contains(event->Channel)) continue;
			if (!send(*event)) {
				// A reused host channel must not leave an old guest loop playing
				// when its replacement one-shot is omitted under congestion.
				if (peer.AudioChannels.contains(event->Channel)) { auto stop = *event; stop.State = AudioMan::SOUND_STOP; send(stop, true); peer.AudioChannels.erase(event->Channel); }
				if (event->Loops != 0) peer.ReplayLoops = true;
			}
		} else if (event->State == AudioMan::SOUND_STOP || event->State == AudioMan::SOUND_FADE_OUT) {
			clearChanges(event->Channel);
			if (peer.AudioChannels.contains(event->Channel)) { send(*event, true); if (event->State == AudioMan::SOUND_STOP) peer.AudioChannels.erase(event->Channel); }
		} else if (event->State == AudioMan::SOUND_SET_GLOBAL_PITCH) {
			if (event->Pitch != peer.SentGlobalPitch) peer.PendingSoundChanges[{0, event->State}] = *event;
			else peer.PendingSoundChanges.erase({0, event->State});
		} else if (peer.AudioChannels.contains(event->Channel)) {
			// Coalesce across simulation/render ticks. Sending every positional
			// sound change reliably used to fill the link and stop world updates.
			peer.PendingSoundChanges[{event->Channel, event->State}] = *event;
		}
	}
	if (peer.ReplayLoops) {
		peer.ReplayLoops = false;
		for (const auto& [channel, loop]: peer.Loops) if (!peer.AudioChannels.contains(channel) && !send(loop)) peer.ReplayLoops = true;
		AudioMan::NetworkSoundData pitch{}; pitch.State = AudioMan::SOUND_SET_GLOBAL_PITCH; pitch.Pitch = g_AudioMan.GetGlobalPitch();
		if (pitch.Pitch != peer.SentGlobalPitch) peer.PendingSoundChanges[{0, pitch.State}] = pitch;
	}
	if (now - peer.LastAudioChanges >= 50) {
		peer.LastAudioChanges = now;
		// Resume after the last transmitted property, so busy low-numbered
		// channels cannot starve the remaining sounds on a slower connection.
		for (size_t remaining = peer.PendingSoundChanges.size(); remaining && !peer.PendingSoundChanges.empty(); --remaining) {
			auto it = peer.PendingSoundChanges.upper_bound(peer.AudioCursor);
			if (it == peer.PendingSoundChanges.end()) it = peer.PendingSoundChanges.begin();
			if (!send(it->second)) break;
			peer.AudioCursor = it->first; peer.PendingSoundChanges.erase(it);
		}
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
	if (!Admin() || Playing) return "";
	if (Online && !Dedicated && !Hosted && !Net.IsRelayReady()) return "Connecting to the room server...";
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
	if (!count) return "Invite a player before starting the match.";
	if (count > activity->GetMaxPlayerSupport()) return "This activity supports up to " + std::to_string(activity->GetMaxPlayerSupport()) + " players.";
	if (const int cpu = activity->GetCPUTeam(); cpu >= 0 && cpu < 4) teams[cpu] = true;
	if (std::count(teams.begin(), teams.end(), true) < activity->GetMinTeamsRequired()) return "Choose at least " + std::to_string(activity->GetMinTeamsRequired()) + " different teams for this activity.";
	return "";
}

MultiplayerMenuGUI::View MultiplayerMan::Impl::MenuView() const {
	MultiplayerMenuGUI::View view;
	view.Host = Admin(); view.Hosted = (Online && Hosted) || Dedicated; view.OwnerSlot = OwnerSlot; view.Online = Online; view.LocalSlot = LocalSlot; view.Played = Played;
	view.Page = State == Mode::Idle ? MultiplayerMenuGUI::Screen::Entry : CloseStarted || State == Mode::Connecting || State == Mode::Reconnecting ? MultiplayerMenuGUI::Screen::Connecting : Playing ? UI ? MultiplayerMenuGUI::Screen::Session : MultiplayerMenuGUI::Screen::Loading : MultiplayerMenuGUI::Screen::Lobby;
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
	view.Subtitle = State == Mode::Idle ? "Gather your group. Choose a battlefield. Stay together between rounds." : State == Mode::Reconnecting ? "Connection interrupted - restoring your original player slot." : State == Mode::Connecting ? (Hosted ? "Connecting to your AWS match server..." : "Joining your host...") : RoomName + (view.Host ? (Hosted ? "  /  Room owner - AWS server" : "  /  You are the host") : std::string(Hosted ? "  /  Server " : Online ? "  /  Relay " : "  /  Host ") + std::to_string(Net.Ping(ServerAddress)) + " ms");
	if (State == Mode::Client && Playing) view.Subtitle += "  /  " + std::to_string(static_cast<int>(FPS)) + " fps";
	if (CloseStarted) { view.Subtitle = "Closing your room..."; view.Notice = "Waiting for the match server to confirm."; }
	if (State == Mode::Client && Playing) view.NetworkStatus = "World updates: " + std::to_string(RecentWorldUpdates.size()) + "/s   Last update: " + (LastFrameReceived ? std::to_string(Now() - LastFrameReceived) + " ms ago" : "waiting");
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
	if (State == Mode::Client && !Admin()) {
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
	if (State == Mode::Client && !Playing) {
		auto index = [](const std::vector<std::string>& items, const std::string& name, int& target) { if (const auto found = std::find(items.begin(), items.end(), name); found != items.end()) target = int(found - items.begin()); };
		auto number = [](const std::string& text, int& target) { int value; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value); if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()) target = value; };
		for (const auto& [control, edit] : PendingEdits) {
			if (control == "Activity") index(view.Activities, edit.Text, view.ActivityIndex);
			else if (control == "Scene") index(view.Scenes, edit.Text, view.SceneIndex);
			else if (control == "Gold") number(edit.Text, view.Gold);
			else if (control == "Difficulty") number(edit.Text, view.Difficulty);
			else if (control == "Fog") view.Fog = edit.Value != 0;
			else if (control == "Deploy") view.Deploy = edit.Value != 0;
			else if (control == "Orbit") view.ClearOrbit = edit.Value != 0;
			else if (control.starts_with("Faction") && control.size() == 8 && edit.Value >= 0 && edit.Value < int(view.Factions.size())) { const int team = control.back() - '0'; if (team >= 0 && team < 4) view.Tech[team] = view.Factions[edit.Value]; }
			else if (control.starts_with("Team") && control.size() == 5 && edit.Value >= 0 && edit.Value < 4) { const int slot = control.back() - '0'; if (slot >= 0 && slot < 4) { view.Players[slot].Team = edit.Value; view.Players[slot].TeamName = view.TeamNames[edit.Value]; } }
			else if (control == "Ready") { view.Ready = edit.Value != 0; auto& slot = view.Players[LocalSlot]; slot.Ready = view.Ready; if (slot.Occupied && slot.Connected) slot.Status = view.Ready ? "READY" : "Not ready"; }
		}
	}
	return view;
}

void MultiplayerMan::Impl::UpdateMenu() {
	if (Dedicated) return;
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
		else if (control == "Bandwidth") { number(BandwidthMbps, 1, 48); if (Admin() && State == Mode::Client) SendControl(control, BandwidthMbps); }
		else if (control == "Port") number(Port, 1, 65535);
		else if (control == "Quality") Quality = std::clamp(event.Value, 0, 1);
		else if (control == "Online") { Online = event.Value != 2; Hosted = event.Value == 0; Error.clear(); }
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
		else if (control == "Primary") { CommitDraft(); if (State == Mode::Host) StartGame(); else if (Admin()) SendControl("Start"); else if (State == Mode::Client && !Playing) SendReady(!view.Ready, uint8_t(view.Players[LocalSlot].Team)); }
		else if (control == "Resume") { UI = false; Menu->Hide(); g_UInputMan.TrapMousePos(true); }
		else if (control == "Session") { UI = true; g_UInputMan.TrapMousePos(false); }
		else if (control == "Return") { if (State == Mode::Host) ReturnToLobby(); else if (Admin()) SendControl("Return"); }
		else if (control == "ConfirmCloseRoom" && Admin()) { SendControl("Close"); CloseStarted = Now(); CloseRetry = CloseStarted + 1000; UI = true; }
		else if (control == "ConfirmLeave") {
			if (Hosted && Online && State == Mode::Client && ReconnectToken) ResumeSlots[ResumeKey()] = {ReconnectToken, Now() + 60000, Session, Epoch, LocalInput};
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
			else if (Admin()) SendControl(control, team);
			else if (slot == LocalSlot) SendReady(false, team);
		} else if (Admin() && !Playing && (control == "Gold" || control == "Difficulty")) {
			if (Draft.Control != control) CommitDraft();
			Draft = {control, event.Text, Now()}; if (event.Value) CommitDraft();
		} else if (Admin() && !Playing) {
			if (State != Mode::Host) { std::string selection = event.Text;
				if (control == "Activity" && event.Value >= 0 && event.Value < view.Activities.size()) selection = view.Activities[event.Value];
				if (control == "Scene" && event.Value >= 0 && event.Value < view.Scenes.size()) selection = view.Scenes[event.Value];
				SendControl(control, event.Value, selection); continue; }
			bool changed = false;
			if (control == "Activity" && event.Value >= 0 && event.Value < Activities.size()) { ActivityIndex = event.Value; LoadScenes(); changed = true; }
			else if (control == "Scene" && event.Value >= 0 && event.Value < Scenes.size()) { SceneIndex = event.Value; SceneName = Scenes[SceneIndex]->GetPresetName(); changed = true; }
			else if (control == "Fog") { Fog = event.Value != 0; changed = true; }
			else if (control == "Deploy") { Deploy = event.Value != 0; changed = true; }
			else if (control == "Orbit") { ClearOrbit = event.Value != 0; changed = true; }
			else if (control.starts_with("Faction") && control.size() == 8 && event.Value >= 0 && event.Value < Factions.size()) { const int team = control.back() - '0'; if (team >= 0 && team < 4) { Tech[team] = Factions[event.Value]; changed = true; } }
			else if (control.starts_with("Release") && control.size() == 8) { const int slot = control.back() - '0'; if (slot > 0 && slot < 4 && !Players[slot].Connected) { Net.Close(Players[slot].Address); Players[slot] = Player(); changed = true; } }
			if (changed) { Error.clear(); Notice = "Match settings changed. Everyone must ready up again."; SettingsChanged(); }
		}
	}
	if (!Draft.Control.empty() && Now() - Draft.Changed >= 700) CommitDraft();
}

void MultiplayerMan::Impl::CommitDraft() {
	if (Draft.Control.empty()) return;
	const auto draft = std::exchange(Draft, {});
	if (!Admin() || Playing) return;
	if (State != Mode::Host) { SendControl(draft.Control, 0, draft.Text); return; }
	int value; const auto parsed = std::from_chars(draft.Text.data(), draft.Text.data() + draft.Text.size(), value);
	if (parsed.ec != std::errc() || parsed.ptr != draft.Text.data() + draft.Text.size()) return;
	int& target = draft.Control == "Gold" ? Gold : Difficulty; const int old = target;
	target = std::clamp(value, 0, draft.Control == "Gold" ? 1000000 : 100);
	if (target != old) { Error.clear(); Notice = "Match settings changed. Everyone must ready up again."; SettingsChanged(); }
}

void MultiplayerMan::Impl::Draw() {
	if (Dedicated) return;
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
		LocalSlot = State == Mode::Client ? 1 : 0; Hosted = test.Hosted || State == Mode::Idle; OwnerSlot = test.Owner ? LocalSlot : 0; Dedicated = false;
		if (CursorPreparedStage != CursorStage) { CursorFrames = 0; if (CursorStage == 0) g_MenuMan.SetMultiplayerMenuBackground(true); }
		if (Activities.empty()) LoadActivities();
		RoomName = "Multiplayer lobby"; Players[0].Name = "Host"; Players[0].Token = 1; Players[0].Connected = Players[0].Ready = true;
	}
	ImGui::GetIO().MouseDrawCursor = false;
	if (State == Mode::Client && Playing && World.Ready()) {
		World.SetLocalInput(LocalInput, !UI && Controls && !World.Paused());
		Texture = World.Render(Now()); TextureWidth = World.Width(); TextureHeight = World.Height();
		// Spatial audio follows the camera actually rendered this frame, including
		// local free flight and aim look-ahead, not the last received host camera.
		if (Vector center; World.RenderedCenter(center)) g_AudioMan.SetStreamListener(center, true);
		const uint64_t now = Now();
		BenchmarkRecord("render", now, LastRender && now >= LastRender ? now - LastRender : 0);
		if (LastRender && now > LastRender) FPS = FPS * 0.9f + 100.0f / float(now - LastRender); LastRender = now;
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
