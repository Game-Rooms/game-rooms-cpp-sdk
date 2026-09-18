#include "game_rooms/sdk.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class ScopeExit {
 public:
  explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
  ~ScopeExit() { fn_(); }

 private:
  std::function<void()> fn_;
};

}  // namespace

int main() {
  using namespace game_rooms;

  {
    const auto value =
        Json::parse(R"({"flag":true,"message":"ready \u2603","nested":{"round":1},"list":[1,2,3]})");
    require(value.at("flag").as_bool(), "expected bool field");
    require(value.at("nested").at("round").as_number() == 1.0, "expected nested number");
    require(Json::parse(value.dump()) == value, "expected JSON round trip");

    const Json control(std::string("\x01", 1));
    require(control.dump() == "\"\\u0001\"", "expected control character escaping");
    require(Json::parse(control.dump()) == control, "expected control character round trip");

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

    bool non_finite_rejected = false;
    try {
      static_cast<void>(Json::parse("1e999"));
    } catch (const ProtocolError&) {
      non_finite_rejected = true;
    }
    require(non_finite_rejected, "expected non-finite JSON number rejection");
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
    std::vector<HttpRequest> seen_requests;
    HttpApi api("https://example.com", [&](const HttpRequest& request) {
      seen_requests.push_back(request);
      return HttpResponse{200, {{"content-type", "application/json"}}, R"({"ok":true})"};
    });

    const auto create = api.create_room(Json::object({{"appId", "fibbage"}}));
    require(create.status_code == 200, "expected custom transport status");
    require(seen_requests.size() == 1, "expected one custom transport request");
    require(seen_requests.front().path == "https://example.com/rooms", "expected create-room request path");

    api.set_transport([&](const HttpRequest& request) {
      seen_requests.push_back(request);
      return HttpResponse{201, {}, ""};
    });
    const auto config = api.fetch_app_config("drawful");
    require(config.status_code == 201, "expected replaced transport status");
    require(seen_requests.size() == 2, "expected two custom transport requests");
    require(seen_requests.back().path == "https://example.com/apps/drawful/config", "expected app-config request path");
  }

  {
    const char* original_path = std::getenv("PATH");
    std::string path_backup = original_path == nullptr ? "" : original_path;

    char temp_dir_template[] = "/tmp/game-rooms-cpp-sdk-tests-XXXXXX";
    char* temp_dir = ::mkdtemp(temp_dir_template);
    require(temp_dir != nullptr, "expected temp dir for default transport test");

    const std::string curl_script_path = std::string(temp_dir) + "/curl";
    {
      std::ofstream script(curl_script_path);
      script << "#!/bin/sh\n";
      script << "header_file=\"\"\n";
      script << "while [ \"$#\" -gt 0 ]; do\n";
      script << "  if [ \"$1\" = \"--dump-header\" ]; then\n";
      script << "    shift\n";
      script << "    header_file=\"$1\"\n";
      script << "  fi\n";
      script << "  shift\n";
      script << "done\n";
      script << "printf 'HTTP/1.1 200 OK\\r\\nContent-Type: application/json\\r\\n\\r\\n' > \"$header_file\"\n";
      script << "printf '{\"ok\":true}'\n";
      script << "exit 0\n";
    }
    require(::chmod(curl_script_path.c_str(), 0700) == 0, "expected executable curl stub");

    const std::string test_path = std::string(temp_dir) + (path_backup.empty() ? "" : ":" + path_backup);
    require(::setenv("PATH", test_path.c_str(), 1) == 0, "expected PATH override for curl stub");
    ScopeExit cleanup([&] {
      ::setenv("PATH", path_backup.c_str(), 1);
      ::unlink(curl_script_path.c_str());
      ::rmdir(temp_dir);
    });

    HttpApi api("https://example.com");
    const auto response = api.fetch_app_config("drawful");
    require(response.status_code == 200, "expected default transport status");
    require(response.body == R"({"ok":true})", "expected default transport body");
    require(response.headers.at("content-type") == "application/json", "expected default transport header");

    bool empty_method_rejected = false;
    try {
      static_cast<void>(api.execute(HttpRequest{"", "https://example.com", {}, {}}));
    } catch (const ProtocolError& error) {
      empty_method_rejected = error.code() == ErrorCode::transport_error;
    }
    require(empty_method_rejected, "expected empty-method rejection in default transport");
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
