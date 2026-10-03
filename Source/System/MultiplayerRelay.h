#pragma once

#include "MultiplayerProtocol.h"
#include <cctype>

namespace RTE::MP::Relay {
inline constexpr uint8_t PacketID = 221;
inline constexpr uint32_t Magic = 0x4343524d;
inline constexpr uint16_t Version = 1;
inline constexpr size_t CodeLength = 10, TokenLength = 32, MaxPacket = 1430;
inline constexpr char Alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
// The deployed v1 broker recognizes the old game header before forwarding.
// Keep that transport envelope stable; the opaque body has its own game version.
inline constexpr uint16_t CarrierVersion = 2;
inline constexpr uint64_t CarrierCookie = 0x434352454c415931ULL;
inline constexpr size_t CarrierHeaderBytes = 20, MaxPayload = 1400;
inline bool ValidPayload(std::span<const uint8_t> bytes) {
 if (bytes.size() < CarrierHeaderBytes || bytes.size() > MaxPayload) return false;
 MP::Reader reader(bytes); uint8_t id, kind; uint32_t magic, epoch; uint16_t version; uint64_t session;
 return reader.U8(id) && reader.U32(magic) && reader.U16(version) && reader.U8(kind) && reader.U64(session) && reader.U32(epoch) && id == MP::PacketID && magic == MP::Magic && version != 0;
}
inline std::vector<uint8_t> WrapPayload(std::span<const uint8_t> bytes) {
 if (bytes.empty() || bytes.size() > MaxPayload - CarrierHeaderBytes) return {};
 MP::Writer writer(MP::Kind::Hello, CarrierCookie); writer.Data[5] = CarrierVersion >> 8; writer.Data[6] = CarrierVersion & 255;
 writer.Bytes(bytes); return std::move(writer.Data);
}
inline std::span<const uint8_t> UnwrapPayload(std::span<const uint8_t> bytes) {
 if (!ValidPayload(bytes)) return {};
 MP::Reader reader(bytes); uint8_t id, kind; uint32_t magic, epoch; uint16_t version; uint64_t session;
 reader.U8(id); reader.U32(magic); reader.U16(version); reader.U8(kind); reader.U64(session); reader.U32(epoch);
 if (version == CarrierVersion && kind == uint8_t(MP::Kind::Hello) && session == CarrierCookie && epoch == 0) return ValidPayload(reader.Rest()) ? reader.Rest() : std::span<const uint8_t>{};
 return bytes;
}
enum class Kind : uint8_t { Create, Join, Accepted, PeerUp, PeerDown, Route, Drop, Leave, Error };
class Writer : public MP::Writer {
public:
 explicit Writer(Kind kind): MP::Writer(MP::Kind::Hello) {
  Data.clear(); U8(PacketID); U32(Magic); U16(Version); U8(static_cast<uint8_t>(kind));
 }
};
inline bool Header(MP::Reader& reader, Kind& kind) {
 uint8_t id, value; uint32_t magic; uint16_t version;
 if (!reader.U8(id) || !reader.U32(magic) || !reader.U16(version) || !reader.U8(value) || id != PacketID || magic != Magic || version != Version || value > static_cast<uint8_t>(Kind::Error)) return false;
 kind = static_cast<Kind>(value); return true;
}
inline std::string NormalizeCode(const std::string& text) {
 std::string code;
 for (unsigned char c: text) { if (c == '-' || c == ' ') continue; code += static_cast<char>(std::toupper(c)); }
 if (code.size() != CodeLength || code.find_first_not_of(Alphabet) != std::string::npos) return {};
 return code;
}
inline std::string DisplayCode(const std::string& code) { return code.size() == CodeLength ? code.substr(0, 5) + "-" + code.substr(5) : code; }
inline std::string PeerAddress(uint8_t slot) { return "relay:" + std::to_string(slot); }
inline int PeerSlot(const std::string& address) { return address.size() == 7 && address.starts_with("relay:") && address[6] >= '0' && address[6] <= '3' ? address[6] - '0' : -1; }
} // namespace RTE::MP::Relay
