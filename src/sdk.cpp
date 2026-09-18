#include "game_rooms/sdk.hpp"

#include <iomanip>
#include <sstream>
#include <string_view>

namespace game_rooms {
namespace {

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  Json parse() {
    auto value = parse_value();
    skip_ws();
    if (pos_ != input_.size()) {
      throw ProtocolError(ErrorCode::protocol_error, "Unexpected trailing JSON content");
    }
    return value;
  }

 private:
  Json parse_value() {
    skip_ws();
    if (pos_ >= input_.size()) {
      throw ProtocolError(ErrorCode::protocol_error, "Unexpected end of JSON input");
    }

    switch (input_[pos_]) {
      case 'n':
        consume("null");
        return Json();
      case 't':
        consume("true");
        return Json(true);
      case 'f':
        consume("false");
        return Json(false);
      case '"':
        return Json(parse_string());
      case '[':
        return Json::array(parse_array());
      case '{':
        return Json::object(parse_object());
      default:
        if (input_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
          return Json(parse_number());
        }
        throw ProtocolError(ErrorCode::protocol_error, "Unexpected JSON token");
    }
  }

  JsonArray parse_array() {
    expect('[');
    JsonArray array;
    skip_ws();
    if (peek(']')) {
      ++pos_;
      return array;
    }

    while (true) {
      array.push_back(parse_value());
      skip_ws();
      if (peek(']')) {
        ++pos_;
        return array;
      }
      expect(',');
    }
  }

  JsonObject parse_object() {
    expect('{');
    JsonObject object;
    skip_ws();
    if (peek('}')) {
      ++pos_;
      return object;
    }

    while (true) {
      skip_ws();
      const auto key = parse_string();
      skip_ws();
      expect(':');
      object.emplace(key, parse_value());
      skip_ws();
      if (peek('}')) {
        ++pos_;
        return object;
      }
      expect(',');
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (pos_ < input_.size()) {
      const char ch = input_[pos_++];
      if (ch == '"') {
        return out;
      }
      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }

      if (pos_ >= input_.size()) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON escape");
      }

      switch (const char escaped = input_[pos_++]) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          throw ProtocolError(ErrorCode::protocol_error, "Unsupported JSON escape");
      }
    }
    throw ProtocolError(ErrorCode::protocol_error, "Unterminated JSON string");
  }

  double parse_number() {
    const std::size_t start = pos_;
    if (input_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
        ++pos_;
      }
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }
    return std::stod(std::string(input_.substr(start, pos_ - start)));
  }

  void consume(std::string_view token) {
    if (input_.substr(pos_, token.size()) != token) {
      throw ProtocolError(ErrorCode::protocol_error, "Unexpected JSON token");
    }
    pos_ += token.size();
  }

  void expect(char expected) {
    skip_ws();
    if (pos_ >= input_.size() || input_[pos_] != expected) {
      throw ProtocolError(ErrorCode::protocol_error, "Unexpected JSON delimiter");
    }
    ++pos_;
  }

  bool peek(char expected) const {
    return pos_ < input_.size() && input_[pos_] == expected;
  }

  void skip_ws() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }

  std::string_view input_;
  std::size_t pos_{0};
};

std::string escape_json(const std::string& value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  return out.str();
}

std::string url_encode(std::string_view value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out << static_cast<char>(ch);
    } else {
      out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
    }
  }
  return out.str();
}

std::string upper(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string normalize_base_url(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  return base_url;
}

std::optional<std::string> extract_error_text(const std::string& body) {
  if (body.empty()) {
    return std::nullopt;
  }

  try {
    const auto json = Json::parse(body);
    if (json.is_object()) {
      const auto& object = json.as_object();
      for (const char* key : {"error", "code", "message"}) {
        auto it = object.find(key);
        if (it != object.end() && it->second.is_string()) {
          return it->second.as_string();
        }
      }
    }
  } catch (const ProtocolError&) {
  }

  return body;
}

std::optional<ProtocolError> classify_error_code(int status, const std::string& body) {
  const auto text = extract_error_text(body);
  const auto matches = [&](std::string_view needle) {
    return text && text->find(std::string(needle)) != std::string::npos;
  };

  if (status == 404 || matches("room-not-found") || matches("ROOM_NOT_FOUND")) {
    return ProtocolError(ErrorCode::room_not_found, "Room not found", status);
  }
  if (status == 423 || matches("room-locked") || matches("ROOM_LOCKED")) {
    return ProtocolError(ErrorCode::room_locked, "Room locked", status);
  }
  if (status == 409 || matches("room-full") || matches("ROOM_FULL")) {
    return ProtocolError(ErrorCode::room_full, "Room full", status);
  }
  if (status >= 400) {
    return ProtocolError(ErrorCode::bad_request, text.value_or("Request failed"), status);
  }
  return std::nullopt;
}

JsonObject message_object(const std::string& key, const Json& value) {
  return JsonObject{{"key", key}, {"value", value}};
}

}  // namespace

Json Json::parse(const std::string& input) {
  return JsonParser(input).parse();
}

bool Json::is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const { return std::holds_alternative<bool>(value_); }
bool Json::is_number() const { return std::holds_alternative<double>(value_); }
bool Json::is_string() const { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const { return std::holds_alternative<JsonArray>(value_); }
bool Json::is_object() const { return std::holds_alternative<JsonObject>(value_); }

bool Json::as_bool() const { return std::get<bool>(value_); }
double Json::as_number() const { return std::get<double>(value_); }
const std::string& Json::as_string() const { return std::get<std::string>(value_); }
const JsonArray& Json::as_array() const { return std::get<JsonArray>(value_); }
const JsonObject& Json::as_object() const { return std::get<JsonObject>(value_); }
JsonArray& Json::as_array() { return std::get<JsonArray>(value_); }
JsonObject& Json::as_object() { return std::get<JsonObject>(value_); }

const Json& Json::at(const std::string& key) const {
  return as_object().at(key);
}

Json& Json::operator[](const std::string& key) {
  return as_object()[key];
}

std::string Json::dump() const {
  if (is_null()) {
    return "null";
  }
  if (is_bool()) {
    return as_bool() ? "true" : "false";
  }
  if (is_number()) {
    std::ostringstream out;
    out << std::setprecision(15) << as_number();
    return out.str();
  }
  if (is_string()) {
    return "\"" + escape_json(as_string()) + "\"";
  }
  if (is_array()) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& item : as_array()) {
      if (!first) {
        out << ",";
      }
      first = false;
      out << item.dump();
    }
    out << "]";
    return out.str();
  }

  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [key, value] : as_object()) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\"" << escape_json(key) << "\":" << value.dump();
  }
  out << "}";
  return out.str();
}

HttpApi::HttpApi(std::string base_url) : base_url_(normalize_base_url(std::move(base_url))) {}

std::string HttpApi::create_room_url() const {
  return base_url_ + "/rooms";
}

std::string HttpApi::app_config_url(const std::string& app_id) const {
  return base_url_ + "/apps/" + url_encode(app_id) + "/config";
}

std::string HttpApi::room_lookup_url(const std::string& code) const {
  return base_url_ + "/rooms/code/" + upper(code);
}

HttpRequest HttpApi::build_create_room_request(const Json& payload) const {
  return HttpRequest{
      "POST",
      create_room_url(),
      {{"content-type", "application/json"}},
      payload.dump(),
  };
}

HttpRequest HttpApi::build_app_config_request(const std::string& app_id) const {
  return HttpRequest{
      "GET",
      app_config_url(app_id),
      {},
      std::nullopt,
  };
}

HttpRequest HttpApi::build_room_lookup_request(const std::string& code) const {
  return HttpRequest{
      "GET",
      room_lookup_url(code),
      {},
      std::nullopt,
  };
}

std::optional<ProtocolError> HttpApi::classify_error(const HttpResponse& response) {
  return classify_error_code(response.status_code, response.body);
}

std::string ProtocolCodec::encode(const ClientEnvelope& message) {
  return Json::object({
      {"opcode", message.opcode},
      {"seq", static_cast<double>(message.seq)},
      {"params", message.params},
  }).dump();
}

std::string ProtocolCodec::encode(const ServerEnvelope& message) {
  JsonObject payload{
      {"opcode", message.opcode},
      {"result", message.result},
  };
  if (message.pc.has_value()) {
    payload.emplace("pc", static_cast<double>(*message.pc));
  }
  if (message.re.has_value()) {
    payload.emplace("re", *message.re);
  }
  return Json::object(std::move(payload)).dump();
}

ClientEnvelope ProtocolCodec::decode_client(const std::string& payload) {
  const auto json = Json::parse(payload);
  const auto& object = json.as_object();
  return ClientEnvelope{
      object.at("opcode").as_string(),
      static_cast<std::uint64_t>(object.at("seq").as_number()),
      object.at("params"),
  };
}

ServerEnvelope ProtocolCodec::decode_server(const std::string& payload) {
  const auto json = Json::parse(payload);
  const auto& object = json.as_object();

  ServerEnvelope envelope{
      std::nullopt,
      object.at("opcode").as_string(),
      object.at("result"),
      std::nullopt,
  };

  if (const auto it = object.find("pc"); it != object.end()) {
    envelope.pc = static_cast<std::uint64_t>(it->second.as_number());
  }
  if (const auto it = object.find("re"); it != object.end()) {
    envelope.re = it->second;
  }

  return envelope;
}

Session::Session(Role role) : role_(role) {}

ClientEnvelope Session::make_request(std::string opcode, Json params) {
  return ClientEnvelope{std::move(opcode), next_seq_++, std::move(params)};
}

std::string Session::object_type_for(const Json& value) {
  if (value.is_string()) {
    return "text";
  }
  if (value.is_number()) {
    return "number";
  }
  return "object";
}

ClientEnvelope Session::create_object(const std::string& key, const Json& value) {
  return make_request(object_type_for(value) + "/create", Json::object(message_object(key, value)));
}

ClientEnvelope Session::update_object(const std::string& key, const Json& value) {
  return make_request(object_type_for(value) + "/update", Json::object(message_object(key, value)));
}

ClientEnvelope Session::get_object(const std::string& key, const std::string& type_hint) {
  const auto type = type_hint.empty() ? "object" : type_hint;
  return make_request(type + "/get", Json::object({{"key", key}}));
}

ClientEnvelope Session::lock_object(const std::string& key, const std::string& type_hint) {
  return make_request(type_hint + "/lock", Json::object({{"key", key}}));
}

ClientEnvelope Session::relay_to_host(const Json& payload) {
  return make_request("host/relay", Json::object({{"payload", payload}}));
}

ClientEnvelope Session::relay_to_player(const std::string& player_id, const Json& payload) {
  return make_request("player/relay", Json::object({{"playerId", player_id}, {"payload", payload}}));
}

}  // namespace game_rooms
