#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace RTE {
class AllegroScreen;
class AllegroBitmap;
class GUIInputWrapper;
class GUIControlManager;
class GUIControl;
class GUICollectionBox;
class Scene;

/// The room menus use the same controls, pixel fonts, skin and sounds as scenario setup.
/// This view owns presentation only; MultiplayerMan validates and executes every action.
class MultiplayerMenuGUI {
public:
	enum class Screen { Entry, Connecting, Lobby, Session, Loading };
	struct Slot {
		std::string Name, TeamName, Status;
		int Team = 0;
		bool Occupied = false, Connected = false, Ready = false, Editable = false;
	};
	struct View {
		Screen Page = Screen::Entry;
		bool Host = false, Online = true, Ready = false, Played = false;
		bool Fog = false, Deploy = false, ClearOrbit = false;
		int LocalSlot = 0, Difficulty = 50, Gold = 5000, Bandwidth = 24, Port = 8000;
		int ActivityIndex = 0, SceneIndex = 0;
		std::string Name, Room, Code, Address, Password, Service, DefaultService;
		std::string Title, Subtitle, Error, Notice, StartBlock, Activity, SceneName, Description;
		std::string LoadingMessage;
		std::vector<std::string> Activities, Scenes, Factions, FactionLabels, Chat, LANRooms;
		std::array<std::string, 4> Tech, TeamNames;
		std::array<Slot, 4> Players;
		std::vector<int> HumanTeams;
		int CPUTeam = -1;
		const Scene* SelectedScene = nullptr;
	};
	struct Event { std::string Control, Text; int Value = 0; };
	MultiplayerMenuGUI();
	~MultiplayerMenuGUI();
	std::vector<Event> Update(const View& view);
	void Draw(const View& view);
	void Hide();
	bool CursorDrawn() const { return m_CursorDrawn; }
	void SetVerificationPage(int page);
	std::string VerifyLayout() const;
	void QueueVerificationClick(const std::string& name) { m_VerificationClick = name; }
private:
	enum class Tab { Join, Host, Connection, Rules, Factions, Chat };
	std::unique_ptr<AllegroScreen> m_Screen;
	std::unique_ptr<GUIInputWrapper> m_Input;
	std::unique_ptr<GUIControlManager> m_Manager;
	std::unique_ptr<AllegroBitmap> m_MenuBitmap;
	unsigned int m_Texture = 0;
	std::unique_ptr<AllegroBitmap> m_PreviewBitmap;
	GUICollectionBox* m_Root = nullptr;
	Screen m_Page = Screen::Entry;
	Tab m_Tab = Tab::Join;
	bool m_Host = false, m_Rebuild = true, m_Visible = false, m_CursorDrawn = false, m_ConfirmLeave = false;
	int m_Width = 0, m_Height = 0, m_BoxWidth = 0, m_BoxHeight = 0;
	int m_VerificationPage = -1;
	bool m_Verification = false;
	std::string m_VerificationClick;
	std::vector<std::string> m_LastChat;
	const Scene* m_PreviewScene = nullptr;
	void Prepare(const View& view);
	void Build(const View& view);
	void Sync(const View& view);
	GUIControl* Add(const std::string& name, const std::string& type, int x, int y, int width, int height, const std::string& text = "", bool small = false);
	void Label(const std::string& name, int x, int y, int width, int height, const std::string& text, bool small = false);
	void Field(const std::string& name, int x, int y, int width, const std::string& title, const std::string& value, int limit = 128);
	void Combo(const std::string& name, int x, int y, int width, const std::vector<std::string>& items, int selection);
	void SetLabel(const std::string& name, const std::string& text);
	void SetField(const std::string& name, const std::string& text);
};
}
