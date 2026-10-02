#pragma once

#include "Singleton.h"
#include "MultiplayerProtocol.h"

#include <memory>
#include <span>
#include <string>

#define g_MultiplayerMan MultiplayerMan::Instance()

namespace RTE {
class MultiplayerMan : public Singleton<MultiplayerMan> {
public:
	MultiplayerMan();
	~MultiplayerMan();
	void Open();
	void Update();
	void DrawUI();
	void ApplyInputs();
	void Stop();
	bool StartRoom(bool host, const std::string& address, bool smokeTest = false);
	void CaptureVerificationFrame();
	bool IsUIOpen() const;
	bool IsHostingMatch() const;
	bool IsRemotePlayer(int player) const;
	void SetTextInputActive(int player, bool active);
	bool TakeTextInput(int player, std::string& text);
	bool TakeLaunchRequest();
	int ViewWidth(int screen) const;
	int ViewHeight(int screen) const;
	bool WantsFrame(int player) const;
	void CaptureFrame(int player, unsigned framebuffer, int width, int height, float cameraX, float cameraY);
private:
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
};
} // namespace RTE
