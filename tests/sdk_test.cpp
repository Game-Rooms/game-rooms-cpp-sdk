#include "game_rooms/sdk.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using namespace game_rooms;

  {
    const auto value =
        Json::parse(R"({"flag":true,"message":"ready \u2603","nested":{"round":1},"list":[1,2,3]})");
    require(value.at("flag").as_bool(), "expected bool field");
    require(value.at("nested").at("round").as_number() == 1.0, "expected nested number");
    require(Json::parse(value.dump()) == value, "expected JSON round trip");

    bool rejected = false;
    try {
      static_cast<void>(Json::parse("{\"bad\":\"line\nbreak\"}"));
    } catch (const ProtocolError&) {
      rejected = true;
    }
    require(rejected, "expected raw control character rejection");

    for (const auto* invalid : {"-", "1.", "1e"}) {
      bool invalid_rejected = false;
      try {
        static_cast<void>(Json::parse(invalid));
      } catch (const ProtocolError&) {
        invalid_rejected = true;
      }
      require(invalid_rejected, "expected invalid JSON number rejection");
    }
  }

  {
    const HttpApi api("https://example.com/");
    require(api.create_room_url() == "https://example.com/rooms", "expected room create URL");
    require(api.app_config_url("drawful 2") == "https://example.com/apps/drawful%202/config",
            "expected app config URL");
    require(api.room_lookup_url("abcz") == "https://example.com/rooms/code/ABCZ",
            "expected room lookup URL");

    const auto request = api.build_create_room_request(Json::object({{"appId", "fibbage"}}));
    require(request.method == "POST", "expected POST request");
    require(request.body.has_value(), "expected JSON request body");
  }

  {
    const auto error = HttpApi::classify_error(HttpResponse{404, {}, R"({"error":"room-not-found"})"});
    require(error.has_value() && error->code() == ErrorCode::room_not_found, "expected room-not-found");

    const auto locked = HttpApi::classify_error(HttpResponse{423, {}, ""});
    require(locked.has_value() && locked->code() == ErrorCode::room_locked, "expected room-locked");

    const auto full = HttpApi::classify_error(HttpResponse{409, {}, R"({"code":"ROOM_FULL"})"});
    require(full.has_value() && full->code() == ErrorCode::room_full, "expected room-full");
  }

  {
    Session session(Role::host);
    const auto create = session.create_object("state", Json::object({{"round", 1}}));
    require(create.seq == 1, "expected first sequence number");
    require(create.opcode == "object/create", "expected object/create");
    require(create.params.at("key").as_string() == "state", "expected create key");

    const auto update = session.update_object("prompt", Json("hello"));
    require(update.seq == 2, "expected second sequence number");
    require(update.opcode == "text/update", "expected text/update");

    const auto relay = session.relay_to_player("p1", Json::object({{"event", "sync"}}));
    require(relay.opcode == "player/relay", "expected player relay opcode");

    const auto lock = session.lock_object("state", "");
    require(lock.opcode == "object/lock", "expected default object lock opcode");
  }

  {
    constexpr std::uint64_t large_seq = 1844674407370955161ULL;
    const ClientEnvelope outbound{"number/update", large_seq, Json::object({{"key", "score"}, {"value", 2}})};
    const auto wire = ProtocolCodec::encode(outbound);
    const auto parsed = ProtocolCodec::decode_client(wire);
    require(parsed.opcode == outbound.opcode, "expected client opcode round trip");
    require(parsed.seq == outbound.seq, "expected client seq round trip");
    require(parsed.params.at("key").as_string() == "score", "expected client params round trip");

    const ServerEnvelope inbound{large_seq, "number/update", Json::object({{"ok", true}}), Json("ack")};
    const auto inbound_wire = ProtocolCodec::encode(inbound);
    const auto inbound_parsed = ProtocolCodec::decode_server(inbound_wire);
    require(inbound_parsed.pc.has_value() && *inbound_parsed.pc == large_seq, "expected server pc");
    require(inbound_parsed.re.has_value() && inbound_parsed.re->as_string() == "ack",
            "expected server re");
  }

  return 0;
}
