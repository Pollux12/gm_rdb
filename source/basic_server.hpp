#pragma once

#if __cplusplus < 201103L && !(defined(_MSC_VER) || _MSC_VER >= 1800)
#error Needs at least a C++11 compiler
#endif

#include <memory>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <tier1/strtools.h>
#include <utility>
#include <vector>

#include <lrdb/debugger.hpp>
#include <lrdb/message.hpp>

#include <picojson.h>

#include "console_adapter_logging.hpp"
#include "console_adapter_spew.hpp"
#ifndef GMOD_CLIENT_MODULE
#include "entity_query.hpp"
#include "entity_snapshot.hpp"
#endif
#include "error_aggregator.hpp"

#define LRDB_SERVER_PROTOCOL_VERSION "gmod-2"

#ifndef GM_RDB_VERSION
#define GM_RDB_VERSION "0.0.0"
#endif

namespace json
{
    using namespace ::picojson;
}

/// @brief Debug Server Class
/// template type is messaging communication customization point
/// require members
///  void close();			/// connection close
///  bool is_open() const; /// connection is opened
///  void poll();          /// polling event data. Require non blocking
///  void run_one();		/// run event data. Blocking until run one
///  message.
///  void wait_for_connection(); //Blocking until connection.
///  bool send_message(const std::string& message); /// send message to
///  communication opponent
///  //callback functions. Must that call inside poll or run_one
///  std::function<void(const std::string& data)> on_data;///callback for
///  receiving data.
///  std::function<void()> on_connection;
///  std::function<void()> on_close;
///  std::function<void(const std::string&)> on_error;
template <typename StreamType>
class basic_server {
 public:
  /// @brief constructor
  /// @param arg Forward to StreamType constructor
  template <typename... StreamArgs>
  basic_server(StreamArgs&&... arg)
      : wait_for_connect_(true),
        attached_(false),
        disposed_(false),
        has_active_connection_(false),
        pending_pause_on_error_(false),
        stop_on_error_(false),
        pause_on_activate_(false),
        lua_base_(nullptr),
        command_stream_(std::forward<StreamArgs>(arg)...) {
    init();
  }

  ~basic_server() { exit(); }

  /// @brief attach (or detach) for debug target
  /// @param lua_State*  debug target
  void reset(lua_State* L = nullptr,
             GarrysMod::Lua::ILuaBase* lua_base = nullptr) {
    attached_ = L != nullptr;
    disposed_ = false;
    pending_pause_on_error_.store(false);
    stop_on_error_ = false;
    error_aggregator_.clear();
    lua_base_ = L == nullptr ? nullptr : lua_base;
    debugger_.reset(L);
    if (!L) {
      wait_for_connect_ = true;
      debugger_.unpause();
      console_adapter_.set_callback(nullptr);
      command_stream_.reconnect();
    } else {
      console_adapter_.set_callback(std::bind(
          &basic_server<StreamType>::handle_console_output, this,
          std::placeholders::_1));
      if (pause_on_activate_) {
        debugger_.pause();
      }
    }
  }

  /// @brief Exit debug server
  void exit() {
    if (disposed_) {
      return;
    }
    disposed_ = true;
    attached_ = false;
    wait_for_connect_ = true;
    pending_pause_on_error_.store(false);
    error_aggregator_.clear();
    lua_base_ = nullptr;
    console_adapter_.set_callback(nullptr);
    debugger_.reset(nullptr);
    debugger_.unpause();
    command_stream_.on_connection = nullptr;
    command_stream_.on_data = nullptr;
    command_stream_.on_close = nullptr;
    command_stream_.on_error = nullptr;
    if (command_stream_.is_open()) {
      send_notify(lrdb::notify_message("exit"));
    }
    command_stream_.close();
  }

  StreamType& command_stream() { return command_stream_; };

  void set_pause_on_activate(bool pause_on_activate) {
    pause_on_activate_ = pause_on_activate;
  }

 private:
  void init() {
    debugger_.set_pause_handler([&](lrdb::debugger&) {
      if (disposed_) {
        return;
      }
      send_pause_status();
      while (debugger_.paused() && command_stream_.is_open()) {
        command_stream_.run_one();
      }
      send_notify(lrdb::notify_message("running"));
    });

    debugger_.set_tick_handler([&](lrdb::debugger&) {
      if (disposed_) {
        return;
      }
      if (pending_pause_on_error_.exchange(false) && attached_ &&
          !debugger_.paused()) {
        debugger_.pause_now();
      }
      if (wait_for_connect_) {
        if (!wait_for_connection_with_timeout()) {
          ++metrics_.connection_wait_timeouts;
          return;
        }
      }
      command_stream_.poll();
    });

    command_stream_.on_connection = [this]() { connected_done(); };
    command_stream_.on_data = [this](const std::string& data) {
      execute_message(data);
    };
    command_stream_.on_close = [this]() {
      if (has_active_connection_) {
        ++metrics_.connections_closed;
      }
      has_active_connection_ = false;
      wait_for_connect_ = true;
      debugger_.unpause();
    };
    command_stream_.on_error = [this](const std::string&) { ++metrics_.stream_errors; };
  }
  void send_pause_status() {
    json::object pauseparam;
    pauseparam["reason"] = json::value(debugger_.pause_reason());
    send_notify(lrdb::notify_message("paused", json::value(pauseparam)));
  }
  void connected_done() {
    if (disposed_) {
      command_stream_.close();
      return;
    }
    wait_for_connect_ = false;
    has_active_connection_ = true;
    ++metrics_.connections_opened;
    json::object param;
    param["protocol_version"] = json::value(LRDB_SERVER_PROTOCOL_VERSION);
    param["module_version"] = json::value(GM_RDB_VERSION);

    json::object lua;
    lua["version"] = json::value(LUA_VERSION);
    lua["release"] = json::value(LUA_RELEASE);
    lua["copyright"] = json::value(LUA_COPYRIGHT);

    param["lua"] = json::value(lua);
    send_notify(lrdb::notify_message("connected", json::value(param)));

    // If the debugger is already paused when a client connects, re-announce
    // the paused state so the adapter can correctly show pause UI.
    if (debugger_.paused()) {
      send_pause_status();
    }
  }

  bool send_message(const std::string& message) {
    if (disposed_) {
      return false;
    }
    bool sent = command_stream_.send_message(message);
    if (!sent) {
      ++metrics_.send_failures;
    }
    return sent;
  }
  void execute_message(const std::string& message) {
    json::value msg;
    std::string err = json::parse(msg, message);
    if (!err.empty()) {
      ++metrics_.parse_errors;
      send_protocol_error(lrdb::response_error::ParseError, "parse error",
                          json::value(), "parse",
                          error_data("parse", "message", err));
      return;
    }
    if (!lrdb::message::is_request(msg)) {
      ++metrics_.invalid_requests;
      send_protocol_error(lrdb::response_error::InvalidRequest, "invalid request",
                          lrdb::message::get_id(msg), "request");
      return;
    }
    lrdb::request_message request;
    if (!lrdb::message::parse(msg, request)) {
      ++metrics_.invalid_requests;
      send_protocol_error(lrdb::response_error::InvalidRequest, "invalid request",
                          lrdb::message::get_id(msg), "request");
      return;
    }
    execute_request(request);
  }

  bool send_notify(const lrdb::notify_message& message) {
    ++metrics_.notifications_sent;
    return send_message(lrdb::message::serialize(message));
  }

  bool wait_for_connection_with_timeout() {
    const auto start = std::chrono::steady_clock::now();
    while (wait_for_connect_ && !command_stream_.is_open()) {
      command_stream_.poll();
      if (command_stream_.is_open()) {
        return true;
      }
      if (std::chrono::steady_clock::now() - start >=
          std::chrono::milliseconds(kConnectionWaitTimeoutMs)) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return command_stream_.is_open();
  }
  bool send_response(lrdb::response_message& message) {
    if (message.error) {
      switch (message.error->code) {
        case lrdb::response_error::InvalidParams:
          ++metrics_.invalid_params_errors;
          break;
        case lrdb::response_error::MethodNotFound:
          ++metrics_.method_not_found_errors;
          break;
        case lrdb::response_error::InternalError:
          ++metrics_.internal_errors;
          break;
        default:
          break;
      }
    }
    ++metrics_.responses_sent;
    return send_message(lrdb::message::serialize(message));
  }

  static json::value error_data(const std::string& phase,
                                const std::string& category,
                                const std::string& detail = std::string()) {
    json::object data;
    data["phase"] = json::value(phase);
    data["category"] = json::value(category);
    if (!detail.empty()) {
      data["detail"] = json::value(detail);
    }
    return json::value(data);
  }

  void set_structured_error(lrdb::response_message& response, int code,
                            const std::string& message,
                            const std::string& method,
                            const json::value& detail = json::value()) {
    response.error = lrdb::response_error(code, message);
    json::object data = {
        {"phase", json::value("phase1")},
        {"method", method.empty() ? json::value() : json::value(method)}};
    if (!detail.is<json::null>()) {
      data["detail"] = detail;
    }
    response.error->data = json::value(data);
  }

  bool send_protocol_error(int code, const std::string& message,
                           const json::value& id, const std::string& method,
                           const json::value& detail = json::value()) {
    lrdb::response_message response;
    response.id = id;
    set_structured_error(response, code, message, method, detail);
    return send_response(response);
  }

  bool try_parse_non_negative_int(const json::value& value, int& out) {
    if (!value.is<double>()) {
      return false;
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(std::numeric_limits<int>::max())) {
      return false;
    }
    out = static_cast<int>(number);
    return static_cast<double>(out) == number;
  }

  bool try_parse_finite_number(const json::value& value, double& out) {
    if (!value.is<double>()) {
      return false;
    }
    out = value.get<double>();
    return std::isfinite(out);
  }

  bool try_parse_triplet(const json::value& value, double& first, double& second,
                         double& third) {
    if (!value.is<json::array>()) {
      return false;
    }

    const json::array& values = value.get<json::array>();
    if (values.size() != 3) {
      return false;
    }

    return try_parse_finite_number(values[0], first) &&
           try_parse_finite_number(values[1], second) &&
           try_parse_finite_number(values[2], third);
  }

  bool try_parse_depth(const json::value& param, int& depth) {
    if (!param.is<json::object>() ||
      param.get<json::object>().count("depth") == 0) {
      depth = 1;
      return true;
    }
    if (!try_parse_non_negative_int(param.get("depth"), depth)) {
      return false;
    }
    if (depth > kMaxObjectDepth) {
      depth = kMaxObjectDepth;
    }
    return true;
  }

  bool try_parse_stack_no(const json::value& param, int& stack_no) {
    if (!param.is<json::object>() ||
      param.get<json::object>().count("stack_no") == 0) {
      return false;
    }
    return try_parse_non_negative_int(param.get("stack_no"), stack_no);
  }

  bool ensure_attached(lrdb::response_message& response, const std::string& method) {
    if (attached_ && !disposed_) {
      return true;
    }
    set_structured_error(response, lrdb::response_error::ServerNotInitialized,
                         "debug session not attached", method,
                         error_data("session", "state", "detached"));
    return false;
  }

  bool ensure_paused(lrdb::response_message& response, const std::string& method) {
    if (debugger_.paused()) {
      return true;
    }
    set_structured_error(response, lrdb::response_error::InvalidRequest,
                         "debugger is not paused", method,
                         error_data("request", "state", "running"));
    return false;
  }

  bool ensure_lua_interface(lrdb::response_message& response,
                            const std::string& method) {
    if (lua_base_ != nullptr) {
      return true;
    }

    set_structured_error(response, lrdb::response_error::InternalError,
                         "lua interface unavailable", method,
                         error_data("session", "state",
                                    "lua_interface_unavailable"));
    return false;
  }

  bool extract_frame(lrdb::response_message& response, const std::string& method,
                     const json::value& param, int& stack_no, int& depth,
                     std::vector<lrdb::stack_info>& callstack) {
    if (!ensure_attached(response, method) || !ensure_paused(response, method)) {
      return false;
    }
    if (!param.is<json::object>() || !try_parse_stack_no(param, stack_no) ||
        !try_parse_depth(param, depth)) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", method,
                           error_data("request", "params"));
      return false;
    }
    callstack = debugger_.get_call_stack();
    if (stack_no >= static_cast<int>(callstack.size())) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid stack frame", method,
                           error_data("request", "stack_no"));
      return false;
    }
    return true;
  }

  static bool is_space(char ch) {
    return V_isspace(ch);
  }

  static bool is_valid_command_name_char(char ch) {
    return V_isalnum(ch) || ch == '_' || ch == '.' || ch == '+' ||
           ch == '-';
  }

  bool validate_console_command(const std::string& command,
                                std::string& reason) const {
    if (command.empty()) {
      reason = "empty command";
      return false;
    }
    if (command.size() > kMaxConsoleCommandBytes) {
      reason = "command exceeds size limit";
      return false;
    }

    for (const char ch : command) {
      const unsigned char value = static_cast<unsigned char>(ch);
      if (ch == '\0') {
        reason = "command contains null byte";
        return false;
      }
      if (ch == ';') {
        reason = "command chaining is not allowed";
        return false;
      }
      if (V_iscntrl(value) && ch != '\t' && ch != ' ') {
        reason = "command contains control characters";
        return false;
      }
    }

    size_t start = 0;
    while (start < command.size() && is_space(command[start])) {
      ++start;
    }
    if (start == command.size()) {
      reason = "empty command";
      return false;
    }

    size_t end = start;
    while (end < command.size() && !is_space(command[end])) {
      ++end;
    }
    const std::string command_name = command.substr(start, end - start);
    if (command_name.size() > kMaxConsoleCommandNameBytes) {
      reason = "command name exceeds size limit";
      return false;
    }

    for (const char ch : command_name) {
      if (!is_valid_command_name_char(ch)) {
        reason = "invalid command name";
        return false;
      }
    }

    return true;
  }

  static bool starts_with_case_insensitive(const std::string& value,
                                           const char* prefix) {
    if (prefix == nullptr) {
      return false;
    }

    const size_t prefix_len = std::strlen(prefix);
    if (value.size() < prefix_len) {
      return false;
    }

    for (size_t i = 0; i < prefix_len; ++i) {
      const char lhs = static_cast<char>(
          std::tolower(static_cast<unsigned char>(value[i])));
      const char rhs = static_cast<char>(
          std::tolower(static_cast<unsigned char>(prefix[i])));
      if (lhs != rhs) {
        return false;
      }
    }

    return value.size() == prefix_len || is_space(value[prefix_len]);
  }

  static bool should_force_console_adapter_dispatch(
      const std::string& command) {
    return starts_with_case_insensitive(command, "lua_run") ||
           starts_with_case_insensitive(command, "lua_run_cl") ||
           starts_with_case_insensitive(command, "lua_run_menu") ||
           starts_with_case_insensitive(command, "lua_openscript") ||
           starts_with_case_insensitive(command, "lua_openscript_cl") ||
           starts_with_case_insensitive(command, "lua_openscript_menu");
  }

  static bool try_parse_realm_value(const json::value& value,
                                    std::string& normalized_realm) {
    if (!value.is<std::string>()) {
      return false;
    }

    normalized_realm = value.get<std::string>();
    std::transform(normalized_realm.begin(), normalized_realm.end(),
                   normalized_realm.begin(), [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });

    return normalized_realm == "server" || normalized_realm == "client" ||
           normalized_realm == "shared";
  }

#ifndef GMOD_CLIENT_MODULE
  bool invoke_gluals_function(const char* function_name,
                              const std::vector<std::string>& args,
                              std::string& out_error) {
    if (lua_base_ == nullptr) {
      out_error = "lua interface unavailable";
      return false;
    }

    const int stack_top = lua_base_->Top();
    auto restore_stack = [&]() {
      const int pop_count = lua_base_->Top() - stack_top;
      if (pop_count > 0) {
        lua_base_->Pop(pop_count);
      }
    };

    lua_base_->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua_base_->GetField(-1, "_GLUALS");
    if (!lua_base_->IsType(-1, GarrysMod::Lua::Type::Table)) {
      restore_stack();
      out_error = "_GLUALS table unavailable (run debugger setup to install lua/autorun/debug.lua)";
      return false;
    }

    lua_base_->GetField(-1, function_name);
    if (!lua_base_->IsType(-1, GarrysMod::Lua::Type::Function)) {
      restore_stack();
      out_error = std::string("_GLUALS.") + function_name + " is unavailable";
      return false;
    }

    for (const std::string& arg : args) {
      lua_base_->PushString(arg.c_str());
    }

    if (lua_base_->PCall(static_cast<int>(args.size()), 2, 0) != 0) {
      const char* err = lua_base_->GetString(-1);
      out_error = err != nullptr ? std::string(err) : "unknown lua error";
      lua_base_->Pop(1);
      restore_stack();
      return false;
    }

    bool ok = true;
    if (lua_base_->IsType(-2, GarrysMod::Lua::Type::Bool)) {
      ok = lua_base_->GetBool(-2);
    }

    if (!ok) {
      if (lua_base_->IsType(-1, GarrysMod::Lua::Type::String)) {
        const char* err = lua_base_->GetString(-1);
        out_error = err != nullptr ? std::string(err)
                                   : std::string("bridge reported failure");
      } else {
        out_error = "bridge reported failure";
      }
      lua_base_->Pop(2);
      restore_stack();
      return false;
    }

    lua_base_->Pop(2);
    restore_stack();
    return true;
  }
#endif  // !GMOD_CLIENT_MODULE

#ifndef GMOD_CLIENT_MODULE
  bool run_lua_request(lrdb::response_message& response,
                       const json::value& param) {
    if (!ensure_attached(response, "run_lua") ||
        !ensure_lua_interface(response, "run_lua")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_lua",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    if (params.count("lua") == 0 || !params.at("lua").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_lua",
                           error_data("request", "lua"));
      return send_response(response);
    }

    std::string realm;
    if (params.count("realm") == 0 ||
        !try_parse_realm_value(params.at("realm"), realm)) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_lua",
                           error_data("request", "realm"));
      return send_response(response);
    }

    const std::string chunk = params.at("lua").get<std::string>();
    if (chunk.size() > kMaxRunLuaChunkBytes) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "lua chunk exceeds limit", "run_lua",
                           error_data("request", "chunk_size"));
      return send_response(response);
    }

    std::string bridge_error;
    if (!invoke_gluals_function("runLua", {realm, chunk}, bridge_error)) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "failed to execute lua", "run_lua",
                           error_data("request", "runtime", bridge_error));
    }

    return send_response(response);
  }

  bool run_file_request(lrdb::response_message& response,
                        const json::value& param) {
    if (!ensure_attached(response, "run_file") ||
        !ensure_lua_interface(response, "run_file")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_file",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    if (params.count("file") == 0 || !params.at("file").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_file",
                           error_data("request", "file"));
      return send_response(response);
    }

    std::string realm;
    if (params.count("realm") == 0 ||
        !try_parse_realm_value(params.at("realm"), realm)) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_file",
                           error_data("request", "realm"));
      return send_response(response);
    }

    const std::string file = params.at("file").get<std::string>();
    if (file.empty() || file.size() > kMaxLuaFilePathBytes) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "run_file",
                           error_data("request", "file"));
      return send_response(response);
    }

    std::string bridge_error;
    if (!invoke_gluals_function("runFile", {realm, file}, bridge_error)) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "failed to execute file", "run_file",
                           error_data("request", "runtime", bridge_error));
    }

    return send_response(response);
  }

  bool refresh_file_request(lrdb::response_message& response,
                            const json::value& param) {
    if (!ensure_attached(response, "refresh_file") ||
        !ensure_lua_interface(response, "refresh_file")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "refresh_file",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    if (params.count("file") == 0 || !params.at("file").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "refresh_file",
                           error_data("request", "file"));
      return send_response(response);
    }

    const std::string file = params.at("file").get<std::string>();
    if (file.empty() || file.size() > kMaxLuaFilePathBytes) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "refresh_file",
                           error_data("request", "file"));
      return send_response(response);
    }

    std::string bridge_error;
    if (!invoke_gluals_function("refreshFile", {file}, bridge_error)) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "failed to refresh file", "refresh_file",
                           error_data("request", "runtime", bridge_error));
    }

    return send_response(response);
  }
#endif  // !GMOD_CLIENT_MODULE

  bool step_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "step")) {
      return send_response(response);
    }
    debugger_.step();
    return send_response(response);
  }

  bool step_in_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "step_in")) {
      return send_response(response);
    }
    debugger_.step_in();
    return send_response(response);
  }
  bool step_out_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "step_out")) {
      return send_response(response);
    }
    debugger_.step_out();
    return send_response(response);
  }
  bool continue_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "continue")) {
      return send_response(response);
    }
    debugger_.unpause();
    return send_response(response);
  }
  bool pause_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "pause")) {
      return send_response(response);
    }
    debugger_.pause();
    return send_response(response);
  }
  bool pause_now_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "pause_now")) {
      return send_response(response);
    }
    debugger_.pause_now();
    return send_response(response);
  }
  bool add_breakpoint_request(lrdb::response_message& response,
                              const json::value& param) {
    if (!ensure_attached(response, "add_breakpoint")) {
      return send_response(response);
    }
    bool has_source = param.get("file").is<std::string>();
    bool has_condition = param.get("condition").is<std::string>();
    bool has_hit_condition = param.get("hit_condition").is<std::string>();
    int line = -1;
    bool has_line = try_parse_non_negative_int(param.get("line"), line);
    if (has_source && has_line && line > 0) {
      std::string source =
          param.get<json::object>().at("file").get<std::string>();

      std::string condition;
      std::string hit_condition;
      if (has_condition) {
        condition =
            param.get<json::object>().at("condition").get<std::string>();
      }
      if (has_hit_condition) {
        hit_condition =
            param.get<json::object>().at("hit_condition").get<std::string>();
      }
      debugger_.add_breakpoint(source, line, condition, hit_condition);

    } else {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "add_breakpoint",
                           error_data("request", "params"));
    }
    return send_response(response);
  }

  bool clear_breakpoints_request(lrdb::response_message& response,
                                 const json::value& param) {
    if (!ensure_attached(response, "clear_breakpoints")) {
      return send_response(response);
    }
    if (!param.is<json::object>() && !param.is<json::null>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "clear_breakpoints",
                           error_data("request", "params"));
      return send_response(response);
    }
    bool has_source = param.get("file").is<std::string>();
    int line = -1;
    bool has_line = try_parse_non_negative_int(param.get("line"), line);
    if (param.is<json::object>() && param.get<json::object>().count("line") > 0 &&
      !has_line) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "clear_breakpoints",
                           error_data("request", "line"));
      return send_response(response);
    }
    if (!has_source) {
      debugger_.clear_breakpoints();
    } else {
      std::string source =
          param.get<json::object>().at("file").get<std::string>();
      if (!has_line) {
        debugger_.clear_breakpoints(source);
      } else {
        debugger_.clear_breakpoints(source, line);
      }
    }

    return send_response(response);
  }

  bool get_breakpoints_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "get_breakpoints")) {
      return send_response(response);
    }
    const lrdb::debugger::line_breakpoint_type& breakpoints =
        debugger_.line_breakpoints();

    json::array res;
    for (const auto& b : breakpoints) {
      json::object br;
      br["file"] = json::value(b.file);
      if (!b.func.empty()) {
        br["func"] = json::value(b.func);
      }
      br["line"] = json::value(double(b.line));
      if (!b.condition.empty()) {
        br["condition"] = json::value(b.condition);
      }
      br["hit_count"] = json::value(double(b.hit_count));
      res.push_back(json::value(br));
    }

    response.result = json::value(res);

    return send_response(response);
  }

  bool get_stacktrace_request(lrdb::response_message& response, const json::value&) {
    if (!ensure_attached(response, "get_stacktrace") ||
        !ensure_paused(response, "get_stacktrace")) {
      return send_response(response);
    }
    auto callstack = debugger_.get_call_stack();
    json::array res;
    for (auto& s : callstack) {
      json::object data;
      if (s.source()) {
        data["file"] = json::value(s.source());
      }
      const char* name = s.name();
      if (!name || name[0] == '\0') {
        name = s.namewhat();
      }
      if (!name || name[0] == '\0') {
        name = s.what();
      }
      if (!name || name[0] == '\0') {
        name = s.source();
      }
      data["func"] = json::value(name != nullptr ? name : "?");
      data["line"] = json::value(double(s.currentline()));
      data["id"] = json::value(s.short_src());
      res.push_back(json::value(data));
    }
    response.result = json::value(res);

    return send_response(response);
  }

  bool get_local_variable_request(lrdb::response_message& response,
                                  const json::value& param) {
    int stack_no = 0;
    int depth = 1;
    std::vector<lrdb::stack_info> callstack;
    if (!extract_frame(response, "get_local_variable", param, stack_no, depth,
                       callstack)) {
      return send_response(response);
    }
    auto localvar = callstack[stack_no].get_local_vars(depth);
    json::object obj;
    for (auto& var : localvar) {
      obj[var.first] = var.second;
    }
    response.result = json::value(obj);
    return send_response(response);
  }

  bool get_upvalues_request(lrdb::response_message& response,
                            const json::value& param) {
    int stack_no = 0;
    int depth = 1;
    std::vector<lrdb::stack_info> callstack;
    if (!extract_frame(response, "get_upvalues", param, stack_no, depth,
                       callstack)) {
      return send_response(response);
    }
    auto localvar = callstack[stack_no].get_upvalues(depth);
    json::object obj;
    for (auto& var : localvar) {
      obj[var.first] = var.second;
    }
    response.result = json::value(obj);
    return send_response(response);
  }
  bool eval_request(lrdb::response_message& response, const json::value& param) {
    int stack_no = 0;
    int depth = 1;
    std::vector<lrdb::stack_info> callstack;
    if (!extract_frame(response, "eval", param, stack_no, depth, callstack)) {
      return send_response(response);
    }
    bool has_chunk = param.get("chunk").is<std::string>();
    if (!has_chunk) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "eval",
                           error_data("request", "chunk"));
      return send_response(response);
    }
    std::string chunk = param.get<json::object>().at("chunk").get<std::string>();
    if (chunk.size() > kMaxEvalChunkBytes) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "eval chunk exceeds limit", "eval",
                           error_data("request", "chunk_size"));
      return send_response(response);
    }

    bool use_global =
        !param.get("global").is<bool>() || param.get("global").get<bool>();
    bool use_upvalue =
        !param.get("upvalue").is<bool>() || param.get("upvalue").get<bool>();
    bool use_local =
        !param.get("local").is<bool>() || param.get("local").get<bool>();

    std::string error;
    json::value ret = json::value(callstack[stack_no].eval(
        chunk.c_str(), error, use_global, use_upvalue, use_local, depth + 1));
    if (error.empty()) {
      response.result = ret;
    } else {
      set_structured_error(response, lrdb::response_error::InvalidParams, error,
                           "eval", error_data("request", "runtime"));
    }
    return send_response(response);
  }
  bool get_global_request(lrdb::response_message& response,
                          const json::value& param) {
    if (!ensure_attached(response, "get_global") ||
        !ensure_paused(response, "get_global")) {
      return send_response(response);
    }
    int depth = 1;
    if (!try_parse_depth(param, depth)) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_global",
                           error_data("request", "depth"));
      return send_response(response);
    }
    response.result =
        debugger_.get_global_table(depth + 1);  //+ 1 is global table self

    return send_response(response);
  }

  bool get_metrics_request(lrdb::response_message& response, const json::value&) {
    response.result = get_metrics_snapshot();
    return send_response(response);
  }

#ifndef GMOD_CLIENT_MODULE
  bool get_entities_request(lrdb::response_message& response,
                            const json::value& param) {
    if (!ensure_attached(response, "get_entities")) {
      return send_response(response);
    }

    if (!param.is<json::null>() && !param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entities",
                           error_data("request", "params"));
      return send_response(response);
    }

    int offset = 0;
    int limit = kDefaultEntityListLimit;
    int filter_id = 0;
    std::string filter_class;

    if (param.is<json::object>()) {
      const json::object& params = param.get<json::object>();

        if (params.count("offset") > 0 &&
          !try_parse_non_negative_int(params.at("offset"), offset)) {
        set_structured_error(response, lrdb::response_error::InvalidParams,
                             "invalid params", "get_entities",
                             error_data("request", "offset"));
        return send_response(response);
      }

        if (params.count("limit") > 0 &&
          !try_parse_non_negative_int(params.at("limit"), limit)) {
        set_structured_error(response, lrdb::response_error::InvalidParams,
                             "invalid params", "get_entities",
                             error_data("request", "limit"));
        return send_response(response);
      }

        if (params.count("filter_id") > 0 &&
          !try_parse_non_negative_int(params.at("filter_id"), filter_id)) {
        set_structured_error(response, lrdb::response_error::InvalidParams,
                             "invalid params", "get_entities",
                             error_data("request", "filter_id"));
        return send_response(response);
      }

      if (params.count("filter_class") > 0) {
        if (!params.at("filter_class").is<std::string>()) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "get_entities",
                               error_data("request", "filter_class"));
          return send_response(response);
        }
        filter_class = params.at("filter_class").get<std::string>();
      }
    }

    if (limit > kMaxEntityListLimit) {
      limit = kMaxEntityListLimit;
    }

    EntitySnapshotQuery entity_query;
    json::array entities;
    entities =
        entity_query.list_entities(offset, limit, filter_id, filter_class);

    json::object result;
    result["entities"] = json::value(entities);
    result["total"] =
        json::value(static_cast<double>(entity_query.last_total()));
    result["offset"] = json::value(static_cast<double>(offset));
    result["limit"] = json::value(static_cast<double>(limit));
    response.result = json::value(result);
    return send_response(response);
  }

  bool get_entity_request(lrdb::response_message& response,
                          const json::value& param) {
    if (!ensure_attached(response, "get_entity")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity",
                           error_data("request", "index"));
      return send_response(response);
    }

    if (lua_base_ != nullptr) {
      try {
        EntityQuery entity_query(lua_base_);
        response.result =
          json::value(entity_query.get_entity_detail(entity_index));
        return send_response(response);
      } catch (const std::exception& ex) {
        set_structured_error(response, lrdb::response_error::InternalError,
                             "entity query failed", "get_entity",
                             error_data("request", "lua", ex.what()));
        return send_response(response);
      }
    }

    EntitySnapshotQuery entity_query;
    response.result = json::value(entity_query.get_entity_detail(entity_index));

    return send_response(response);
  }

  bool get_entity_table_request(lrdb::response_message& response,
                                const json::value& param) {
    if (!ensure_attached(response, "get_entity_table") ||
        !ensure_lua_interface(response, "get_entity_table")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity_table",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity_table",
                           error_data("request", "index"));
      return send_response(response);
    }

    std::string filter;
    if (params.count("filter") > 0) {
      if (!params.at("filter").is<std::string>()) {
        set_structured_error(response, lrdb::response_error::InvalidParams,
                             "invalid params", "get_entity_table",
                             error_data("request", "filter"));
        return send_response(response);
      }
      filter = params.at("filter").get<std::string>();
    }

    json::array entries;
    try {
      EntityQuery entity_query(lua_base_);
      entries = entity_query.get_entity_table_entries(entity_index, filter);
    } catch (const std::exception& ex) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "entity table query failed", "get_entity_table",
                           error_data("request", "lua", ex.what()));
      return send_response(response);
    }

    json::object result;
    result["index"] = json::value(static_cast<double>(entity_index));
    result["total"] = json::value(static_cast<double>(entries.size()));
    result["entries"] = json::value(entries);
    response.result = json::value(result);
    return send_response(response);
  }

  bool set_entity_table_value_request(lrdb::response_message& response,
                                      const json::value& param) {
    if (!ensure_attached(response, "set_entity_table_value") ||
        !ensure_lua_interface(response, "set_entity_table_value")) {
      return send_response(response);
    }

    if (!debugger_.paused()) {
      set_structured_error(
          response, -32001,
          "Entity table modification requires debugger to be paused",
          "set_entity_table_value", error_data("request", "state", "running"));
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_table_value",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_table_value",
                           error_data("request", "index"));
      return send_response(response);
    }

    if (params.count("property") == 0 || !params.at("property").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_table_value",
                           error_data("request", "property"));
      return send_response(response);
    }

    if (params.count("value") == 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_table_value",
                           error_data("request", "value"));
      return send_response(response);
    }

    const std::string field_name = params.at("property").get<std::string>();
    const json::value& value = params.at("value");

    EntityQuery entity_query(lua_base_);
    bool updated = false;
    try {
      updated = entity_query.set_entity_table_field(entity_index, field_name, value);
    } catch (const std::exception& ex) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "entity table modification failed",
                           "set_entity_table_value",
                           error_data("request", "lua", ex.what()));
      return send_response(response);
    }

    if (!updated) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "entity table update failed", "set_entity_table_value",
                           error_data("request", "entity"));
      return send_response(response);
    }

    json::object result;
    result["ok"] = json::value(true);
    result["index"] = json::value(static_cast<double>(entity_index));
    result["property"] = json::value(field_name);
    response.result = json::value(result);
    return send_response(response);
  }

  bool get_entity_network_vars_request(lrdb::response_message& response,
                                       const json::value& param) {
    if (!ensure_attached(response, "get_entity_network_vars") ||
        !ensure_lua_interface(response, "get_entity_network_vars")) {
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity_network_vars",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "get_entity_network_vars",
                           error_data("request", "index"));
      return send_response(response);
    }

    json::array entries;
    try {
      EntityQuery entity_query(lua_base_);
      entries = entity_query.get_entity_network_var_entries(entity_index);
    } catch (const std::exception& ex) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "entity network var query failed",
                           "get_entity_network_vars",
                           error_data("request", "lua", ex.what()));
      return send_response(response);
    }

    json::object result;
    result["index"] = json::value(static_cast<double>(entity_index));
    result["total"] = json::value(static_cast<double>(entries.size()));
    result["entries"] = json::value(entries);
    response.result = json::value(result);
    return send_response(response);
  }

  bool set_entity_property_request(lrdb::response_message& response,
                                   const json::value& param) {
    if (!ensure_attached(response, "set_entity_property") ||
        !ensure_lua_interface(response, "set_entity_property")) {
      return send_response(response);
    }

    if (!debugger_.paused()) {
      set_structured_error(
          response, -32001,
          "Entity modification requires debugger to be paused",
          "set_entity_property", error_data("request", "state", "running"));
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_property",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_property",
                           error_data("request", "index"));
      return send_response(response);
    }

    if (params.count("property") == 0 ||
        !params.at("property").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_property",
                           error_data("request", "property"));
      return send_response(response);
    }

    if (params.count("value") == 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_property",
                           error_data("request", "value"));
      return send_response(response);
    }

    const std::string property_name = params.at("property").get<std::string>();
    const json::value& property_value = params.at("value");

    EntityQuery entity_query(lua_base_);
    bool updated = false;

    try {
      if (property_name == "pos") {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (!try_parse_triplet(property_value, x, y, z)) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "set_entity_property",
                               error_data("request", "pos"));
          return send_response(response);
        }
        updated = entity_query.set_entity_pos(entity_index, x, y, z);
      } else if (property_name == "angles") {
        double pitch = 0.0;
        double yaw = 0.0;
        double roll = 0.0;
        if (!try_parse_triplet(property_value, pitch, yaw, roll)) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "set_entity_property",
                               error_data("request", "angles"));
          return send_response(response);
        }
        updated = entity_query.set_entity_angles(entity_index, pitch, yaw, roll);
      } else if (property_name == "health") {
        double health = 0.0;
        if (!property_value.is<double>()) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "set_entity_property",
                               error_data("request", "health"));
          return send_response(response);
        }
        health = property_value.get<double>();
        if (!std::isfinite(health)) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "set_entity_property",
                               error_data("request", "health"));
          return send_response(response);
        }
        updated = entity_query.set_entity_health(entity_index, health);
      } else {
        updated =
            entity_query.set_entity_field(entity_index, property_name, property_value);
      }
    } catch (const std::exception& ex) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "entity modification failed", "set_entity_property",
                           error_data("request", "lua", ex.what()));
      return send_response(response);
    }

    if (!updated) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "entity update failed", "set_entity_property",
                           error_data("request", "entity"));
      return send_response(response);
    }

    json::object result;
    result["ok"] = json::value(true);
    result["index"] = json::value(static_cast<double>(entity_index));
    result["property"] = json::value(property_name);
    response.result = json::value(result);
    return send_response(response);
  }

  bool set_entity_network_var_request(lrdb::response_message& response,
                                      const json::value& param) {
    if (!ensure_attached(response, "set_entity_network_var") ||
        !ensure_lua_interface(response, "set_entity_network_var")) {
      return send_response(response);
    }

    if (!debugger_.paused()) {
      set_structured_error(
          response, -32001,
          "NetworkVar modification requires debugger to be paused",
          "set_entity_network_var", error_data("request", "state", "running"));
      return send_response(response);
    }

    if (!param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_network_var",
                           error_data("request", "params"));
      return send_response(response);
    }

    const json::object& params = param.get<json::object>();
    int entity_index = 0;
    if (params.count("index") == 0 ||
        !try_parse_non_negative_int(params.at("index"), entity_index) ||
        entity_index <= 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_network_var",
                           error_data("request", "index"));
      return send_response(response);
    }

    if (params.count("name") == 0 || !params.at("name").is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_network_var",
                           error_data("request", "name"));
      return send_response(response);
    }

    if (params.count("value") == 0) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "set_entity_network_var",
                           error_data("request", "value"));
      return send_response(response);
    }

    const std::string var_name = params.at("name").get<std::string>();
    const json::value& value = params.at("value");

    EntityQuery entity_query(lua_base_);
    bool updated = false;
    try {
      updated = entity_query.set_entity_network_var(entity_index, var_name, value);
    } catch (const std::exception& ex) {
      set_structured_error(response, lrdb::response_error::InternalError,
                           "network var modification failed",
                           "set_entity_network_var",
                           error_data("request", "lua", ex.what()));
      return send_response(response);
    }

    if (!updated) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "network var update failed", "set_entity_network_var",
                           error_data("request", "entity"));
      return send_response(response);
    }

    // Successful setter invocation means the call path worked; the game may
    // still clamp/reject values in scripted logic after this request returns.

    json::object result;
    result["ok"] = json::value(true);
    result["index"] = json::value(static_cast<double>(entity_index));
    result["name"] = json::value(var_name);
    response.result = json::value(result);
    return send_response(response);
  }
#endif  // !GMOD_CLIENT_MODULE

  static std::string trim_copy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && V_isspace(value[begin])) {
      ++begin;
    }

    size_t end = value.size();
    while (end > begin && V_isspace(value[end - 1])) {
      --end;
    }

    return value.substr(begin, end - begin);
  }

  static bool try_extract_lua_error_from_line(const std::string& line,
                                              std::string& out_message) {
    static const std::regex kStackTraceRegex(R"(stack traceback:)",
                                             std::regex::icase);
    static const std::regex kStackFrameRegex(R"(^\s*\d+\.\s+)");
    static const std::regex kLuaPathErrorRegex(
      R"(^\s*(?:\[[^\]\r\n]+\]\s*)?(?:[A-Za-z]:)?@?(?:[^:\r\n]*[\\/])?[^:\r\n]*\.lua:\d+:\s*(.+?)\s*$)",
        std::regex::icase);

    std::string trimmed = trim_copy(line);
    if (trimmed.empty() || std::regex_search(trimmed, kStackTraceRegex)) {
      return false;
    }

    const std::string error_prefix = "[ERROR]";
    bool has_error_prefix = false;
    if (trimmed.compare(0, error_prefix.size(), error_prefix) == 0) {
      trimmed = trim_copy(trimmed.substr(error_prefix.size()));
      has_error_prefix = true;
    }

    if (trimmed.empty() || std::regex_search(trimmed, kStackTraceRegex) ||
        std::regex_search(trimmed, kStackFrameRegex)) {
      return false;
    }

    std::smatch match;
    if (std::regex_match(trimmed, match, kLuaPathErrorRegex) &&
        match.size() >= 2) {
      out_message = trim_copy(match[1].str());
      return !out_message.empty();
    }

    if (has_error_prefix) {
      out_message = trimmed;
      return !out_message.empty();
    }

    return false;
  }

  bool try_extract_lua_error_message(const json::value& message,
                                     std::string& out_message) const {
    if (!message.is<json::object>() ||
      message.get<json::object>().count("message") == 0 ||
        !message.get("message").is<std::string>()) {
      return false;
    }

    const std::string raw_message = message.get("message").get<std::string>();
    if (try_extract_lua_error_from_line(raw_message, out_message)) {
      return true;
    }

    size_t line_start = 0;
    while (line_start <= raw_message.size()) {
      size_t line_end = raw_message.find_first_of("\r\n", line_start);
      const std::string line =
          line_end == std::string::npos
              ? raw_message.substr(line_start)
              : raw_message.substr(line_start, line_end - line_start);

      if (try_extract_lua_error_from_line(line, out_message)) {
        return true;
      }

      if (line_end == std::string::npos) {
        break;
      }

      line_start = line_end + 1;
      if (line_start < raw_message.size() && raw_message[line_end] == '\r' &&
          raw_message[line_start] == '\n') {
        ++line_start;
      }
    }

    return false;
  }

  void emit_error_notification(const std::string& message,
                               const std::string& raw_message,
                               const std::string& fingerprint,
                               int count) {
    json::object param;
    param["message"] = json::value(message);
    param["raw_message"] = json::value(raw_message);
    param["fingerprint"] = json::value(fingerprint);
    param["count"] = json::value(static_cast<double>(count));
    param["source"] = json::value("lua");
    send_notify(lrdb::notify_message("error", json::value(param)));
  }

  void handle_console_output(const json::value& message) {
    send_output(message);

    std::string lua_error;
    if (!try_extract_lua_error_message(message, lua_error)) {
      return;
    }

    std::string raw_lua_error = lua_error;
    if (message.is<json::object>() &&
        message.get<json::object>().count("message") > 0 &&
        message.get("message").is<std::string>()) {
      raw_lua_error = message.get("message").get<std::string>();
    }

    const auto aggregated = error_aggregator_.add_error(lua_error);
    emit_error_notification(lua_error, raw_lua_error, aggregated.first,
                            aggregated.second);

    // Pause requests are queued and fulfilled from debugger tick context to
    // avoid touching debugger state from console callback threads.
    if (attached_ && !disposed_ && stop_on_error_) {
      pending_pause_on_error_.store(true);
    }
  }

  bool send_output(const json::value& message) {
    return send_notify(lrdb::notify_message("output", message));
  }

  json::value get_metrics_snapshot() const {
    json::object metrics;
    metrics["connections_opened"] =
        json::value(static_cast<double>(metrics_.connections_opened.load()));
    metrics["connections_closed"] =
        json::value(static_cast<double>(metrics_.connections_closed.load()));
    metrics["requests_received"] =
        json::value(static_cast<double>(metrics_.requests_received.load()));
    metrics["responses_sent"] =
        json::value(static_cast<double>(metrics_.responses_sent.load()));
    metrics["notifications_sent"] =
        json::value(static_cast<double>(metrics_.notifications_sent.load()));
    metrics["parse_errors"] =
        json::value(static_cast<double>(metrics_.parse_errors.load()));
    metrics["invalid_requests"] =
        json::value(static_cast<double>(metrics_.invalid_requests.load()));
    metrics["invalid_params_errors"] =
        json::value(static_cast<double>(metrics_.invalid_params_errors.load()));
    metrics["method_not_found_errors"] =
        json::value(static_cast<double>(metrics_.method_not_found_errors.load()));
    metrics["internal_errors"] =
        json::value(static_cast<double>(metrics_.internal_errors.load()));
    metrics["stream_errors"] =
        json::value(static_cast<double>(metrics_.stream_errors.load()));
    metrics["send_failures"] =
        json::value(static_cast<double>(metrics_.send_failures.load()));
    metrics["connection_wait_timeouts"] =
        json::value(static_cast<double>(metrics_.connection_wait_timeouts.load()));
    return json::value(metrics);
  }

#ifndef GMOD_CLIENT_MODULE
  bool command_request(lrdb::response_message& response,
                           const json::value& param) {
    if (!ensure_attached(response, "command")) {
      return send_response(response);
    }
    if (!param.is<std::string>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "command",
                           error_data("request", "params"));
      return send_response(response);
    }

    const std::string command = param.get<std::string>();
    std::string validation_error;
    if (!validate_console_command(command, validation_error)) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid command", "command",
                           error_data("request", "command", validation_error));
      return send_response(response);
    }

    auto run_console_fallback = [&](const std::string& reason) {
      std::string dispatch_error;
      if (console_adapter_.run_command(command, dispatch_error)) {
        return true;
      }

      std::string detail = reason;
      if (!dispatch_error.empty()) {
        detail += "; fallback dispatch failed: " + dispatch_error;
      }

      set_structured_error(response, lrdb::response_error::InternalError,
                           "failed to execute command", "command",
                           error_data("request", "command", detail));
      return false;
    };

    if (lua_base_ == nullptr) {
      if (!run_console_fallback("lua interface unavailable")) {
        return send_response(response);
      }
      return send_response(response);
    }

    if (should_force_console_adapter_dispatch(command)) {
      if (!run_console_fallback(
              "command requires engine dispatch (game.ConsoleCommand blocked)")) {
        return send_response(response);
      }
      return send_response(response);
    }

    // Use Lua game.ConsoleCommand for reliable command execution in GMod.
    // This avoids engine interface timing issues when called from a debug hook.
    std::string command_text = command;
    if (command_text.empty() || command_text.back() != '\n') {
      command_text.push_back('\n');
    }

    const int stack_top = lua_base_->Top();

    lua_base_->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua_base_->GetField(-1, "game");

    bool is_game_table =
        lua_base_->IsType(-1, GarrysMod::Lua::Type::Table);
    if (!is_game_table) {
      lua_base_->Pop(lua_base_->Top() - stack_top);
      if (!run_console_fallback("game table not found")) {
        return send_response(response);
      }
      return send_response(response);
    }

    lua_base_->GetField(-1, "ConsoleCommand");

    bool is_function =
        lua_base_->IsType(-1, GarrysMod::Lua::Type::Function);
    if (!is_function) {
      lua_base_->Pop(lua_base_->Top() - stack_top);
      if (!run_console_fallback("game.ConsoleCommand not found")) {
        return send_response(response);
      }
      return send_response(response);
    }

    lua_base_->PushString(command_text.c_str());
    int pcall_result = lua_base_->PCall(1, 0, 0);

    if (pcall_result != 0) {
      const char* err_str = lua_base_->GetString(-1);
      std::string err_msg =
          err_str != nullptr ? std::string(err_str) : "unknown lua error";
      lua_base_->Pop(1);  // Pop error message.
      lua_base_->Pop(lua_base_->Top() - stack_top);
      if (!run_console_fallback("game.ConsoleCommand runtime error: " +
                                err_msg)) {
        return send_response(response);
      }
      return send_response(response);
    }

    // Clean up game table and global table.
    lua_base_->Pop(lua_base_->Top() - stack_top);
    return send_response(response);
  }
#endif  // !GMOD_CLIENT_MODULE

  bool init_request(lrdb::response_message& response,
                    const json::value& param) {
    if (!param.is<json::null>() && !param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "init",
                           error_data("request", "params"));
      return send_response(response);
    }

    if (param.is<json::object>()) {
      const json::object& params = param.get<json::object>();

      if (params.count("protocol_version") > 0) {
        if (!params.at("protocol_version").is<std::string>()) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "init",
                               error_data("request", "protocol_version"));
          return send_response(response);
        }

        const std::string protocol_version =
            params.at("protocol_version").get<std::string>();
        if (protocol_version != LRDB_SERVER_PROTOCOL_VERSION) {
          json::object result;
          result["warning"] = json::value("protocol_version mismatch");
          result["server_protocol_version"] =
              json::value(LRDB_SERVER_PROTOCOL_VERSION);
          response.result = json::value(result);
        }
      }

      if (params.count("stop_on_error") > 0) {
        if (!params.at("stop_on_error").is<bool>()) {
          set_structured_error(response, lrdb::response_error::InvalidParams,
                               "invalid params", "init",
                               error_data("request", "stop_on_error"));
          return send_response(response);
        }
        stop_on_error_ = params.at("stop_on_error").get<bool>();
      }
    }

    return send_response(response);
  }

  bool clear_error_cache_request(lrdb::response_message& response,
                                 const json::value& param) {
    if (!param.is<json::null>() && !param.is<json::object>()) {
      set_structured_error(response, lrdb::response_error::InvalidParams,
                           "invalid params", "clear_error_cache",
                           error_data("request", "params"));
      return send_response(response);
    }

    error_aggregator_.clear();
    pending_pause_on_error_.store(false);
    return send_response(response);
  }

  void execute_request(const lrdb::request_message& req) {
    ++metrics_.requests_received;
    typedef bool (basic_server::*exec_cmd_fn)(lrdb::response_message & response,
                                              const json::value& param);

    static const std::map<std::string, exec_cmd_fn> cmd_map = {
#define LRDB_DEBUG_COMMAND_TABLE(NAME) {#NAME, &basic_server::NAME##_request}
        LRDB_DEBUG_COMMAND_TABLE(init),
        LRDB_DEBUG_COMMAND_TABLE(step),
        LRDB_DEBUG_COMMAND_TABLE(step_in),
        LRDB_DEBUG_COMMAND_TABLE(step_out),
        LRDB_DEBUG_COMMAND_TABLE(continue),
        LRDB_DEBUG_COMMAND_TABLE(pause),
        LRDB_DEBUG_COMMAND_TABLE(pause_now),
        LRDB_DEBUG_COMMAND_TABLE(add_breakpoint),
        LRDB_DEBUG_COMMAND_TABLE(get_breakpoints),
        LRDB_DEBUG_COMMAND_TABLE(clear_breakpoints),
        LRDB_DEBUG_COMMAND_TABLE(get_stacktrace),
        LRDB_DEBUG_COMMAND_TABLE(get_local_variable),
        LRDB_DEBUG_COMMAND_TABLE(get_upvalues),
        LRDB_DEBUG_COMMAND_TABLE(eval),
        LRDB_DEBUG_COMMAND_TABLE(get_global),
        LRDB_DEBUG_COMMAND_TABLE(get_metrics),
#ifndef GMOD_CLIENT_MODULE
        LRDB_DEBUG_COMMAND_TABLE(get_entities),
        LRDB_DEBUG_COMMAND_TABLE(get_entity),
        LRDB_DEBUG_COMMAND_TABLE(get_entity_network_vars),
        LRDB_DEBUG_COMMAND_TABLE(get_entity_table),
        LRDB_DEBUG_COMMAND_TABLE(set_entity_table_value),
        LRDB_DEBUG_COMMAND_TABLE(set_entity_network_var),
        LRDB_DEBUG_COMMAND_TABLE(set_entity_property),
        LRDB_DEBUG_COMMAND_TABLE(command),
        LRDB_DEBUG_COMMAND_TABLE(run_lua),
        LRDB_DEBUG_COMMAND_TABLE(run_file),
        LRDB_DEBUG_COMMAND_TABLE(refresh_file),
#endif  // !GMOD_CLIENT_MODULE
        LRDB_DEBUG_COMMAND_TABLE(clear_error_cache),
#undef LRDB_DEBUG_COMMAND_TABLE
    };

    lrdb::response_message response;
    response.id = req.id;
    if (disposed_) {
      set_structured_error(response, lrdb::response_error::ServerNotInitialized,
                           "server is disposed", req.method,
                           error_data("session", "state", "disposed"));
      send_response(response);
      return;
    }
    auto match = cmd_map.find(req.method);
    if (match != cmd_map.end()) {
      try {
        (this->*(match->second))(response, req.params);
      } catch (const std::exception& ex) {
        set_structured_error(response, lrdb::response_error::InternalError,
                             "internal server error", req.method,
                             error_data("request", "exception", ex.what()));
        send_response(response);
      } catch (...) {
        set_structured_error(response, lrdb::response_error::InternalError,
                             "internal server error", req.method,
                             error_data("request", "exception"));
        send_response(response);
      }
    } else {
      json::object method_error_detail;
      method_error_detail["phase"] = json::value("request");
      method_error_detail["category"] = json::value("method");
      method_error_detail["protocol_version"] =
          json::value(LRDB_SERVER_PROTOCOL_VERSION);
      set_structured_error(response, lrdb::response_error::MethodNotFound,
                           "method not found : " + req.method, req.method,
                           json::value(method_error_detail));
      send_response(response);
    }
  }
  // Zero means one non-blocking ASIO poll per Lua line when waiting for a client;
  // avoids freezing the game while no debugger is connected.
  static constexpr int kConnectionWaitTimeoutMs = 0;
  static constexpr int kMaxObjectDepth = 8;
  static constexpr int kDefaultEntityListLimit = 50;
  static constexpr int kMaxEntityListLimit = 200;
  static constexpr size_t kMaxEvalChunkBytes = 16 * 1024;
  static constexpr size_t kMaxRunLuaChunkBytes = 256 * 1024;
  static constexpr size_t kMaxLuaFilePathBytes = 1024;
  static constexpr size_t kMaxConsoleCommandBytes = 2048;
  static constexpr size_t kMaxConsoleCommandNameBytes = 128;

  bool wait_for_connect_;
  bool attached_;
  bool disposed_;
  bool has_active_connection_;
  std::atomic_bool pending_pause_on_error_;
  bool stop_on_error_;
  bool pause_on_activate_;
  GarrysMod::Lua::ILuaBase* lua_base_;
  struct reliability_metrics {
    std::atomic<uint64_t> connections_opened{0};
    std::atomic<uint64_t> connections_closed{0};
    std::atomic<uint64_t> requests_received{0};
    std::atomic<uint64_t> responses_sent{0};
    std::atomic<uint64_t> notifications_sent{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> invalid_requests{0};
    std::atomic<uint64_t> invalid_params_errors{0};
    std::atomic<uint64_t> method_not_found_errors{0};
    std::atomic<uint64_t> internal_errors{0};
    std::atomic<uint64_t> stream_errors{0};
    std::atomic<uint64_t> send_failures{0};
    std::atomic<uint64_t> connection_wait_timeouts{0};
  } metrics_;
  error_aggregator error_aggregator_;
  lrdb::debugger debugger_;
  StreamType command_stream_;
  console_adapter console_adapter_;
};
