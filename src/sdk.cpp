#include "game_rooms/sdk.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace game_rooms {
namespace {

bool is_json_whitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

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
          return parse_number();
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
      if (static_cast<unsigned char>(ch) < 0x20) {
        throw ProtocolError(ErrorCode::protocol_error, "Unescaped control character in JSON string");
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
        case 'u':
          append_codepoint(out, parse_codepoint());
          break;
        default:
          throw ProtocolError(ErrorCode::protocol_error, "Unsupported JSON escape");
      }
    }
    throw ProtocolError(ErrorCode::protocol_error, "Unterminated JSON string");
  }

  Json parse_number() {
    const std::size_t start = pos_;
    if (input_[pos_] == '-') {
      ++pos_;
    }

    if (pos_ >= input_.size()) {
      throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
    }

    if (input_[pos_] == '0') {
      ++pos_;
      if (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
      }
    } else if (std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    } else {
      throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
    }

    bool is_integer = true;
    if (pos_ < input_.size() && input_[pos_] == '.') {
      is_integer = false;
      ++pos_;
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
      }
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      is_integer = false;
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
      }
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }

    const auto token = std::string(input_.substr(start, pos_ - start));

    if (!is_integer) {
      try {
        const auto value = std::stod(token);
        if (!std::isfinite(value)) {
          throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
        }
        return Json(value);
      } catch (const ProtocolError&) {
        throw;
      } catch (const std::exception&) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
      }
    }

    try {
      if (!token.empty() && token.front() == '-') {
        return Json(static_cast<std::int64_t>(std::stoll(token)));
      }

      const auto value = std::stoull(token);
      if (value <= static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
        return Json(static_cast<std::int64_t>(value));
      }
      return Json(static_cast<std::uint64_t>(value));
    } catch (const std::exception&) {
      throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON number");
    }
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

  std::uint32_t parse_codepoint() {
    auto codepoint = read_hex4();
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
      if (pos_ + 1 >= input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON surrogate pair");
      }
      pos_ += 2;
      const auto low = read_hex4();
      if (low < 0xDC00 || low > 0xDFFF) {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON surrogate pair");
      }
      return 0x10000u + ((codepoint - 0xD800u) << 10u) + (low - 0xDC00u);
    }
    if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
      throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON surrogate pair");
    }
    return codepoint;
  }

  std::uint32_t read_hex4() {
    if (pos_ + 4 > input_.size()) {
      throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON unicode escape");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = input_[pos_++];
      value <<= 4u;
      if (ch >= '0' && ch <= '9') {
        value |= static_cast<std::uint32_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        value |= static_cast<std::uint32_t>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        value |= static_cast<std::uint32_t>(ch - 'A' + 10);
      } else {
        throw ProtocolError(ErrorCode::protocol_error, "Invalid JSON unicode escape");
      }
    }
    return value;
  }

  static void append_codepoint(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
      return;
    }
    if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
      return;
    }
    if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
      return;
    }
    out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  }

  void skip_ws() {
    while (pos_ < input_.size() && is_json_whitespace(input_[pos_])) {
      ++pos_;
    }
  }

  std::string_view input_;
  std::size_t pos_{0};
};

std::string escape_json(const std::string& value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
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
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u" << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch));
        } else {
          out << ch;
        }
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

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string trim(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  auto begin = std::find_if_not(value.begin(), value.end(), is_space);
  auto end = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

std::string read_fd(int fd) {
  std::string output;
  char buffer[4096];
  while (true) {
    const ssize_t read_count = ::read(fd, buffer, sizeof(buffer));
    if (read_count > 0) {
      output.append(buffer, static_cast<std::size_t>(read_count));
      continue;
    }
    if (read_count == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    throw ProtocolError(ErrorCode::transport_error, std::strerror(errno));
  }
  return output;
}

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw ProtocolError(ErrorCode::transport_error, "Failed to read default transport headers");
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

std::string find_last_status_block(const std::string& headers_text) {
  std::size_t cursor = 0;
  std::string last_block;

  while (cursor < headers_text.size()) {
    const auto block_end = headers_text.find("\r\n\r\n", cursor);
    if (block_end == std::string::npos) {
      break;
    }
    const auto block = headers_text.substr(cursor, block_end - cursor);
    if (block.rfind("HTTP/", 0) == 0) {
      last_block = block;
    }
    cursor = block_end + 4;
  }

  if (last_block.empty()) {
    throw ProtocolError(ErrorCode::transport_error, "Missing HTTP headers in default transport response");
  }
  return last_block;
}

HttpResponse parse_http_response(const std::string& headers_text, std::string body) {
  std::istringstream header_stream(find_last_status_block(headers_text));

  std::string status_line;
  if (!std::getline(header_stream, status_line)) {
    throw ProtocolError(ErrorCode::transport_error, "Missing HTTP status line");
  }
  status_line = trim(status_line);
  if (!status_line.empty() && status_line.back() == '\r') {
    status_line.pop_back();
  }

  std::istringstream status_stream(status_line);
  std::string http_version;
  int status_code = 0;
  status_stream >> http_version >> status_code;
  if (http_version.rfind("HTTP/", 0) != 0 || status_code <= 0) {
    throw ProtocolError(ErrorCode::transport_error, "Invalid HTTP status line from default transport");
  }

  std::map<std::string, std::string> headers;
  for (std::string line; std::getline(header_stream, line);) {
    line = trim(line);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      continue;
    }
    auto key = to_lower(trim(line.substr(0, separator)));
    auto value = trim(line.substr(separator + 1));
    if (!key.empty()) {
      headers[std::move(key)] = std::move(value);
    }
  }

  return HttpResponse{status_code, std::move(headers), std::move(body)};
}

HttpResponse perform_curl_request(const HttpRequest& request) {
  const auto method = upper(request.method);
  if (method.empty()) {
    throw ProtocolError(ErrorCode::transport_error, "HTTP method cannot be empty");
  }

  std::vector<std::string> args{
      "curl",
      "--silent",
      "--show-error",
      "--location",
      "--request",
      method,
      "--url",
      request.path,
  };
  for (const auto& [name, value] : request.headers) {
    args.push_back("--header");
    args.push_back(name + ": " + value);
  }
  if (request.body.has_value()) {
    args.push_back("--data-binary");
    args.push_back(*request.body);
  }

  char header_file_template[] = "/tmp/game-rooms-cpp-sdk-curl-headers-XXXXXX";
  const int header_fd = ::mkstemp(header_file_template);
  if (header_fd < 0) {
    throw ProtocolError(ErrorCode::transport_error, "Failed to create default transport header file");
  }
  ::close(header_fd);
  const std::string header_file_path(header_file_template);
  args.push_back("--dump-header");
  args.push_back(header_file_path);

  int stdout_pipe[2];
  if (::pipe(stdout_pipe) != 0) {
    ::unlink(header_file_path.c_str());
    throw ProtocolError(ErrorCode::transport_error, "Failed to initialize default transport pipes");
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    ::unlink(header_file_path.c_str());
    throw ProtocolError(ErrorCode::transport_error, "Failed to initialize default transport process");
  }

  if (pid == 0) {
    if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || ::dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
      _exit(126);
    }

    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execvp("curl", argv.data());
    _exit(127);
  }

  ::close(stdout_pipe[1]);

  std::string stdout_text;
  try {
    stdout_text = read_fd(stdout_pipe[0]);
  } catch (...) {
    ::close(stdout_pipe[0]);
    throw;
  }
  ::close(stdout_pipe[0]);

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    ::unlink(header_file_path.c_str());
    throw ProtocolError(ErrorCode::transport_error, "Failed waiting for default transport process");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ::unlink(header_file_path.c_str());
    throw ProtocolError(ErrorCode::transport_error,
                        stdout_text.empty() ? "Default HTTP transport failed" : trim(stdout_text));
  }

  std::string headers_text;
  try {
    headers_text = read_file(header_file_path);
  } catch (...) {
    ::unlink(header_file_path.c_str());
    throw;
  }
  ::unlink(header_file_path.c_str());
  return parse_http_response(headers_text, std::move(stdout_text));
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
bool Json::is_number() const {
  return std::holds_alternative<std::int64_t>(value_) || std::holds_alternative<std::uint64_t>(value_) ||
         std::holds_alternative<double>(value_);
}
bool Json::is_string() const { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const { return std::holds_alternative<JsonArray>(value_); }
bool Json::is_object() const { return std::holds_alternative<JsonObject>(value_); }

bool Json::as_bool() const { return std::get<bool>(value_); }
std::int64_t Json::as_int64() const {
  if (std::holds_alternative<std::int64_t>(value_)) {
    return std::get<std::int64_t>(value_);
  }
  if (std::holds_alternative<std::uint64_t>(value_)) {
    const auto value = std::get<std::uint64_t>(value_);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::bad_variant_access();
    }
    return static_cast<std::int64_t>(value);
  }
  return static_cast<std::int64_t>(std::get<double>(value_));
}
std::uint64_t Json::as_uint64() const {
  if (std::holds_alternative<std::uint64_t>(value_)) {
    return std::get<std::uint64_t>(value_);
  }
  if (std::holds_alternative<std::int64_t>(value_)) {
    const auto value = std::get<std::int64_t>(value_);
    if (value < 0) {
      throw std::bad_variant_access();
    }
    return static_cast<std::uint64_t>(value);
  }
  const auto value = std::get<double>(value_);
  if (value < 0) {
    throw std::bad_variant_access();
  }
  return static_cast<std::uint64_t>(value);
}
double Json::as_number() const {
  if (std::holds_alternative<std::int64_t>(value_)) {
    return static_cast<double>(std::get<std::int64_t>(value_));
  }
  if (std::holds_alternative<std::uint64_t>(value_)) {
    return static_cast<double>(std::get<std::uint64_t>(value_));
  }
  return std::get<double>(value_);
}
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
    if (std::holds_alternative<std::int64_t>(value_)) {
      out << std::get<std::int64_t>(value_);
    } else if (std::holds_alternative<std::uint64_t>(value_)) {
      out << std::get<std::uint64_t>(value_);
    } else {
      out << std::setprecision(15) << std::get<double>(value_);
    }
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

HttpApi::HttpApi(std::string base_url, Transport transport)
    : base_url_(normalize_base_url(std::move(base_url))), transport_(std::move(transport)) {
  if (!transport_) {
    transport_ = [](const HttpRequest& request) { return perform_curl_request(request); };
  }
}

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
      std::optional<std::string>(payload.dump()),
  };
}

HttpRequest HttpApi::build_app_config_request(const std::string& app_id) const {
  return HttpRequest{
      "GET",
      app_config_url(app_id),
      {},
      {},
  };
}

HttpRequest HttpApi::build_room_lookup_request(const std::string& code) const {
  return HttpRequest{
      "GET",
      room_lookup_url(code),
      {},
      {},
  };
}

HttpResponse HttpApi::create_room(const Json& payload) const {
  return execute(build_create_room_request(payload));
}

HttpResponse HttpApi::fetch_app_config(const std::string& app_id) const {
  return execute(build_app_config_request(app_id));
}

HttpResponse HttpApi::lookup_room(const std::string& code) const {
  return execute(build_room_lookup_request(code));
}

HttpResponse HttpApi::execute(const HttpRequest& request) const {
  if (!transport_) {
    throw ProtocolError(ErrorCode::transport_error, "HTTP transport is not configured");
  }
  return transport_(request);
}

void HttpApi::set_transport(Transport transport) {
  if (transport) {
    transport_ = std::move(transport);
    return;
  }
  transport_ = [](const HttpRequest& request) { return perform_curl_request(request); };
}

std::optional<ProtocolError> HttpApi::classify_error(const HttpResponse& response) {
  return classify_error_code(response.status_code, response.body);
}

std::string ProtocolCodec::encode(const ClientEnvelope& message) {
  return Json::object({
      {"opcode", message.opcode},
      {"seq", Json(message.seq)},
      {"params", message.params},
  }).dump();
}

std::string ProtocolCodec::encode(const ServerEnvelope& message) {
  JsonObject payload{
      {"opcode", message.opcode},
      {"result", message.result},
  };
  if (message.pc.has_value()) {
    payload.emplace("pc", Json(*message.pc));
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
      object.at("seq").as_uint64(),
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
    envelope.pc = it->second.as_uint64();
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
  const auto type = type_hint.empty() ? "object" : type_hint;
  return make_request(type + "/lock", Json::object({{"key", key}}));
}

ClientEnvelope Session::relay_to_host(const Json& payload) {
  return make_request("host/relay", Json::object({{"payload", payload}}));
}

ClientEnvelope Session::relay_to_player(const std::string& player_id, const Json& payload) {
  return make_request("player/relay", Json::object({{"playerId", player_id}, {"payload", payload}}));
}

}  // namespace game_rooms
