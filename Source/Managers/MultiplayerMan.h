#pragma once

#include "Singleton.h"
#include "MultiplayerProtocol.h"

#include <memory>
#include <span>
#include <string>

struct BITMAP;

#define g_MultiplayerMan MultiplayerMan::Instance()

namespace RTE {
class MultiplayerMan : public Singleton<MultiplayerMan> {
public:
	MultiplayerMan();
	~MultiplayerMan();
	void Open();
	void Update();
	void UpdateMenu();
	void DrawUI();
	void ApplyInputs();
	void Stop();
	void HandleActivityExit();
	bool StartRoom(bool host, const std::string& address, bool smokeTest = false, bool hosted = false);
	bool StartDedicated(const std::string& configPath);
	bool IsDedicated() const;
	void CaptureVerificationFrame();
	bool IsUIOpen() const;
	bool IsHostingMatch() const;
	bool IsRemotePlayer(int player) const;
	void SetTextInputActive(int player, bool active);
	bool TakeTextInput(int player, std::string& text);
	bool TakeLaunchRequest();
	int ViewWidth(int screen) const;
	int ViewHeight(int screen) const;
	bool WantsState(int player) const;
	bool GuestView(int player, float& x, float& y) const;
	bool GuestCursor(int player, int mode, float& x, float& y) const;
	bool GuestPointer(int player, float& x, float& y) const;
	void BeginWorldCapture();
	void BeginWorldTrails();
	void EndWorldCapture();
	void BeginGuestView(BITMAP* gui, float cameraX, float cameraY);
	void EndGuestView(int player);
	// Encodes and queues the guest views captured in this draw pass.
	void FinishCaptures();
	// Opt-in server profiling (CCCP_MPBENCHMARK): per-stage loop and capture timing.
	void RecordSimulationUpdate();
	void RecordServerLoop(long long networkUs, long long simulationUs, int simulationUpdates, long long drawUs, long long idleUs);
private:
	struct Impl;
	std::unique_ptr<Impl> m_Impl;
};
} // namespace RTE
