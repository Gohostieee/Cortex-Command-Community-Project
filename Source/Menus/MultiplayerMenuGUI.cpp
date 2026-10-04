#include "MultiplayerMenuGUI.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "AllegroBitmap.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUILabel.h"
#include "GUITextBox.h"
#include "GUIComboBox.h"
#include "GUICheckbox.h"
#include "GUISlider.h"
#include "GUIListBox.h"
#include "FrameMan.h"
#include "WindowMan.h"
#include "UInputMan.h"
#include "Scene.h"
#include "GUI/imgui/imgui.h"
#include "raylib/rlgl.h"
#include <cstdint>
#include <tuple>

using namespace RTE;

MultiplayerMenuGUI::MultiplayerMenuGUI() = default;
MultiplayerMenuGUI::~MultiplayerMenuGUI() { Hide(); if (m_Texture) rlUnloadTexture(m_Texture); }

GUIControl* MultiplayerMenuGUI::Add(const std::string& name, const std::string& type, int x, int y, int width, int height, const std::string& text, bool small) {
	auto* control = m_Manager->AddControl(name, type, m_Root, x, y, width, height);
	RTEAssert(control, "Failed to create multiplayer menu control: " + name);
	if (auto* label = dynamic_cast<GUILabel*>(control)) {
		label->SetText(text); label->SetHAlignment(GUIFont::Left); label->SetVAlignment(GUIFont::Top);
		if (small) label->SetFont(m_Manager->GetSkin()->GetFont("FontSmall.png"));
	} else if (auto* button = dynamic_cast<GUIButton*>(control)) button->SetText(text);
	else if (auto* check = dynamic_cast<GUICheckbox*>(control)) check->SetText(text);
	return control;
}

void MultiplayerMenuGUI::Label(const std::string& name, int x, int y, int width, int height, const std::string& text, bool small) { Add(name, "LABEL", x, y, width, height, text, small); }

void MultiplayerMenuGUI::Field(const std::string& name, int x, int y, int width, const std::string& title, const std::string& value, int limit) {
	Label(name + "Label", x, y, width, 16, title, true);
	auto* field = dynamic_cast<GUITextBox*>(Add(name, "TEXTBOX", x, y + 16, width, 20));
	field->SetMaxTextLength(limit); field->SetText(value);
	if (name == "Password") field->SetPasswordMode(true);
}

void MultiplayerMenuGUI::Combo(const std::string& name, int x, int y, int width, const std::vector<std::string>& items, int selection) {
	auto* combo = dynamic_cast<GUIComboBox*>(Add(name, "COMBOBOX", x, y, width, 20));
	combo->SetDropHeight(std::min(180, m_BoxHeight - y - 12));
	combo->BeginUpdate(); for (const auto& item: items) combo->AddItem(item); combo->EndUpdate();
	if (!items.empty()) combo->SetSelectedIndex(std::clamp(selection, 0, static_cast<int>(items.size()) - 1));
}

void MultiplayerMenuGUI::SetLabel(const std::string& name, const std::string& text) {
	if (auto* control = dynamic_cast<GUILabel*>(m_Manager->GetControl(name))) control->SetText(text);
}

void MultiplayerMenuGUI::SetField(const std::string& name, const std::string& text) {
	if (auto* control = dynamic_cast<GUITextBox*>(m_Manager->GetControl(name)); control && !control->HasFocus() && control->GetText() != text) control->SetText(text);
}

void MultiplayerMenuGUI::Hide() {
	if (m_Manager) m_Manager->GetManager()->SetFocus(nullptr);
	m_Visible = m_CursorDrawn = false;
}

void MultiplayerMenuGUI::SetVerificationPage(int page) {
	m_VerificationPage = page; m_Verification = m_Rebuild = true;
}

void MultiplayerMenuGUI::Prepare(const View& view) {
	const int width = g_WindowMan.GetResX(), height = g_WindowMan.GetResY();
	if (m_Width != width || m_Height != height || m_Page != view.Page || m_Host != view.Host || m_Rebuild || !m_Manager) {
		Hide();
		m_Manager.reset(); m_Input.reset(); m_Screen.reset(); m_MenuBitmap.reset();
		if (m_Texture) { rlUnloadTexture(m_Texture); m_Texture = 0; }
		if (m_Page != view.Page) { m_Tab = Tab::Join; m_ConfirmLeave = false; }
		if (m_VerificationPage >= 0) {
			const int page = m_VerificationPage;
			m_Tab = page == 1 ? Tab::Host : page == 2 ? Tab::Connection : page == 3 ? Tab::Rules : page == 4 ? Tab::Factions : page == 5 ? Tab::Chat : Tab::Join;
			m_ConfirmLeave = page == 6;
			m_VerificationPage = -1;
		}
		m_Width = width; m_Height = height; m_Page = view.Page; m_Host = view.Host;
		m_MenuBitmap = std::make_unique<AllegroBitmap>(); m_MenuBitmap->Create(width, height, 32);
		m_Screen = std::make_unique<AllegroScreen>(m_MenuBitmap->GetBitmap());
		m_Input = std::make_unique<GUIInputWrapper>(-1, true);
		m_Manager = std::make_unique<GUIControlManager>();
		RTEAssert(m_Manager->Create(m_Screen.get(), m_Input.get(), "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Could not load multiplayer menu skin.");
		m_Texture = rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
		m_PreviewBitmap.reset(); Build(view); m_Rebuild = false; m_LastChat.clear(); m_PreviewScene = nullptr;
	}
	m_Visible = true; Sync(view);
}

void MultiplayerMenuGUI::Build(const View& view) {
	const bool lobby = view.Page == Screen::Lobby;
	m_BoxWidth = std::min(lobby ? 900 : 600, m_Width - 24);
	m_BoxHeight = std::min(lobby ? 500 : 360, m_Height - 24);
	const int w = m_BoxWidth, h = m_BoxHeight;
	m_Root = dynamic_cast<GUICollectionBox*>(m_Manager->AddControl("root", "COLLECTIONBOX", nullptr, (m_Width - w) / 2, (m_Height - h) / 2, w, h));
	m_Root->SetDrawType(GUICollectionBox::Panel);
	Label("Title", 16, 12, w - 32, 20, view.Title);
	Label("Subtitle", 16, 35, w - 32, 26, view.Subtitle, true);
	Label("Feedback", 16, h - 67, w - 32, 30, "", true);

	if (m_ConfirmLeave) {
		Label("ConfirmTitle", 24, 96, w - 48, 24, view.Host ? "CLOSE THIS ROOM?" : "LEAVE THIS ROOM?");
		Label("ConfirmText", 24, 132, w - 48, 64, view.Host ? "Everyone will be disconnected. Return to the lobby to keep your group together for another match." : "You will leave the group. You can join again using the room code.", true);
		Add("ConfirmLeave", "BUTTON", w - 184, h - 32, 168, 22, view.Host ? "Close room" : "Leave room");
		Add("KeepRoom", "BUTTON", 16, h - 32, 168, 22, "Stay in room");
		return;
	}
	if (view.Page == Screen::Entry) {
		const int third = (w - 48) / 3;
		Add("JoinTab", "BUTTON", 16, 65, third, 24, "Join game");
		Add("HostTab", "BUTTON", 24 + third, 65, third, 24, "Host game");
		Add("ConnectionTab", "BUTTON", 32 + third * 2, 65, third, 24, "Connection");
		dynamic_cast<GUIButton*>(m_Manager->GetControl(m_Tab == Tab::Connection ? "ConnectionTab" : m_Tab == Tab::Host || m_Tab == Tab::Rules ? "HostTab" : "JoinTab"))->SetPushed(true);
		const int half = (w - 48) / 2;
		if (m_Tab == Tab::Connection) {
			Combo("Online", 16, 112, w - 32, {"Online - invitation codes", "Local network / direct address"}, view.Online ? 0 : 1);
			Field("Service", 16, 146, w - 32, "Room server", view.Service, 255);
			Label("ConnectionHelp", 16, 197, w - 32, 38, "Friends must use the same room server. Leave this as supplied for normal online play.", true);
			Add("SaveService", "BUTTON", 16, 245, half, 22, "Save server");
			Add("DefaultService", "BUTTON", 32 + half, 245, half, 22, "Use default")->SetEnabled(!view.DefaultService.empty());
			Add("EntryBack", "BUTTON", 16, h - 32, 160, 22, "Back to main menu");
		} else {
			Field("Name", 16, 105, half, "Your player name", view.Name, 31);
			Field("Password", 32 + half, 105, half, "Room password (optional)", view.Password, 63);
			if (m_Tab == Tab::Host) {
				Field("Room", 16, 158, w - 32, "Room name", view.Room, 63);
				Label("HostHelp", 16, 214, w - 32, 22, "Create a lobby, invite your friends, then choose the battlefield together.", true);
				Add("HostConnectionSettings", "BUTTON", 16, std::min(264, h - 93), 176, 22, "Connection settings");
				Add("Create", "BUTTON", w - 184, h - 32, 168, 22, "Create lobby");
			} else if (m_Tab == Tab::Rules) {
				Label("HostConnectionTitle", 16, 155, w - 32, 16, "HOST CONNECTION SETTINGS", true);
				Label("ResolutionInfo", 16, 180, w - 32, 22, "Each guest draws at their own game resolution.", true);
				Field("Bandwidth", 16, 214, half, "Upload per guest (1-48 Mbps)", std::to_string(view.Bandwidth), 2);
				Field("Port", 32 + half, 214, half, "Direct connection port", std::to_string(view.Port), 5);
				Add("HostDone", "BUTTON", w - 184, h - 32, 168, 22, "Done");
			} else {
				Field(view.Online ? "Code" : "Address", 16, 158, w - 32, view.Online ? "Invitation code" : "Host address", view.Online ? view.Code : view.Address, view.Online ? 31 : 255);
				Label("JoinHelp", 16, 214, w - 32, 38, view.Online ? "Ask your host for the room code. Your group stays together between matches." : "Enter the host's address, or find a game on your local network.", true);
				if (!view.Online) { const int y = std::min(255, h - 93); Add("Discover", "BUTTON", 16, y, 150, 22, "Find LAN games"); Combo("LANRooms", 178, y, w - 194, view.LANRooms, -1); }
				Add("Join", "BUTTON", w - 184, h - 32, 168, 22, "Join lobby");
			}
			Add("EntryBack", "BUTTON", 16, h - 32, 160, 22, "Back to main menu");
		}
		return;
	}
	if (view.Page == Screen::Connecting || view.Page == Screen::Loading) {
		Label("ConnectTitle", 24, h / 2 - 35, w - 48, 24, view.Page == Screen::Loading ? "PREPARING THE BATTLEFIELD" : "CONNECTING TO YOUR GROUP");
		Label("ConnectInfo", 24, h / 2, w - 48, 58, view.Page == Screen::Loading ? "The host is loading the scene. Your game will appear here as soon as it is ready." : "Your player slot and team are restored automatically when reconnecting.", true);
		Add(view.Page == Screen::Loading ? "Session" : "Cancel", "BUTTON", 16, h - 32, 160, 22, view.Page == Screen::Loading ? "Session menu" : "Cancel");
		return;
	}
	if (view.Page == Screen::Session) {
		Label("SessionActivity", 24, 88, w - 48, 20, view.Activity + " / " + view.SceneName);
		if (m_Tab == Tab::Chat) {
			Add("ChatLog", "LISTBOX", 24, 119, w - 48, h - 229);
			auto* chat = dynamic_cast<GUITextBox*>(Add("Chat", "TEXTBOX", 24, h - 101, w - 116, 20)); chat->SetMaxTextLength(192);
			Add("Send", "BUTTON", w - 76, h - 101, 52, 20, "Send");
			Add("MatchTab", "BUTTON", w - 184, h - 32, 168, 22, "Back to session");
			return;
		}
		Label("SessionInfo", 24, 119, w - 48, 46, view.Host ? "The match continues while this menu is open. Return to the lobby to set up another round with the same group." : "The match continues while this menu is open. Your host can return everyone to the lobby for another round.", true);
		Add("Resume", "BUTTON", 24, 174, w - 48, 24, "Resume match");
		if (view.Host) Add("Return", "BUTTON", 24, 210, w - 48, 24, "Return everyone to lobby");
		Add("ChatTab", "BUTTON", 24, std::min(246, h - 93), w - 48, 22, "Room chat");
		Add("Leave", "BUTTON", 16, h - 32, 160, 22, view.Host ? "Close room..." : "Leave room...");
		return;
	}

	// Room identity stays above both panes; players and match settings are peers.
	Label("Invitation", 16, 64, w - 140, 22, "");
	Add("Copy", "BUTTON", w - 116, 61, 100, 22, "Copy invite");
	const int left = (w - 48) * 44 / 100, rightX = left + 32, right = w - rightX - 16;
	const int content = h - 165;
	auto* roster = dynamic_cast<GUICollectionBox*>(Add("RosterPanel", "COLLECTIONBOX", 16, 93, left, 156)); roster->SetDrawType(GUICollectionBox::Panel);
	Label("RosterTitle", 28, 102, left - 24, 16, "PLAYERS", true);
	for (int i = 0; i < 4; ++i) {
		const int y = 125 + i * 29;
		Label("Player" + std::to_string(i), 28, y, left - 124, 16, "", true);
		Label("Status" + std::to_string(i), 28, y + 12, left - 124, 14, "", true);
		Combo("Team" + std::to_string(i), left - 80, y, 84, {}, 0);
		Add("Release" + std::to_string(i), "BUTTON", left - 80, y, 84, 21, "Release slot")->SetVisible(false);
	}
	const int chatHeight = content - 166;
	if (chatHeight >= 75 && m_Tab != Tab::Chat) {
		Label("ChatTitle", 16, 259, left, 14, "ROOM CHAT", true);
		Add("ChatLog", "LISTBOX", 16, 277, left, h - 389);
		auto* chat = dynamic_cast<GUITextBox*>(Add("Chat", "TEXTBOX", 16, h - 107, left - 60, 20)); chat->SetMaxTextLength(192);
		Add("Send", "BUTTON", left - 36, h - 107, 52, 20, "Send");
	} else Add("ChatTab", "BUTTON", 16, h - 87, left, 20, "Room chat");

	auto* match = dynamic_cast<GUICollectionBox*>(Add("MatchPanel", "COLLECTIONBOX", rightX, 93, right, content)); match->SetDrawType(GUICollectionBox::Panel);
	const int x = rightX + 12, rw = right - 24;
	if (m_Tab == Tab::Chat) {
		Label("ChatTitle", x, 103, rw, 18, "ROOM CHAT", true);
		Add("ChatLog", "LISTBOX", x, 127, rw, content - 94);
		auto* chat = dynamic_cast<GUITextBox*>(Add("Chat", "TEXTBOX", x, h - 127, rw - 60, 20)); chat->SetMaxTextLength(192);
		Add("Send", "BUTTON", x + rw - 52, h - 127, 52, 20, "Send");
		Add("MatchTab", "BUTTON", x, h - 95, rw, 20, "Back to match");
	} else if (m_Tab == Tab::Rules) {
		Label("RulesTitle", x, 103, rw, 18, "MATCH RULES", true);
		Field("Gold", x, 133, rw / 2 - 8, "Starting gold", std::to_string(view.Gold), 7);
		Field("Difficulty", x + rw / 2 + 8, 133, rw / 2 - 8, "Difficulty (0-100)", std::to_string(view.Difficulty), 3);
		Add("Fog", "CHECKBOX", x, 180, rw, 18, "Fog of war");
		Add("Deploy", "CHECKBOX", x, 201, rw, 18, "Deploy scene units");
		Add("Orbit", "CHECKBOX", x, 222, rw, 18, "Clear path to orbit");
		Add("FactionsTab", "BUTTON", x, h - 95, rw / 2 - 4, 22, "Factions");
		Add("MatchTab", "BUTTON", x + rw / 2 + 4, h - 95, rw / 2 - 4, 22, "Done");
	} else if (m_Tab == Tab::Factions) {
		Label("FactionsTitle", x, 103, rw, 18, "TEAM FACTIONS", true);
		int row = 0;
		for (int team = 0; team < 4; ++team) if (std::find(view.HumanTeams.begin(), view.HumanTeams.end(), team) != view.HumanTeams.end() || team == view.CPUTeam) {
			const int y = 133 + row++ * 29;
			Label("FactionLabel" + std::to_string(team), x, y, 94, 20, view.TeamNames[team] + (team == view.CPUTeam ? " (AI)" : ""), true);
			Combo("Faction" + std::to_string(team), x + 98, y, rw - 98, view.FactionLabels, 0);
		}
		Add("RulesTab", "BUTTON", x, h - 95, rw, 22, "Back to rules");
	} else {
		Label("MatchTitle", x, 103, rw, 18, view.Host ? "SET UP THE MATCH" : "NEXT MATCH", true);
		Label("ActivityLabel", x, 127, rw, 15, "Activity", true);
		Combo("Activity", x, 144, rw, view.Activities, view.ActivityIndex);
		Label("SceneLabel", x, 172, rw, 15, "Battlefield", true);
		Combo("Scene", x, 189, rw, view.Scenes, view.SceneIndex);
		if (content >= 320) {
			auto* preview = dynamic_cast<GUICollectionBox*>(Add("Preview", "COLLECTIONBOX", x, 219, c_ScenePreviewWidth, c_ScenePreviewHeight)); preview->SetDrawType(GUICollectionBox::Image);
			Label("Description", x + c_ScenePreviewWidth + 12, 219, rw - c_ScenePreviewWidth - 12, 90, view.Description, true);
			Label("RulesSummary", x, 319, rw, 32, "", true);
		} else Label("RulesSummary", x, 218, rw, 16, "", true);
		Add("RulesTab", "BUTTON", x, h - 99, rw, 22, view.Host ? "Match rules and factions" : "View rules and factions");
	}
	Add("Leave", "BUTTON", 16, h - 32, 160, 22, view.Host ? "Close room..." : "Leave room...");
	Add("Primary", "BUTTON", w - 206, h - 32, 190, 22, "");
}

void MultiplayerMenuGUI::Sync(const View& view) {
	SetLabel("Title", view.Title); SetLabel("Subtitle", view.Subtitle);
	std::string feedback = view.Notice;
	if (view.Page == Screen::Lobby && !view.StartBlock.empty()) feedback += (feedback.empty() ? "" : "\n") + view.StartBlock;
	SetLabel("Feedback", view.Error.empty() ? feedback : view.Error);
	SetField("Name", view.Name); SetField("Room", view.Room); SetField("Password", view.Password);
	SetField("Code", view.Code); SetField("Address", view.Address); SetField("Service", view.Service);
	if (auto* rooms = dynamic_cast<GUIComboBox*>(m_Manager->GetControl("LANRooms")); rooms && rooms->GetCount() != view.LANRooms.size()) { rooms->ClearList(); for (const auto& room: view.LANRooms) rooms->AddItem(room); }
	if (auto* mode = dynamic_cast<GUIComboBox*>(m_Manager->GetControl("Online")); mode && !mode->IsDropped() && mode->GetSelectedIndex() != (view.Online ? 0 : 1)) mode->SetSelectedIndex(view.Online ? 0 : 1);
	if (view.Page == Screen::Loading) SetLabel("ConnectInfo", view.LoadingMessage.empty() ? "Preparing your battlefield..." : view.LoadingMessage);
	if (view.Page == Screen::Session && !view.Host && !view.NetworkStatus.empty()) SetLabel("SessionInfo", view.NetworkStatus + "\nThe match continues while this menu is open.");
	if (m_ConfirmLeave) return;
	if (auto* log = dynamic_cast<GUIListBox*>(m_Manager->GetControl("ChatLog")); log && m_LastChat != view.Chat) {
		log->ClearList(); for (const auto& line: view.Chat) log->AddItem(line); log->ScrollToBottom(); m_LastChat = view.Chat;
	}
	if (view.Page != Screen::Lobby) return;
	SetLabel("Invitation", view.Online ? "ROOM CODE   " + (view.Code.empty() ? std::string("Connecting...") : view.Code) : "INVITE   " + view.Address);
	if (auto* copy = m_Manager->GetControl("Copy")) copy->SetEnabled(view.Online ? !view.Code.empty() : !view.Address.empty());
	for (int i = 0; i < 4; ++i) {
		const auto& slot = view.Players[i]; const std::string suffix = std::to_string(i);
		SetLabel("Player" + suffix, slot.Occupied ? slot.Name + (i == 0 ? " [HOST]" : i == view.LocalSlot ? " [YOU]" : "") : "Open player slot");
		SetLabel("Status" + suffix, slot.Status);
		auto* team = dynamic_cast<GUIComboBox*>(m_Manager->GetControl("Team" + suffix));
		if (team) {
			bool different = team->GetCount() != view.HumanTeams.size();
			for (int n = 0; !different && n < team->GetCount(); ++n) different = team->GetItem(n)->m_Name != view.TeamNames[view.HumanTeams[n]];
			if (different) { team->ClearList(); for (int id: view.HumanTeams) team->AddItem(view.TeamNames[id]); }
			const auto selected = std::find(view.HumanTeams.begin(), view.HumanTeams.end(), slot.Team);
			if (!team->IsDropped() && selected != view.HumanTeams.end() && team->GetSelectedIndex() != selected - view.HumanTeams.begin()) team->SetSelectedIndex(static_cast<int>(selected - view.HumanTeams.begin()));
			team->SetVisible(slot.Occupied && slot.Connected); team->SetEnabled(slot.Editable);
		}
		if (auto* release = m_Manager->GetControl("Release" + suffix)) release->SetVisible(view.Host && slot.Occupied && !slot.Connected);
	}
	for (const auto& [id, items, selected]: {std::tuple<std::string, const std::vector<std::string>*, int>{"Activity", &view.Activities, view.ActivityIndex}, {"Scene", &view.Scenes, view.SceneIndex}}) {
		if (auto* combo = dynamic_cast<GUIComboBox*>(m_Manager->GetControl(id))) {
			bool different = combo->GetCount() != items->size();
			for (int n = 0; !different && n < combo->GetCount(); ++n) different = combo->GetItem(n)->m_Name != (*items)[n];
			if (different) { combo->ClearList(); for (const auto& item: *items) combo->AddItem(item); }
			if (!combo->IsDropped() && !items->empty() && combo->GetSelectedIndex() != selected) combo->SetSelectedIndex(selected);
			combo->SetEnabled(view.Host && !items->empty());
		}
	}
	SetField("Gold", std::to_string(view.Gold)); SetField("Difficulty", std::to_string(view.Difficulty));
	for (const auto& [name, value]: {std::pair{"Fog", view.Fog}, {"Deploy", view.Deploy}, {"Orbit", view.ClearOrbit}}) {
		if (auto* check = dynamic_cast<GUICheckbox*>(m_Manager->GetControl(name))) { check->SetCheck(value ? GUICheckbox::Checked : GUICheckbox::Unchecked); check->SetEnabled(view.Host); }
	}
	for (const auto* name: {"Gold", "Difficulty"}) if (auto* field = m_Manager->GetControl(name)) field->SetEnabled(view.Host);
	for (int team = 0; team < 4; ++team) if (auto* combo = dynamic_cast<GUIComboBox*>(m_Manager->GetControl("Faction" + std::to_string(team)))) {
		const auto tech = std::find(view.Factions.begin(), view.Factions.end(), view.Tech[team]);
		if (!combo->IsDropped() && tech != view.Factions.end() && combo->GetSelectedIndex() != tech - view.Factions.begin()) combo->SetSelectedIndex(static_cast<int>(tech - view.Factions.begin()));
		combo->SetEnabled(view.Host);
	}
	SetLabel("RulesSummary", "Gold: " + std::to_string(view.Gold) + "   Difficulty: " + std::to_string(view.Difficulty) + "\nFog: " + (view.Fog ? "on" : "off") + "   Deployment: " + (view.Deploy ? "on" : "off"));
	SetLabel("Description", view.Description);
	if (view.SelectedScene != m_PreviewScene) {
		if (auto* preview = dynamic_cast<GUICollectionBox*>(m_Manager->GetControl("Preview"))) {
			preview->SetDrawImage(nullptr); m_PreviewBitmap.reset();
			if (auto* bitmap = view.SelectedScene ? view.SelectedScene->GetPreviewBitmap() : nullptr) {
				m_PreviewBitmap = std::make_unique<AllegroBitmap>(); m_PreviewBitmap->Create(c_ScenePreviewWidth, c_ScenePreviewHeight, 32);
				clear_to_color(m_PreviewBitmap->GetBitmap(), ColorKeys::g_MaskColor);
				draw_sprite(m_PreviewBitmap->GetBitmap(), bitmap, 0, 0);
				preview->SetDrawImage(new AllegroBitmap(m_PreviewBitmap->GetBitmap()));
			}
		}
		m_PreviewScene = view.SelectedScene;
	}
	if (auto* primary = dynamic_cast<GUIButton*>(m_Manager->GetControl("Primary"))) {
		primary->SetText(view.Host ? view.Played ? "Play again" : "Start match" : view.Ready ? "Cancel ready" : "Ready up");
		primary->SetEnabled(!view.Host || view.StartBlock.empty());
	}
}

std::vector<MultiplayerMenuGUI::Event> MultiplayerMenuGUI::Update(const View& view) {
	Prepare(view);
	m_Manager->Update();
	if (!m_VerificationClick.empty()) {
		if (auto* button = dynamic_cast<GUIButton*>(m_Manager->GetControl(m_VerificationClick)); button && button->GetVisible() && button->GetEnabled()) {
			int x, y, w, h; button->GetControlRect(&x, &y, &w, &h);
			button->OnMouseDown(x + w / 2, y + h / 2, GUIPanel::MOUSE_LEFT, 0);
			button->OnMouseUp(x + w / 2, y + h / 2, GUIPanel::MOUSE_LEFT, 0);
		}
		m_VerificationClick.clear();
	}
	std::vector<Event> result;
	GUIEvent event;
	while (m_Manager->GetEvent(&event)) {
		auto* control = event.GetControl(); if (!control || !control->GetEnabled()) continue;
		const auto& name = control->GetName();
		if (event.GetType() == GUIEvent::Command) {
			g_GUISound.ButtonPressSound()->Play();
			if (name == "JoinTab" || name == "HostTab" || name == "HostDone" || name == "ConnectionTab" || name == "HostConnectionSettings" || name == "RulesTab" || name == "FactionsTab" || name == "ChatTab" || name == "MatchTab") {
				m_Tab = name == "HostTab" || name == "HostDone" ? Tab::Host : name == "ConnectionTab" ? Tab::Connection : name == "RulesTab" || name == "HostConnectionSettings" ? Tab::Rules : name == "FactionsTab" ? Tab::Factions : name == "ChatTab" ? Tab::Chat : Tab::Join; m_Rebuild = true;
			} else if (name == "Leave") { m_ConfirmLeave = m_Rebuild = true; }
			else if (name == "KeepRoom") { m_ConfirmLeave = false; m_Rebuild = true; }
			else { std::string text; if (name == "Send") if (auto* chat = dynamic_cast<GUITextBox*>(m_Manager->GetControl("Chat"))) { text = chat->GetText(); chat->SetText(""); } result.push_back({name, text}); }
		} else if (auto* field = dynamic_cast<GUITextBox*>(control); field && (event.GetMsg() == GUITextBox::Changed || event.GetMsg() == GUITextBox::Enter)) {
			result.push_back({name == "Chat" && event.GetMsg() == GUITextBox::Enter ? "Send" : name, field->GetText()});
			if (name == "Chat" && event.GetMsg() == GUITextBox::Enter) field->SetText("");
		} else if (auto* combo = dynamic_cast<GUIComboBox*>(control); combo && event.GetMsg() == GUIComboBox::Closed && combo->GetSelectedIndex() != combo->GetOldSelectionIndex()) {
			g_GUISound.SelectionChangeSound()->Play(); result.push_back({name, "", combo->GetSelectedIndex()});
		} else if (auto* check = dynamic_cast<GUICheckbox*>(control); check && event.GetMsg() == GUICheckbox::Changed) result.push_back({name, "", check->GetCheck() == GUICheckbox::Checked});
	}
	// GUIControlManager drains its event stack in reverse order. Apply edits
	// before a button pressed in the same frame consumes the form values.
	std::reverse(result.begin(), result.end());
	return result;
}

void MultiplayerMenuGUI::Draw(const View& view) {
	Prepare(view);
	// Native Allegro controls draw into a transparent CPU bitmap; upload that layer
	// above locally rendered gameplay so guests receive the same native menus.
	clear_to_color(m_MenuBitmap->GetBitmap(), 0);
	m_Manager->Draw(); m_Manager->SetCursor(GUIControlManager::Pointer);
	std::array<int, 24 * 24> before{}, after{};
	if (m_Verification) {
		int x, y; m_Input->GetMousePosition(&x, &y); m_Input->SetMouseOffset(24 - x, 24 - y);
		for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) before[y * 24 + x] = getpixel(m_MenuBitmap->GetBitmap(), 24 + x, 24 + y);
	}
	m_Manager->DrawMouse(); m_CursorDrawn = true;
	if (m_Verification) {
		for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) after[y * 24 + x] = getpixel(m_MenuBitmap->GetBitmap(), 24 + x, 24 + y);
		m_CursorDrawn = before != after; m_Input->SetMouseOffset(0, 0);
	}
	// Match ScreenBlit's native GUI transparency rule. Palette thumbnails and
	// primitive text colours have RGB data without an Allegro alpha channel.
	auto* pixels = reinterpret_cast<uint32_t*>(m_MenuBitmap->GetBitmap()->line[0]);
	for (size_t i = 0; i < size_t(m_Width) * m_Height; ++i) if (pixels[i] & 0x00FFFFFF) pixels[i] |= 0xFF000000;
	int previousTexture, alignment; glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture); glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glBindTexture(GL_TEXTURE_2D, m_Texture); glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, GL_RGBA, GL_UNSIGNED_BYTE, m_MenuBitmap->GetBitmap()->line[0]);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment); glBindTexture(GL_TEXTURE_2D, previousTexture);
	const auto* viewport = ImGui::GetMainViewport(); auto* draw = ImGui::GetBackgroundDrawList();
	draw->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(0, 0, 0, view.Page == Screen::Session ? 160 : 90));
	draw->AddImage(static_cast<ImTextureID>(m_Texture), viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
}

std::string MultiplayerMenuGUI::VerifyLayout() const {
	if (!m_Visible || !m_Manager) return "Menu is hidden";
	int rx, ry, rw, rh; m_Root->GetControlRect(&rx, &ry, &rw, &rh);
	std::vector<std::tuple<std::string, int, int, int, int>> interactive;
	for (auto* control: *m_Manager->GetControlList()) {
		if (!control->GetVisible()) continue;
		int x, y, w, h; control->GetControlRect(&x, &y, &w, &h);
		if (x < rx || y < ry || w <= 0 || h <= 0 || x + w > rx + rw || y + h > ry + rh) return "Control outside menu: " + control->GetName();
		if (dynamic_cast<GUIButton*>(control) || dynamic_cast<GUITextBox*>(control) || dynamic_cast<GUIComboBox*>(control) || dynamic_cast<GUICheckbox*>(control) || dynamic_cast<GUIListBox*>(control)) interactive.emplace_back(control->GetName(), x, y, w, h);
	}
	for (size_t i = 0; i < interactive.size(); ++i) for (size_t j = i + 1; j < interactive.size(); ++j) {
		const auto& [a, ax, ay, aw, ah] = interactive[i]; const auto& [b, bx, by, bw, bh] = interactive[j];
		if (ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah) return "Overlapping controls: " + a + " / " + b;
	}
	const char* expected = m_ConfirmLeave ? "ConfirmLeave" : m_Page == Screen::Entry ? m_Tab == Tab::Host ? "Create" : m_Tab == Tab::Connection ? "SaveService" : m_Tab == Tab::Rules ? "ResolutionInfo" : "Join" : m_Page == Screen::Lobby ? m_Tab == Tab::Rules ? "Gold" : m_Tab == Tab::Factions ? "FactionsTitle" : m_Tab == Tab::Chat ? "ChatLog" : "Activity" : m_Page == Screen::Session ? m_Tab == Tab::Chat ? "ChatLog" : "Resume" : "ConnectTitle";
	return m_Manager->GetControl(expected) ? "" : "Expected page missing: " + std::string(expected);
}
