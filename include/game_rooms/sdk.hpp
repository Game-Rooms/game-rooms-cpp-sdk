#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace game_rooms {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::map<std::string, Json>;

class Json {
 public:
  using storage_type =
      std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, JsonArray, JsonObject>;

  Json() : value_(nullptr) {}
  Json(std::nullptr_t) : value_(nullptr) {}
  Json(bool value) : value_(value) {}
  Json(int value) : value_(static_cast<std::int64_t>(value)) {}
  Json(std::int64_t value) : value_(value) {}
  Json(std::uint64_t value) : value_(value) {}
  Json(double value) : value_(value) {}
  Json(const char* value) : value_(std::string(value)) {}
  Json(std::string value) : value_(std::move(value)) {}
  Json(JsonArray value) : value_(std::move(value)) {}
  Json(JsonObject value) : value_(std::move(value)) {}

  static Json array(JsonArray value) { return Json(std::move(value)); }
  static Json object(JsonObject value) { return Json(std::move(value)); }
  static Json parse(const std::string& input);

  bool is_null() const;
  bool is_bool() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  bool as_bool() const;
  std::int64_t as_int64() const;
  std::uint64_t as_uint64() const;
  double as_number() const;
  const std::string& as_string() const;
  const JsonArray& as_array() const;
  const JsonObject& as_object() const;
  JsonArray& as_array();
  JsonObject& as_object();

  const Json& at(const std::string& key) const;
  Json& operator[](const std::string& key);

  std::string dump() const;

  bool operator==(const Json& other) const { return value_ == other.value_; }
  bool operator!=(const Json& other) const { return !(*this == other); }

 private:
  storage_type value_;
};

enum class ErrorCode {
  bad_request,
  room_not_found,
  room_locked,
  room_full,
  protocol_error,
  transport_error
};

class ProtocolError : public std::runtime_error {
 public:
  ProtocolError(ErrorCode code, std::string message, int http_status = 0)
      : std::runtime_error(std::move(message)), code_(code), http_status_(http_status) {}

  ErrorCode code() const { return code_; }
  int http_status() const { return http_status_; }

 private:
  ErrorCode code_;
  int http_status_;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::optional<std::string> body;
};

struct HttpResponse {
  int status_code{};
  std::map<std::string, std::string> headers;
  std::string body;
};

class HttpApi {
 public:
  explicit HttpApi(std::string base_url);

  const std::string& base_url() const { return base_url_; }
  std::string create_room_url() const;
  std::string app_config_url(const std::string& app_id) const;
  std::string room_lookup_url(const std::string& code) const;

  HttpRequest build_create_room_request(const Json& payload) const;
  HttpRequest build_app_config_request(const std::string& app_id) const;
  HttpRequest build_room_lookup_request(const std::string& code) const;

  static std::optional<ProtocolError> classify_error(const HttpResponse& response);

 private:
  std::string base_url_;
};

enum class Role {
  host,
  player
};

struct ClientEnvelope {
  std::string opcode;
  std::uint64_t seq{};
  Json params;
};

struct ServerEnvelope {
  std::optional<std::uint64_t> pc;
  std::string opcode;
  Json result;
  std::optional<Json> re;
};

class ProtocolCodec {
 public:
  static std::string encode(const ClientEnvelope& message);
  static std::string encode(const ServerEnvelope& message);
  static ClientEnvelope decode_client(const std::string& payload);
  static ServerEnvelope decode_server(const std::string& payload);
};

class Session {
 public:
  explicit Session(Role role);

  Role role() const { return role_; }
  std::uint64_t next_sequence() const { return next_seq_; }

  ClientEnvelope make_request(std::string opcode, Json params);
  ClientEnvelope create_object(const std::string& key, const Json& value);
  ClientEnvelope update_object(const std::string& key, const Json& value);
  ClientEnvelope get_object(const std::string& key, const std::string& type_hint = "");
  ClientEnvelope lock_object(const std::string& key, const std::string& type_hint = "");
  ClientEnvelope relay_to_host(const Json& payload);
  ClientEnvelope relay_to_player(const std::string& player_id, const Json& payload);

  static std::string object_type_for(const Json& value);

 private:
  Role role_;
  std::uint64_t next_seq_{1};
};

}  // namespace game_rooms
