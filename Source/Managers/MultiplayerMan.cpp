#include "MultiplayerMan.h"

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
#include <future>
#include <random>
#include <unordered_map>
#include <fstream>
#include <SDL3_image/SDL_image.h>
#include "System.h"
#include "DataModule.h"
#include "Actor.h"

using namespace RTE;
using namespace RTE::MP;
namespace {
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
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
	struct Encoded { FrameInfo Info; std::vector<uint8_t> Bytes; };
	struct Player {
		std::string Address, Name;
		uint64_t Token = 0, ReservedUntil = 0, LastFrame = 0, LastAck = 0, LastChat = 0;
		bool Connected = false, Ready = false;
		bool AudioReady = false, ReplayLoops = true;
		bool TextActive = false, SmokeTextSeen = false;
		std::string Text;
		std::unordered_map<int, AudioMan::NetworkSoundData> Loops;
		uint8_t Team = 0;
		InputReceiver Inputs;
		std::future<Encoded> Encoding;
		std::vector<std::vector<uint8_t>> Outgoing;
		size_t NextPacket = 0;
		uint32_t FrameID = 0, AckID = 0;
		double Budget = 0;
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
	bool TextActive = false;
	std::string ServerAddress, Error, RoomName, ActivityName, SceneName;
	std::vector<std::string> Chat;
	char ChatText[193] = "";
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
	FrameAssembler Frames;
	unsigned Texture = 0;
	int TextureWidth = 0, TextureHeight = 0;
	uint64_t BytesReceived = 0;
	float FPS = 0;
	bool Smoke = false, SmokeCapture = false;
	bool CursorVerification = false;
	size_t CursorStage = 0;
	int CursorFrames = 0;
	struct CursorCase { const char* Name; Mode State; bool UI, Playing, Visible; };
	static constexpr std::array<CursorCase, 12> CursorCases{{
		{"menu", Mode::Idle, true, false, true}, {"main-menu", Mode::Idle, false, false, false},
		{"host-lobby", Mode::Host, true, false, true}, {"host-gameplay", Mode::Host, false, true, false},
		{"host-session", Mode::Host, true, true, true}, {"host-resume", Mode::Host, false, true, false},
		{"guest-lobby", Mode::Client, true, false, true}, {"guest-gameplay", Mode::Client, false, true, false},
		{"guest-session", Mode::Client, true, true, true}, {"connecting", Mode::Connecting, false, false, true},
		{"reconnecting", Mode::Reconnecting, false, false, true}, {"closed-menu", Mode::Idle, false, false, false}
	}};
	bool SmokeReconnected = false, SmokeCapturedHost = false;
	int SmokeStage = 0;
	int SmokeRejoins = 0;
	int SmokeGuests = 1;
	std::string SmokeRole;
	int SmokeCaptureDelay = 0;
	uint64_t SmokeStarted = 0, SmokeStageTime = 0, Presented = 0, AudioReceived = 0;
	uint64_t SmokeSecondStart = 0;
	uint64_t SmokeLastStats = 0;
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

	void ClearSounds() { for (auto& [channel, sound]: Sounds) sound->Stop(); Sounds.clear(); }
	void Stop() {
		if (TextActive && State != Mode::Host) SDL_StopTextInput(g_WindowMan.GetWindow()); TextActive = false;
		if (SmokeLoop) { SmokeLoop->Stop(); SmokeLoop.reset(); }
		if (State == Mode::Host) { MP::Writer leave(Kind::Leave, Session, Epoch); for (size_t i = 1; i < Players.size(); ++i) if (Players[i].Connected) Net.Send(Players[i].Address, leave.Data, Delivery::Control); }
		if (State == Mode::Client || State == Mode::Connecting || State == Mode::Reconnecting) { MP::Writer leave(Kind::Leave, Session, Epoch); Net.Send(ServerAddress, leave.Data, Delivery::Control); }
		Net.Stop();
		for (auto& player: Players) { if (player.Encoding.valid()) player.Encoding.wait(); player = Player(); }
		Pending.clear(); Chat.clear(); ClearSounds(); Frames.Reset(); LocalInput = {}; Session = ReconnectToken = 0; Epoch = 0; MouseRemainderX = MouseRemainderY = 0;
		if (Texture) { glDeleteTextures(1, &Texture); Texture = 0; TextureWidth = TextureHeight = 0; }
		State = Mode::Idle; Playing = Launch = false; g_AudioMan.SetMultiplayerMode(false); g_UInputMan.TrapMousePos(false);
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
		ServerAddress = address; MP::Writer writer(Kind::Hello); writer.Text(Name[0] ? Name : "Player", 31); writer.U64(ReconnectToken); writer.Text(c_GameVersion.str()); Net.Send(address, writer.Data, Delivery::Control);
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
		if (Online && !Net.IsRelayReady()) { Error = "Wait for the room service to connect before starting."; return false; }
		if (Activities.empty() || Scenes.empty()) { Error = "Select an activity with a compatible scene."; return false; }
		for (const auto& player: Players) if (player.Token && !player.Connected) { Error = "Wait for disconnected players to rejoin or release their slots before starting a new match."; return false; }
		int count = 0; for (const auto& player: Players) { if (player.Connected) { if (!player.Ready) { Error = "Every connected player must be ready."; return false; } ++count; } }
		if (count > Activities[ActivityIndex]->GetMaxPlayerSupport()) { Error = "This activity does not support this many players."; return false; }
		std::array<bool, 4> teams{}; for (const auto& player: Players) if (player.Connected) teams[player.Team] = true;
		if (const int cpu = Activities[ActivityIndex]->GetCPUTeam(); cpu >= 0 && cpu < 4) teams[cpu] = true;
		const int teamCount = static_cast<int>(std::count(teams.begin(), teams.end(), true));
		if (teamCount < Activities[ActivityIndex]->GetMinTeamsRequired()) { Error = "Choose enough different player teams for this activity."; return false; }
		auto* game = dynamic_cast<GameActivity*>(Activities[ActivityIndex]->Clone()); game->ClearPlayers(false);
		if (game->GetCPUTeam() >= 0) game->SetCPUTeam(game->GetCPUTeam());
		for (int i = 0; i < 4; ++i) if (Players[i].Connected) game->AddPlayer(i, true, Players[i].Team, 0);
		game->SetDifficulty(Difficulty); game->SetStartingGold(Gold); game->SetFogOfWarEnabled(Fog); game->SetRequireClearPathToOrbit(ClearOrbit);
		for (int team = 0; team < 4; ++team) game->SetTeamTech(team, Tech[team]);
		if (game->GetCPUTeam() >= 0) { const int cpuTeam = game->GetCPUTeam(); for (const auto& player: Players) if (player.Connected && player.Team == cpuTeam) { delete game; Error = "That team belongs to the AI. Choose another team."; return false; } }
		g_SceneMan.SetSceneToLoad(Scenes[SceneIndex], true, Deploy); g_LuaMan.FileCloseAll(); g_ActivityMan.SetStartActivity(game); g_ActivityMan.SetRestartActivity();
		++Epoch; Playing = Launch = true; UI = false; Error.clear();
		for (auto& player: Players) { if (player.Encoding.valid()) player.Encoding.get(); player.Inputs.Reset(); player.Outgoing.clear(); player.NextPacket = 0; player.LastFrame = player.LastAck = Now(); player.Loops.clear(); player.AudioReady = false; player.ReplayLoops = true; player.Text.clear(); player.TextActive = false; }
		g_AudioMan.ClearSoundEvents(-1); g_AudioMan.SetMultiplayerMode(true); Lobby(); return true;
	}
	void ReturnToLobby() {
		Playing = false; UI = true; ++Epoch; g_AudioMan.SetMultiplayerMode(false); g_AudioMan.ClearSoundEvents(-1); g_ActivityMan.PauseActivity();
		for (size_t i = 1; i < Players.size(); ++i) { Players[i].Ready = false; Players[i].Inputs.Reset(); Players[i].Outgoing.clear(); g_UInputMan.ClearRemoteInput(static_cast<int>(i)); }
		Lobby();
	}
	void Receive(const TransportEvent& event);
	void HandleAudio(MP::Reader& reader);
	void SendAudio(int player);
	void Tick();
	void SampleInput();
	void Present(const FrameAssembler::Complete& frame);
	void QueueFrame(Player& player, Encoded frame);
	void Draw();
};

MultiplayerMan::MultiplayerMan(): m_Impl(std::make_unique<Impl>()) {}
MultiplayerMan::~MultiplayerMan() = default;
void MultiplayerMan::Open() { m_Impl->UI = true; g_UInputMan.TrapMousePos(false); }
void MultiplayerMan::Update() { m_Impl->Tick(); }
void MultiplayerMan::DrawUI() { m_Impl->Draw(); }
void MultiplayerMan::Stop() { m_Impl->Stop(); }
bool MultiplayerMan::StartRoom(bool host, const std::string& address, bool smokeTest) {
	auto& impl = *m_Impl; impl.UI = true; impl.Smoke = smokeTest;
	impl.Online = std::getenv("CCCP_MP_SERVICE") != nullptr;
	if (!host && impl.Online) std::snprintf(impl.JoinCode, sizeof(impl.JoinCode), "%s", address.c_str());
	if (smokeTest) {
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
		if (host && started) { for (int i = 0; i < impl.Activities.size(); ++i) if (impl.Activities[i]->GetPresetName() == "Test Activity") { impl.ActivityIndex = i; impl.LoadScenes(); break; } impl.Lobby(); }
	}
	return started;
}
void MultiplayerMan::CaptureVerificationFrame() {
	auto& impl = *m_Impl;
	if (impl.CursorVerification) {
		if (++impl.CursorFrames < 3) return;
		const auto& test = Impl::CursorCases[impl.CursorStage];
		const bool drawn = ImGui::GetForegroundDrawList()->VtxBuffer.Size > 0;
		std::ofstream log("build-mp/cursor-smoke.log", std::ios::app);
		log << test.Name << ": expected=" << test.Visible << " software=" << ImGui::GetIO().MouseDrawCursor << " rendered=" << drawn << '\n';
		impl.SmokeCapture = true; impl.SmokeCaptureDelay = 2;
		if (drawn != test.Visible || ImGui::GetIO().MouseDrawCursor != test.Visible) { log << "FAIL: cursor visibility in " << test.Name << '\n'; System::SetQuit(); }
	}
	if (!impl.SmokeCapture) return; if (++impl.SmokeCaptureDelay < 3) return; impl.SmokeCapture = false; impl.SmokeCaptureDelay = 0;
	int viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport); int alignment; glGetIntegerv(GL_PACK_ALIGNMENT, &alignment); glPixelStorei(GL_PACK_ALIGNMENT, 1);
	std::vector<uint8_t> pixels(size_t(viewport[2]) * viewport[3] * 3); glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGB, GL_UNSIGNED_BYTE, pixels.data()); glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	const size_t pitch = size_t(viewport[2]) * 3; for (int row = 0; row < viewport[3] / 2; ++row) std::swap_ranges(pixels.begin() + row * pitch, pixels.begin() + (row + 1) * pitch, pixels.begin() + (viewport[3] - row - 1) * pitch);
	SDL_Surface* surface = SDL_CreateSurfaceFrom(viewport[2], viewport[3], SDL_PIXELFORMAT_RGB24, pixels.data(), static_cast<int>(pitch));
	const auto path = impl.CursorVerification ? std::string("build-mp/cursor-") + Impl::CursorCases[impl.CursorStage].Name + ".png" : std::string("build-mp/") + impl.SmokeRole + "-stage" + std::to_string(impl.SmokeStage) + ".png";
	if (surface) { impl.Verify(IMG_SavePNG(surface, path.c_str()) ? "SCREENSHOT: " + path : "FAIL: screenshot"); SDL_DestroySurface(surface); }
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
int MultiplayerMan::ViewWidth(int screen) const { const auto* activity = g_ActivityMan.GetActivity(); return IsHostingMatch() && screen > 0 && activity ? (m_Impl->Quality == 0 ? 640 : 960) : g_WindowMan.GetResX(); }
int MultiplayerMan::ViewHeight(int screen) const { return IsHostingMatch() && screen > 0 ? (m_Impl->Quality == 0 ? 360 : 540) : g_WindowMan.GetResY(); }
bool MultiplayerMan::WantsFrame(int player) const {
	if (!IsRemotePlayer(player)) return false; const auto& peer = m_Impl->Players[player];
	if (uint32_t(peer.FrameID - peer.AckID) >= 2 && Now() - peer.LastFrame < FrameTimeoutMS) return false;
	return peer.Connected && !peer.Encoding.valid() && peer.Outgoing.empty() && Now() - peer.LastFrame >= (Now() - peer.LastAck > 2000 ? 66 : 33) && m_Impl->Net.QueuedBytes(peer.Address) < 32 * 1024;
}
void MultiplayerMan::CaptureFrame(int player, unsigned framebuffer, int width, int height, float cameraX, float cameraY) {
	if (!WantsFrame(player) || width < 320 || width > MaxWidth || height < 180 || height > MaxHeight) return;
	auto& peer = m_Impl->Players[player]; peer.LastFrame = Now();
	FrameInfo info; info.ID = ++peer.FrameID; info.InputSequence = peer.Inputs.LastSequence(); info.Width = static_cast<uint16_t>(width); info.Height = static_cast<uint16_t>(height); info.CameraX = cameraX; info.CameraY = cameraY;
	std::vector<uint8_t> pixels(size_t(width) * height * 2);
	int previousRead = 0, previousPack = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead); glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead); glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
	if constexpr (std::endian::native == std::endian::big) for (size_t i = 0; i < pixels.size(); i += 2) std::swap(pixels[i], pixels[i + 1]);
	peer.Encoding = std::async(std::launch::async, [info, pixels = std::move(pixels)]() mutable {
		Impl::Encoded result; result.Info = info; result.Bytes.resize(LZ4_compressBound(static_cast<int>(pixels.size())));
		const int size = LZ4_compress_default(reinterpret_cast<const char*>(pixels.data()), reinterpret_cast<char*>(result.Bytes.data()), static_cast<int>(pixels.size()), static_cast<int>(result.Bytes.size()));
		result.Bytes.resize(std::max(0, size)); result.Info.Bytes = static_cast<uint32_t>(result.Bytes.size()); return result;
	});
}

void MultiplayerMan::Impl::Receive(const TransportEvent& event) {
	MP::Reader reader(event.Data); Header header;
	if (!ReadHeader(reader, header)) { if (State == Mode::Host && Pending.contains(event.Address)) Reject(event.Address, "Incompatible multiplayer protocol. Install the same game build as the host."); return; }
	if (State == Mode::Host) {
		if (header.Type == Kind::Hello && Pending.contains(event.Address)) {
			std::string name, version; uint64_t token;
			if (!reader.Text(name, 31) || name.empty() || !reader.U64(token) || !reader.Text(version) || !reader.Done()) { Reject(event.Address, "Invalid player registration."); return; }
			if (version != c_GameVersion.str()) { Reject(event.Address, "Game version differs from the host. Install the same build."); return; }
			int slot = -1;
			if (token) for (int i = 1; i < 4; ++i) if (Players[i].Token == token && !Players[i].Connected && Players[i].ReservedUntil > Now()) slot = i;
			if (slot < 0 && !Playing) for (int i = 1; i < 4; ++i) if (!Players[i].Token) { slot = i; break; }
			if (slot < 0) { Reject(event.Address, Playing ? "Match already started. Rejoin with your original player slot or wait for the lobby." : "All player slots are occupied or reserved for reconnecting players."); return; }
			if (Smoke && token && Players[slot].Token == token) { ++SmokeRejoins; Verify("REJOIN: player " + std::to_string(slot + 1)); }
			auto& player = Players[slot]; player.Address = event.Address; player.Name = name; player.Connected = true; player.Ready = false; player.ReservedUntil = 0;
			if (!player.Token) player.Token = Token();
			player.Inputs.Reset(); player.Outgoing.clear(); player.NextPacket = 0; player.LastAck = Now(); player.AudioReady = false; player.ReplayLoops = true; Pending.erase(event.Address); Welcome(slot); return;
		}
		int slot = -1; for (int i = 1; i < 4; ++i) if (Players[i].Connected && Players[i].Address == event.Address) slot = i;
		if (slot < 0 || header.Session != Session || (header.Epoch != Epoch && header.Type != Kind::Chat)) return;
		auto& player = Players[slot];
		if (header.Type == Kind::Input && Playing) { Input input; if (ReadInput(reader, input)) player.Inputs.Push(input, Now()); }
		else if (header.Type == Kind::Ready && !Playing) { uint8_t ready, team; if (reader.U8(ready) && ready <= 1 && reader.U8(team) && team < 4 && (AvailableTeams & (1 << team)) && reader.Done()) { player.Ready = ready != 0; player.Team = team; Lobby(); } }
		else if (header.Type == Kind::Chat) { std::string message; if (reader.Text(message, 192) && !message.empty() && reader.Done() && Now() - player.LastChat > 250) { player.LastChat = Now(); AddChat(slot, message); } }
		else if (header.Type == Kind::FrameAck) { uint32_t frame; if (reader.U32(frame) && reader.Done() && !Newer(frame, player.FrameID) && Newer(frame, player.AckID)) { player.AckID = frame; player.LastAck = Now(); if (!player.AudioReady) g_MultiplayerMan.SetTextInputActive(slot, player.TextActive); player.AudioReady = true; } }
		else if (header.Type == Kind::TextInput && Playing) { uint8_t direction; std::string text; if (reader.U8(direction) && direction == 0 && reader.Text(text, 64) && reader.Done() && player.TextActive && player.Text.size() + text.size() <= 256) { player.Text += text; if (Smoke && text == "Shop text test") { player.SmokeTextSeen = true; Verify("GUI TEXT: player " + std::to_string(slot + 1)); } } }
		else if (header.Type == Kind::Leave && reader.Done()) { Net.Close(event.Address); player.Connected = false; player.Ready = false; player.ReservedUntil = Playing ? Now() + 60000 : 0; player.Inputs.Reset(); g_UInputMan.ClearRemoteInput(slot); Lobby(); }
		return;
	}
	if (event.Address != ServerAddress) return;
	if (header.Type == Kind::Reject && (State == Mode::Connecting || State == Mode::Reconnecting)) { std::string message; if (reader.Text(message) && reader.Done()) { Error = message; Stop(); UI = true; } return; }
	if (header.Type == Kind::Welcome && (State == Mode::Connecting || State == Mode::Reconnecting)) {
		uint8_t slot; uint64_t token; std::string version;
		if (!header.Session || !reader.U8(slot) || slot < 1 || slot > 3 || !reader.U64(token) || !token || !reader.Text(version) || version != c_GameVersion.str() || !reader.Done()) return;
		Session = header.Session; Epoch = header.Epoch; LocalSlot = slot; ReconnectToken = token; State = Mode::Client; LocalInput = {}; Frames.Reset(); ClearSounds(); UI = !Playing; Error.clear(); LastFrameReceived = Now(); return;
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
		if (Epoch != header.Epoch) { Epoch = header.Epoch; Frames.Reset(); LocalInput = {}; ClearSounds(); TextActive = false; LastFrameReceived = Now(); if (Texture) { glDeleteTextures(1, &Texture); Texture = 0; TextureWidth = TextureHeight = 0; } }
		Playing = playing != 0; RoomName = room; ActivityName = activity; SceneName = scene; if (!Playing || changedMatch) UI = !Playing;
		Difficulty = difficulty; Gold = static_cast<int>(gold); Fog = fog; Deploy = deploy; ClearOrbit = clearOrbit; AvailableTeams = teams; CPUTeam = cpu; Tech = std::move(tech);
		if (!Playing) g_AudioMan.SetStreamListener(Vector(), false);
		if (Smoke) Verify("LOBBY UPDATE: epoch=" + std::to_string(Epoch) + " playing=" + std::to_string(Playing));
		for (size_t i = 0; i < slots.size(); ++i) { Players[i].Token = slots[i].Occupied; Players[i].Connected = slots[i].Connected; Players[i].Ready = slots[i].Ready; Players[i].Team = slots[i].Team; Players[i].Name = slots[i].Name; }
		g_UInputMan.TrapMousePos(Playing && !UI); return;
	}
	if (header.Epoch != Epoch) return;
	if (header.Type == Kind::TextInput && Playing) { uint8_t direction, active; if (reader.U8(direction) && direction == 1 && reader.U8(active) && active <= 1 && reader.Done()) TextActive = active != 0; return; }
	if (header.Type == Kind::Frame && Playing) {
		FrameChunk chunk; if (!ReadChunk(reader, chunk)) return;
		if (Smoke && chunk.Info.ID <= 2 && chunk.Index == 0) Verify("FRAME: " + std::to_string(chunk.Info.ID) + " bytes=" + std::to_string(chunk.Info.Bytes));
		if (auto frame = Frames.Push(chunk, Now())) { Present(*frame); }
	} else if (header.Type == Kind::Sound && Playing) HandleAudio(reader);
}

void MultiplayerMan::Impl::Present(const FrameAssembler::Complete& frame) {
	const size_t size = size_t(frame.Info.Width) * frame.Info.Height * 2; std::vector<uint8_t> pixels(size);
	const int decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(frame.Bytes.data()), reinterpret_cast<char*>(pixels.data()), static_cast<int>(frame.Bytes.size()), static_cast<int>(pixels.size()));
	if (decoded != static_cast<int>(pixels.size())) { Verify("DECODE FAILURE: " + std::to_string(decoded) + " expected=" + std::to_string(size)); return; }
	std::vector<uint8_t> rgba(size * 2);
	for (size_t i = 0; i < size / 2; ++i) { const uint16_t color = uint16_t(pixels[i * 2]) | uint16_t(pixels[i * 2 + 1]) << 8; rgba[i * 4] = ((color >> 11) & 31) * 255 / 31; rgba[i * 4 + 1] = ((color >> 5) & 63) * 255 / 63; rgba[i * 4 + 2] = (color & 31) * 255 / 31; rgba[i * 4 + 3] = 255; }
	int previousTexture = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	if (!Texture) glGenTextures(1, &Texture);
	glBindTexture(GL_TEXTURE_2D, Texture); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (TextureWidth != frame.Info.Width || TextureHeight != frame.Info.Height) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.Info.Width, frame.Info.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()); TextureWidth = frame.Info.Width; TextureHeight = frame.Info.Height; }
	else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.Info.Width, frame.Info.Height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindTexture(GL_TEXTURE_2D, previousTexture);
	const uint64_t now = Now(); if (now > LastFrameReceived) FPS = FPS * 0.9f + 100.0f / static_cast<float>(now - LastFrameReceived); LastFrameReceived = now; BytesReceived += frame.Bytes.size();
	g_AudioMan.SetStreamListener(Vector(frame.Info.CameraX + frame.Info.Width / 2, frame.Info.CameraY + frame.Info.Height / 2), true);
	MP::Writer ack(Kind::FrameAck, Session, Epoch); ack.U32(frame.Info.ID); Net.Send(ServerAddress, ack.Data, Delivery::Input);
	++Presented;
}

void MultiplayerMan::Impl::QueueFrame(Player& player, Encoded frame) {
	if (!ValidFrame(frame.Info)) return;
	if (Smoke && player.FrameID < 5) Verify("ENCODED: " + std::to_string(frame.Info.Bytes));
	player.Outgoing.clear(); player.NextPacket = 0; const auto count = ChunkCount(frame.Info);
	for (uint16_t start = 0; start < count; start += ParityGroup) {
		std::array<uint8_t, ChunkBytes> parity{};
		for (uint16_t i = start; i < std::min<int>(count, start + ParityGroup); ++i) {
			const auto bytes = std::span(frame.Bytes).subspan(size_t(i) * ChunkBytes, ChunkSize(frame.Info, i));
			for (size_t j = 0; j < bytes.size(); ++j) parity[j] ^= bytes[j];
			MP::Writer writer(Kind::Frame, Session, Epoch); WriteChunk(writer, FrameChunk{frame.Info, i, false, bytes}); player.Outgoing.push_back(std::move(writer.Data));
		}
		MP::Writer writer(Kind::Frame, Session, Epoch); WriteChunk(writer, FrameChunk{frame.Info, static_cast<uint16_t>(start / ParityGroup), true, parity}); player.Outgoing.push_back(std::move(writer.Data));
	}
}

void MultiplayerMan::Impl::SampleInput() {
	static_assert(InputElements::INPUT_COUNT <= GUIKeyFirst && GUIKeyFirst + GUIKeys.size() <= InputCount);
	if (State != Mode::Client || !Playing) return;
	const bool enabled = !UI && Controls && g_WindowMan.AnyWindowHasFocus();
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
	if (Smoke) { LocalInput.Held |= (uint64_t(1) << InputElements::INPUT_L_RIGHT) | (uint64_t(1) << InputElements::INPUT_FIRE); LocalInput.MouseHeld = 1; LocalInput.AimX = 0.75f; LocalInput.MouseX += 2; }
	if (Smoke && TextActive && !SmokeTextSent) { SmokeTextSent = true; MP::Writer writer(Kind::TextInput, Session, Epoch); writer.U8(0); writer.Text("Shop text test", 64); Net.Send(ServerAddress, writer.Data, Delivery::Control); }
	if (Smoke && SmokeStage == 1 && TextActive) LocalInput.Held |= uint64_t(1) << (GUIKeyFirst + 1);
	if (Now() - LastInputSent >= 8) { ++LocalInput.Sequence; LastInputSent = Now(); MP::Writer writer(Kind::Input, Session, Epoch); WriteInput(writer, LocalInput); Net.Send(ServerAddress, writer.Data, Delivery::Input); }
}

void MultiplayerMan::ApplyInputs() {
	if (!IsHostingMatch()) return;
	for (int i = 1; i < 4; ++i) if (IsRemotePlayer(i)) g_UInputMan.SetRemoteInput(i, m_Impl->Players[i].Inputs.Consume(Now()));
}

void MultiplayerMan::Impl::Tick() {
	const uint64_t now = Now(); const double elapsed = std::min<uint64_t>(100, now - LastUpdate) / 1000.0; LastUpdate = now;
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
				for (int i = 1; i < 4; ++i) if (Players[i].Address == event.Address && Players[i].Connected) { Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.Reset(); Players[i].Outgoing.clear(); g_UInputMan.ClearRemoteInput(i); Lobby(); }
			} else if (State == Mode::Client && event.Address == ServerAddress) { State = Mode::Reconnecting; ConnectStarted = now; NextRetry = now + 1000; UI = true; Error = "Connection interrupted. Reconnecting to your player slot..."; g_UInputMan.TrapMousePos(false); ClearSounds(); }
		}
	}
	if (State == Mode::Connecting && now - ConnectStarted > 10000) { Error = "Connection timed out. Check the host address and UDP port."; Stop(); UI = true; }
	if (State == Mode::Reconnecting) { if (now - ConnectStarted > 20000) { Error = "Unable to reconnect. Your host keeps the slot for 60 seconds."; Stop(); UI = true; } else if (now >= NextRetry) Join(true); }
	if (State == Mode::Host) {
		for (auto it = Pending.begin(); it != Pending.end();) { if (now - it->second > 5000) { Net.Close(it->first); it = Pending.erase(it); } else ++it; }
		for (int i = 1; i < 4; ++i) {
			auto& player = Players[i];
			if (!Playing && player.Token && !player.Connected && now >= player.ReservedUntil) { Net.Close(player.Address); if (player.Encoding.valid()) player.Encoding.wait(); player = Player(); Lobby(); }
			if (player.Encoding.valid() && player.Encoding.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) { auto encoded = player.Encoding.get(); if (Playing && player.Connected) QueueFrame(player, std::move(encoded)); }
			if (!player.Outgoing.empty() && now - player.LastFrame > FrameTimeoutMS) { player.Outgoing.clear(); player.NextPacket = 0; }
			player.Budget = std::min(262144.0, player.Budget + elapsed * BandwidthMbps * 125000.0);
			while (player.NextPacket < player.Outgoing.size() && player.Budget >= player.Outgoing[player.NextPacket].size()) { const auto& bytes = player.Outgoing[player.NextPacket++]; player.Budget -= bytes.size(); Net.Send(player.Address, bytes, Delivery::Frame); }
			if (player.NextPacket == player.Outgoing.size()) { player.Outgoing.clear(); player.NextPacket = 0; }
			if (Playing && player.Token) SendAudio(i); else g_AudioMan.ClearSoundEvents(i);
		}
		g_AudioMan.ClearSoundEvents(0);
	}
	SampleInput();
	SmokeTick();
}

void MultiplayerMan::Impl::SmokeTick() {
	if (!Smoke) return; const auto now = Now();
	if (Playing && now - SmokeLastStats > 3000) { SmokeLastStats = now; if (State == Mode::Host) Verify("TRANSPORT: " + Net.Diagnostics(Players[1].Address)); else Verify("TRANSPORT: " + Net.Diagnostics(ServerAddress) + " buffered=" + std::to_string(Frames.BufferedChunks()) + " expired=" + std::to_string(Frames.DroppedFrames()) + " presented=" + std::to_string(Presented)); }
	if (now - SmokeStarted > 90000) { Verify("FAIL: smoke timed out: " + Error); System::SetQuit(true); return; }
	if (State == Mode::Host) {
		if (SmokeStage == 1 && !SmokeLoop && !SmokeLoopStopped && now - SmokeStageTime > 1500) { for (int player = 1; player <= SmokeGuests; ++player) g_MultiplayerMan.SetTextInputActive(player, true); SmokeLoop = std::make_unique<SoundContainer>(); SmokeLoop->GetTopLevelSoundSet().AddSound("Base.rte/Sounds/Craft/ThrusterLoop.flac", false); SmokeLoop->SetLoopSetting(-1); SmokeLoop->SetImmobile(true); SmokeLoop->SetVolume(0.013f); if (!SmokeLoop->Play()) Verify("FAIL: looping sound setup"); }
		if (SmokeStage == 1) for (int player = 1; player <= SmokeGuests; ++player) SmokeGUIKeySeen[player] = SmokeGUIKeySeen[player] || g_UInputMan.KeyHeld(SDL_SCANCODE_BACKSPACE, player);
		if (SmokeStage == 1 && SmokeLoop && now - SmokeStageTime > 11500) { SmokeLoop->Stop(); SmokeLoop.reset(); SmokeLoopStopped = true; }
		if (SmokeStage == 1 && !SmokeReconnected && now - SmokeStageTime > 5000 && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.AckID > 0; })) { SmokeReconnected = true; for (int i = 1; i <= SmokeGuests; ++i) { Net.Close(Players[i].Address); Players[i].Connected = Players[i].Ready = false; Players[i].ReservedUntil = now + 60000; Players[i].Inputs.Reset(); Players[i].Outgoing.clear(); } Lobby(); Verify("RECONNECT: interrupted guest connections"); }
		if (SmokeStage == 1 && !SmokeCapturedHost && now - SmokeStageTime > 9000) { SmokeCapturedHost = true; SmokeCapture = true; }
		if ((SmokeStage == 0 || SmokeStage == 2) && std::all_of(Players.begin() + 1, Players.begin() + 1 + SmokeGuests, [](const auto& player) { return player.Connected && player.Ready; })) { if (!StartGame()) { Verify("FAIL: " + Error); System::SetQuit(true); return; } ++SmokeStage; SmokeStageTime = now; Verify("MATCH: " + std::to_string(SmokeStage) + " " + ActivityName + " guests=" + std::to_string(SmokeGuests)); }
		if (SmokeStage == 1 && now - SmokeStageTime > 14000) { Verify("INPUT: " + std::to_string(Players[1].Inputs.LastSequence()) + " ACK: " + std::to_string(Players[1].AckID)); SmokeCapture = true; ReturnToLobby(); SmokeStage = 2; SmokeStageTime = now; Quality = 1; BandwidthMbps = 48; for (int i = 0; i < Activities.size(); ++i) if (Activities[i]->GetPresetName() == "One-Man Army") { ActivityIndex = i; LoadScenes(); break; } Lobby(); }
		if (SmokeStage == 3) for (int player = 1; player <= SmokeGuests; ++player) if (auto* actor = g_ActivityMan.GetActivity()->GetControlledActor(player)) { if (!SmokeActorSeen[player]) { SmokeActorSeen[player] = true; SmokeActorStartX[player] = actor->GetPos().GetX(); } SmokeActorMoved[player] = SmokeActorMoved[player] || std::abs(actor->GetPos().GetX() - SmokeActorStartX[player]) > 5; SmokeFireSeen[player] = SmokeFireSeen[player] || actor->GetController()->IsState(ControlState::WEAPON_FIRE); }
		if (SmokeStage == 3 && now - SmokeStageTime > 12000) { bool passed = SmokeRejoins == SmokeGuests; for (int player = 1; player <= SmokeGuests; ++player) { passed &= Players[player].AckID && Players[player].Inputs.LastSequence() && SmokeActorSeen[player] && SmokeActorMoved[player] && SmokeFireSeen[player] && SmokeGUIKeySeen[player] && Players[player].SmokeTextSeen; Verify("PLAYER: " + std::to_string(player + 1) + " moved=" + std::to_string(SmokeActorMoved[player]) + " fired=" + std::to_string(SmokeFireSeen[player]) + " GUI key=" + std::to_string(SmokeGUIKeySeen[player]) + " text=" + std::to_string(Players[player].SmokeTextSeen)); } SmokeCapture = true; Verify(passed ? "PASS: two matches, both stream sizes, remote movement and firing, GUI keys and text, frame acknowledgements, reconnect and return to lobby" : "FAIL: gameplay or reconnect acknowledgement missing"); SmokeStage = 4; SmokeStageTime = now; }
		if (SmokeStage == 4 && now - SmokeStageTime > 1000) { Stop(); System::SetQuit(true); }
	} else if (State == Mode::Client) {
		if (!SmokeChatSent) { SmokeChatSent = true; MP::Writer chat(Kind::Chat, Session, Epoch); chat.Text("Guest room chat test", 192); Net.Send(ServerAddress, chat.Data, Delivery::Control); }
		if (!Playing && !Players[LocalSlot].Ready) SendReady(true, Players[LocalSlot].Team);
		if (Playing && Presented >= 20 && SmokeStage == 0) { SmokeStage = 1; SmokeCapture = true; Verify("FRAMES: " + std::to_string(Presented) + " FPS: " + std::to_string(FPS)); }
		if (!Playing && SmokeStage == 1) { SmokeStage = 2; SmokeCapture = true; Verify("LOBBY: returned from match"); }
		if (Playing && SmokeStage == 2) { SmokeStage = 3; SmokeStageTime = now; SmokeSecondStart = Presented; }
		if (SmokeStage == 3 && now - SmokeStageTime > 6000 && Presented >= SmokeSecondStart + 20 && Texture) { SmokeStage = 4; SmokeCapture = true; const bool chat = std::any_of(Chat.begin(), Chat.end(), [](const auto& line) { return line.find("Guest room chat test") != std::string::npos; }); Verify(std::string(SmokeLoopPlays >= 2 && SmokeLoopStopped && chat ? "PASS: " : "FAIL: audio replay, stop or chat missing. ") + "second match. new frames=" + std::to_string(Presented - SmokeSecondStart) + " FPS=" + std::to_string(FPS) + " sounds=" + std::to_string(AudioReceived)); }
	} else if (SmokeStage == 4 && State == Mode::Idle) System::SetQuit(true);
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
	if (Net.QueuedBytes(peer.Address) > 512 * 1024) { Net.Close(peer.Address); peer.Connected = peer.Ready = false; peer.ReservedUntil = Now() + 60000; peer.Inputs.Reset(); peer.Outgoing.clear(); g_UInputMan.ClearRemoteInput(player); Lobby(); return; }
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

void MultiplayerMan::Impl::Draw() {
	if (CursorVerification) {
		const auto& test = CursorCases[CursorStage]; State = test.State; UI = test.UI; Playing = test.Playing;
		if (Activities.empty()) LoadActivities();
		RoomName = "Cursor verification"; Players[0].Name = "Host"; Players[0].Token = 1; Players[0].Connected = Players[0].Ready = true;
		const auto* viewport = ImGui::GetMainViewport(); ImGui::GetIO().MousePos = ImVec2(viewport->Pos.x + viewport->Size.x / 2 - 280, viewport->Pos.y + viewport->Size.y / 2);
	}
	// The engine hides SDL's cursor and draws its legacy pointer behind these panels.
	ImGui::GetIO().MouseDrawCursor = UI || State == Mode::Connecting || State == Mode::Reconnecting;
	if (State == Mode::Client && Playing && !Texture) { const auto* viewport = ImGui::GetMainViewport(); ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(0, 0, 0, 255)); ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2, viewport->Pos.y + viewport->Size.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f)); ImGui::Begin("Loading##mp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs); ImGui::Text("The host is loading the scene..."); ImGui::Text("Esc: session menu"); ImGui::End(); }
	if (State == Mode::Client && Playing && Texture) {
		const auto* viewport = ImGui::GetMainViewport(); const float scale = std::min(viewport->Size.x / TextureWidth, viewport->Size.y / TextureHeight);
		const ImVec2 size(TextureWidth * scale, TextureHeight * scale); const ImVec2 position(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f, viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f);
		auto* draw = ImGui::GetBackgroundDrawList(); draw->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(0, 0, 0, 255)); draw->AddImage(static_cast<ImTextureID>(Texture), position, ImVec2(position.x + size.x, position.y + size.y), ImVec2(0, 1), ImVec2(1, 0));
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 12, viewport->Pos.y + 12)); ImGui::SetNextWindowBgAlpha(0.7f);
		ImGui::Begin("Connection##mp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
		ImGui::Text("%s | %d ms | %.0f FPS | Esc: session menu", RoomName.c_str(), Net.Ping(ServerAddress), FPS);
		if (Now() - LastFrameReceived > 1500) ImGui::TextColored(ImVec4(1, 0.7f, 0.25f, 1), "Waiting for the host's next frame...");
		ImGui::End();
	}
	if (State == Mode::Host && Playing && !UI) {
		ImGui::SetNextWindowPos(ImVec2(12, 12)); ImGui::SetNextWindowBgAlpha(0.7f); ImGui::Begin("Host##mp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
		if (ImGui::Button("Session menu")) { UI = true; g_UInputMan.TrapMousePos(false); }
		ImGui::End();
	}
	if (!UI && State != Mode::Connecting && State != Mode::Reconnecting) return;
	const auto* viewport = ImGui::GetMainViewport(); ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2, viewport->Pos.y + viewport->Size.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(600.0f, viewport->Size.x - 24), 0), ImGuiCond_Always);
	ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(viewport->Size.x - 24, viewport->Size.y - 24));
	ImGui::Begin("Multiplayer", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
	if (!Error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.68f, 0.32f, 1)); ImGui::TextWrapped("%s", Error.c_str()); ImGui::PopStyleColor(); ImGui::Separator(); }
	if (State == Mode::Idle) {
		ImGui::InputText("Player name", Name, sizeof(Name));
		ImGui::Checkbox("Use room codes", &Online);
		if (Online && ImGui::TreeNode("Connection settings")) {
			ImGui::InputText("Server IP or hostname", ServiceAddress, sizeof(ServiceAddress));
			if (ImGui::Button("Save server address")) { std::string host; uint16_t port; if (Address(ServiceAddress, host, port, 8001)) { SaveService(); Error.clear(); } else Error = "Enter the server IP or hostname, optionally followed by :port."; }
			if (!DefaultService.empty()) { ImGui::SameLine(); if (ImGui::Button("Use default server")) { std::snprintf(ServiceAddress, sizeof(ServiceAddress), "%s", DefaultService.c_str()); SaveService(); Error.clear(); } }
			ImGui::TextWrapped("Use the same server as your friends. Port 8001 is used when you leave out the port."); ImGui::TreePop();
		}
		if (Online && !ServiceAddress[0]) ImGui::TextWrapped("Configure your room service address in Connection settings.");
		if (ImGui::BeginTabBar("mp-tabs")) {
			if (ImGui::BeginTabItem("Join")) {
				if (Online) ImGui::InputText("Room code", JoinCode, sizeof(JoinCode)); else ImGui::InputText("Host address", HostAddress, sizeof(HostAddress)); ImGui::InputText("Room password", Password, sizeof(Password), ImGuiInputTextFlags_Password);
				if (ImGui::Button("Join room", ImVec2(140, 0))) Join();
				if (!Online) { ImGui::SameLine(); if (ImGui::Button("Find LAN rooms")) { Discovered.clear(); if (Net.Start(false, 0, "", Error)) Net.Discover(8000); } }
				for (const auto& [address, room]: Discovered) if (ImGui::Selectable((room + "##" + address).c_str())) { std::snprintf(HostAddress, sizeof(HostAddress), "%s", address.c_str()); }
				ImGui::TextWrapped("%s", Online ? "Ask your host for their room code. No router setup is needed." : "Join with the host's address. LAN discovery searches UDP port 8000."); ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Host")) {
				ImGui::InputText("Room name", Room, sizeof(Room)); if (!Online) ImGui::InputInt("UDP port", &Port); ImGui::InputText("Room password", Password, sizeof(Password), ImGuiInputTextFlags_Password);
				ImGui::Combo("Stream quality", &Quality, "640 x 360 (recommended)\0 960 x 540\0");
				ImGui::SliderInt("Upload per guest (Mbps)", &BandwidthMbps, 6, 48);
				if (ImGui::Button("Create room", ImVec2(140, 0))) Host();
				ImGui::TextWrapped("%s", Online ? "Create a room and share its code. You run the match; the room service connects your friends." : "You play as the host. Internet guests need your public address and this UDP port forwarded to your computer."); ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		if (ImGui::Button("Back to main menu")) { Net.Stop(); UI = false; }
	} else if (State == Mode::Connecting || State == Mode::Reconnecting) {
		ImGui::Text("%s", State == Mode::Connecting ? "Connecting to the host..." : "Reconnecting...");
		ImGui::TextWrapped("%s", Online ? Relay::DisplayCode(Relay::NormalizeCode(JoinCode)).c_str() : HostAddress); if (ImGui::Button("Cancel")) { Stop(); UI = true; }
	} else {
		ImGui::Text("%s", RoomName.c_str());
		if (Online) { const auto code = Relay::DisplayCode(Net.RoomCode()); ImGui::Text("Room code: %s", code.empty() ? "Connecting..." : code.c_str()); if (!code.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("Copy code")) ImGui::SetClipboardText(code.c_str()); } ImGui::Text("%s", Net.IsRelayReady() ? "Connected through room service" : "Waiting for room service..."); }
		else if (State == Mode::Host) { ImGui::Text("Hosting on UDP port %d", Port); for (const auto& address: Net.LocalAddresses(static_cast<uint16_t>(Port))) { ImGui::Text("LAN address: %s", address.c_str()); ImGui::SameLine(); if (ImGui::SmallButton(("Copy##" + address).c_str())) ImGui::SetClipboardText(address.c_str()); } }
		else ImGui::Text("Connected to %s | %d ms", ServerAddress.c_str(), Net.Ping(ServerAddress));
		ImGui::Separator();
		if (ImGui::BeginTable("Players", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Player"); ImGui::TableSetupColumn("Team"); ImGui::TableSetupColumn("Status"); ImGui::TableHeadersRow();
			for (int i = 0; i < 4; ++i) { const auto& player = Players[i]; ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%s%s", player.Token ? player.Name.c_str() : "Open slot", i == 0 ? " (host)" : ""); ImGui::TableNextColumn();
				if (player.Token && !Playing && (State == Mode::Host || i == LocalSlot)) { ImGui::SetNextItemWidth(100); if (ImGui::BeginCombo("##team", ("Team " + std::to_string(player.Team + 1)).c_str())) { for (uint8_t team = 0; team < 4; ++team) if ((AvailableTeams & (1 << team)) && ImGui::Selectable(("Team " + std::to_string(team + 1)).c_str(), player.Team == team)) { if (State == Mode::Host) { Players[i].Team = team; Players[i].Ready = i == 0; Lobby(); } else SendReady(false, team); } ImGui::EndCombo(); } }
				else if (player.Token) ImGui::Text("Team %d", player.Team + 1);
				ImGui::TableNextColumn(); ImGui::Text("%s", !player.Token ? "Available" : !player.Connected ? "Reconnecting" : Playing ? "Playing" : player.Ready ? "Ready" : "Not ready");
				if (State == Mode::Host && !Playing && player.Token && !player.Connected) { ImGui::SameLine(); if (ImGui::SmallButton("Release slot")) { Net.Close(Players[i].Address); if (Players[i].Encoding.valid()) Players[i].Encoding.wait(); Players[i] = Player(); Lobby(); } }
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (!Playing && State == Mode::Host) {
			if (ImGui::BeginCombo("Activity", Activities.empty() ? "No multiplayer activities" : Activities[ActivityIndex]->GetPresetName().c_str())) { for (int i = 0; i < Activities.size(); ++i) if (ImGui::Selectable(Activities[i]->GetPresetName().c_str(), ActivityIndex == i)) { ActivityIndex = i; LoadScenes(); for (size_t slot = 1; slot < Players.size(); ++slot) Players[slot].Ready = false; Lobby(); } ImGui::EndCombo(); }
			if (ImGui::BeginCombo("Scene", Scenes.empty() ? "No compatible scenes" : Scenes[SceneIndex]->GetPresetName().c_str())) { for (int i = 0; i < Scenes.size(); ++i) if (ImGui::Selectable(Scenes[i]->GetPresetName().c_str(), SceneIndex == i)) { SceneIndex = i; SceneName = Scenes[i]->GetPresetName(); for (size_t slot = 1; slot < Players.size(); ++slot) Players[slot].Ready = false; Lobby(); } ImGui::EndCombo(); }
			bool changed = ImGui::SliderInt("Difficulty", &Difficulty, 0, 100); changed |= ImGui::InputInt("Starting gold", &Gold); Gold = std::clamp(Gold, 0, 1000000);
			changed |= ImGui::Checkbox("Fog of war", &Fog); ImGui::SameLine(); changed |= ImGui::Checkbox("Deploy scene units", &Deploy); changed |= ImGui::Checkbox("Clear path to orbit", &ClearOrbit);
			if (ImGui::TreeNode("Team factions")) { for (int team = 0; team < 4; ++team) if ((AvailableTeams & (1 << team)) || team + 1 == CPUTeam) { if (ImGui::BeginCombo(("Team " + std::to_string(team + 1) + " faction").c_str(), Tech[team].c_str())) { for (const auto& faction: Factions) if (ImGui::Selectable(faction.c_str(), Tech[team] == faction)) { Tech[team] = faction; changed = true; } ImGui::EndCombo(); } } ImGui::TreePop(); }
			if (changed) SettingsChanged();
			if (ImGui::Button("Start match", ImVec2(160, 0))) StartGame();
		} else if (!Playing) {
			ImGui::TextWrapped("Activity: %s\nScene: %s", ActivityName.c_str(), SceneName.c_str());
			ImGui::Text("Difficulty: %d | Gold: %d | Fog: %s", Difficulty, Gold, Fog ? "on" : "off");
			if (ImGui::Button(Players[LocalSlot].Ready ? "Not ready" : "Ready", ImVec2(160, 0))) SendReady(!Players[LocalSlot].Ready, Players[LocalSlot].Team);
		} else {
			if (ImGui::Button("Resume", ImVec2(140, 0))) { UI = false; g_UInputMan.TrapMousePos(true); }
			if (State == Mode::Host) { ImGui::SameLine(); if (ImGui::Button("Return everyone to lobby")) ReturnToLobby(); }
		}
		ImGui::Separator();
		if (ImGui::BeginChild("Room chat", ImVec2(0, 90), true)) { for (const auto& line: Chat) ImGui::TextWrapped("%s", line.c_str()); if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1); } ImGui::EndChild();
		ImGui::SetNextItemWidth(-70); const bool send = ImGui::InputText("##chat", ChatText, sizeof(ChatText), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
		if ((ImGui::Button("Send") || send) && ChatText[0] && Now() - LastChatSent >= 250) { LastChatSent = Now(); if (State == Mode::Host) AddChat(0, ChatText); else { MP::Writer writer(Kind::Chat, Session, Epoch); writer.Text(ChatText, 192); Net.Send(ServerAddress, writer.Data, Delivery::Control); } ChatText[0] = 0; }
		if (ImGui::Button(State == Mode::Host ? "Close room" : "Leave room")) { if (State == Mode::Host && g_ActivityMan.GetActivity()) { g_ActivityMan.EndActivity(); g_ActivityMan.PauseActivity(); } Stop(); UI = true; }
	}
	ImGui::End();
}
