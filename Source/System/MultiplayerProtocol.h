#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace RTE::MP {

inline constexpr uint8_t PacketID = 220;
inline constexpr uint32_t Magic = 0x43434D50;
inline constexpr uint16_t Version = 6;
inline constexpr size_t MaxPlayers = 4;
inline constexpr size_t InputCount = 64;
inline constexpr uint16_t ChunkBytes = 1100, ParityGroup = 8;
inline constexpr uint32_t InputTimeoutMS = 250;

// Values 5 and 6 belonged to the retired framebuffer stream.
enum class Kind : uint8_t { Hello = 0, Welcome, Lobby, Ready, Input, Leave = 7, Reject, Sound, Announcement, Chat, TextInput, WorldSnapshot, WorldResource, WorldAck, WorldManifest };
inline bool Newer(uint32_t value, uint32_t previous) { return value != previous && uint32_t(value - previous) < 0x80000000u; }

// The wire format is explicit big endian. No engine structures, pointers, hashes,
// size_t, compiler padding, or native-endian floats cross this interface.
class Writer {
public:
	std::vector<uint8_t> Data;
	void U8(uint8_t value) { Data.push_back(value); }
	void U16(uint16_t value) { U8(value >> 8); U8(value & 255); }
	void U32(uint32_t value) { U16(value >> 16); U16(value & 65535); }
	void U64(uint64_t value) { U32(value >> 32); U32(value & 0xffffffffu); }
	void F32(float value) { U32(std::bit_cast<uint32_t>(value)); }
	void Text(const std::string& value, size_t limit = 128) { const size_t size = std::min(value.size(), limit); U16(static_cast<uint16_t>(size)); Data.insert(Data.end(), value.begin(), value.begin() + size); }
	void Bytes(std::span<const uint8_t> value) { Data.insert(Data.end(), value.begin(), value.end()); }
	Writer(Kind kind, uint64_t session = 0, uint32_t epoch = 0) { U8(PacketID); U32(Magic); U16(Version); U8(static_cast<uint8_t>(kind)); U64(session); U32(epoch); }
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> data): m_Data(data) {}
	bool U8(uint8_t& value) { if (Remaining() < 1) return false; value = m_Data[m_Pos++]; return true; }
	bool U16(uint16_t& value) { uint8_t a, b; if (!U8(a) || !U8(b)) return false; value = uint16_t(a) << 8 | b; return true; }
	bool U32(uint32_t& value) { uint16_t a, b; if (!U16(a) || !U16(b)) return false; value = uint32_t(a) << 16 | b; return true; }
	bool U64(uint64_t& value) { uint32_t a, b; if (!U32(a) || !U32(b)) return false; value = uint64_t(a) << 32 | b; return true; }
	bool F32(float& value) { uint32_t bits; if (!U32(bits)) return false; value = std::bit_cast<float>(bits); return std::isfinite(value); }
	bool Text(std::string& value, size_t limit = 128) { uint16_t size; if (!U16(size) || size > limit || Remaining() < size) return false; value.assign(reinterpret_cast<const char*>(m_Data.data() + m_Pos), size); m_Pos += size; return value.find('\0') == std::string::npos; }
	bool Done() const { return Remaining() == 0; }
	size_t Remaining() const { return m_Data.size() - m_Pos; }
	std::span<const uint8_t> Rest() const { return m_Data.subspan(m_Pos); }
private:
	std::span<const uint8_t> m_Data;
	size_t m_Pos = 0;
};

struct Header { Kind Type{}; uint64_t Session = 0; uint32_t Epoch = 0; };
inline bool ReadHeader(Reader& reader, Header& header) {
	uint8_t id, kind; uint32_t magic; uint16_t version;
	if (!reader.U8(id) || !reader.U32(magic) || !reader.U16(version) || !reader.U8(kind) || !reader.U64(header.Session) || !reader.U32(header.Epoch)) return false;
	if (id != PacketID || magic != Magic || version != Version || kind == 5 || kind == 6 || kind > static_cast<uint8_t>(Kind::WorldManifest)) return false;
	header.Type = static_cast<Kind>(kind); return true;
}

struct Input {
	uint32_t Sequence = 0, MouseX = 0, MouseY = 0, Wheel = 0;
	uint64_t Held = 0;
	std::array<uint16_t, InputCount> Presses{}, Releases{};
	uint8_t MouseHeld = 0;
	uint8_t Device = 1;
	std::array<uint16_t, 3> MousePresses{}, MouseReleases{};
	float AimX = 0, AimY = 0, MoveX = 0, MoveY = 0;
	// Client-owned presentation coordinates. The host uses these for capture
	// and cursor actions, never to correct the client's view.
	bool ViewValid = false, CursorValid = false, PointerValid = false;
	uint8_t CursorMode = 0;
	float ViewX = 0, ViewY = 0, CursorX = 0, CursorY = 0;
	float PointerX = 0, PointerY = 0;
};
inline void WriteInput(Writer& writer, const Input& input) {
	writer.U32(input.Sequence); writer.U32(input.MouseX); writer.U32(input.MouseY); writer.U32(input.Wheel); writer.U64(input.Held);
	for (auto value: input.Presses) writer.U16(value);
	for (auto value: input.Releases) writer.U16(value);
	writer.U8(input.MouseHeld);
	writer.U8(input.Device);
	for (auto value: input.MousePresses) writer.U16(value);
	for (auto value: input.MouseReleases) writer.U16(value);
	writer.F32(input.AimX); writer.F32(input.AimY); writer.F32(input.MoveX); writer.F32(input.MoveY);
	writer.U8(input.ViewValid); writer.U8(input.CursorValid); writer.U8(input.PointerValid); writer.U8(input.CursorMode);
	writer.F32(input.ViewX); writer.F32(input.ViewY); writer.F32(input.CursorX); writer.F32(input.CursorY);
	writer.F32(input.PointerX); writer.F32(input.PointerY);
}
inline bool ReadInput(Reader& reader, Input& input) {
	if (!reader.U32(input.Sequence) || !reader.U32(input.MouseX) || !reader.U32(input.MouseY) || !reader.U32(input.Wheel) || !reader.U64(input.Held)) return false;
	for (auto& value: input.Presses) if (!reader.U16(value)) return false;
	for (auto& value: input.Releases) if (!reader.U16(value)) return false;
	if (!reader.U8(input.MouseHeld) || input.MouseHeld > 7) return false;
	if (!reader.U8(input.Device) || input.Device > 5) return false;
	for (auto& value: input.MousePresses) if (!reader.U16(value)) return false;
	for (auto& value: input.MouseReleases) if (!reader.U16(value)) return false;
	if (!reader.F32(input.AimX) || !reader.F32(input.AimY) || !reader.F32(input.MoveX) || !reader.F32(input.MoveY) || std::abs(input.AimX) > 1.01f || std::abs(input.AimY) > 1.01f || std::abs(input.MoveX) > 1.01f || std::abs(input.MoveY) > 1.01f) return false;
	uint8_t view, cursor, pointer;
	if (!reader.U8(view) || view > 1 || !reader.U8(cursor) || cursor > 1 || !reader.U8(pointer) || pointer > 1 || !reader.U8(input.CursorMode) || input.CursorMode > 20) return false;
	input.ViewValid = view; input.CursorValid = cursor; input.PointerValid = pointer;
	for (float* value : {&input.ViewX, &input.ViewY, &input.CursorX, &input.CursorY, &input.PointerX, &input.PointerY}) if (!reader.F32(*value) || std::abs(*value) > 1000000) return false;
	return reader.Done();
}

struct InputState {
	Input Snapshot{};
	uint64_t Pressed = 0, Released = 0;
	uint8_t MousePressed = 0, MouseReleased = 0;
	int32_t MouseDX = 0, MouseDY = 0, WheelDelta = 0;
};
class InputReceiver {
public:
	bool Push(const Input& input, uint64_t now) {
		if (m_HaveInput && !Newer(input.Sequence, m_State.Snapshot.Sequence)) return false;
		for (size_t i = 0; i < InputCount; ++i) {
			if (Changed(input.Presses[i], m_State.Snapshot.Presses[i])) m_State.Pressed |= uint64_t(1) << i;
			if (Changed(input.Releases[i], m_State.Snapshot.Releases[i])) m_State.Released |= uint64_t(1) << i;
		}
		for (size_t i = 0; i < 3; ++i) {
			if (Changed(input.MousePresses[i], m_State.Snapshot.MousePresses[i])) m_State.MousePressed |= uint8_t(1 << i);
			if (Changed(input.MouseReleases[i], m_State.Snapshot.MouseReleases[i])) m_State.MouseReleased |= uint8_t(1 << i);
		}
		m_State.MouseDX += Delta(input.MouseX, m_State.Snapshot.MouseX, 2048);
		m_State.MouseDY += Delta(input.MouseY, m_State.Snapshot.MouseY, 2048);
		m_State.WheelDelta += Delta(input.Wheel, m_State.Snapshot.Wheel, 16);
		m_State.Snapshot = input; m_LastTime = now; m_HaveInput = true; return true;
	}
	InputState Consume(uint64_t now) {
		if (m_HaveInput && now - m_LastTime > InputTimeoutMS) ReleaseControls();
		InputState result = m_State;
		m_AppliedInput = result.Snapshot;
		m_State.Pressed = m_State.Released = 0; m_State.MousePressed = m_State.MouseReleased = 0;
		m_State.MouseDX = m_State.MouseDY = m_State.WheelDelta = 0; return result;
	}
	// Same-match reconnects retain sequence and cumulative counters so delayed
	// datagrams cannot replay earlier motion or presses.
	void ReleaseControls() {
		m_State.Pressed = 0; m_State.MousePressed = 0; m_State.MouseDX = m_State.MouseDY = m_State.WheelDelta = 0;
		m_State.Released |= m_State.Snapshot.Held; m_State.MouseReleased |= m_State.Snapshot.MouseHeld;
		m_State.Snapshot.Held = 0; m_State.Snapshot.MouseHeld = 0;
		m_State.Snapshot.AimX = m_State.Snapshot.AimY = m_State.Snapshot.MoveX = m_State.Snapshot.MoveY = 0;
		m_State.Snapshot.ViewValid = m_State.Snapshot.CursorValid = m_State.Snapshot.PointerValid = false;
	}
	void Reset() { *this = InputReceiver(); }
	uint32_t LastSequence() const { return m_State.Snapshot.Sequence; }
	// Presentation must acknowledge only input that reached a simulation step.
	const Input& LastInput() const { return m_AppliedInput; }
private:
	static bool Changed(uint16_t value, uint16_t previous) { const uint16_t delta = value - previous; return delta != 0 && delta <= 64; }
	static int32_t Delta(uint32_t value, uint32_t previous, int32_t limit) { return std::clamp(std::bit_cast<int32_t>(value - previous), -limit, limit); }
	InputState m_State{};
	Input m_AppliedInput{};
	uint64_t m_LastTime = 0;
	bool m_HaveInput = false;
};

} // namespace RTE::MP
