#pragma once

#if __cplusplus < 201103L && !(defined(_MSC_VER) || _MSC_VER >= 1800)
#error Needs at least a C++11 compiler
#endif

#include <memory>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lrdb/debugger.hpp>
#include <lrdb/message.hpp>

#include <picojson.h>

#include "console_adapter_logging.hpp"
#include "console_adapter_spew.hpp"

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
        command_stream_(std::forward<StreamArgs>(arg)...) {
    init();
  }

  ~basic_server() { exit(); }

  /// @brief attach (or detach) for debug target
  /// @param lua_State*  debug target
  void reset(lua_State* L = nullptr) {
    attached_ = L != nullptr;
    disposed_ = false;
    debugger_.reset(L);
    if (!L) {
      wait_for_connect_ = true;
      debugger_.unpause();
      console_adapter_.set_callback(nullptr);
      command_stream_.reconnect();
    } else {
      console_adapter_.set_callback(std::bind(&basic_server<StreamType>::send_output, this, std::placeholders::_1));
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

  bool try_parse_depth(const json::value& param, int& depth) {
    if (!param.is<json::object>() || !param.contains("depth")) {
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
    if (!param.is<json::object>() || !param.contains("stack_no")) {
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
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
  }

  static bool is_valid_command_name_char(char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0 || ch == '_' || ch == '.' || ch == '+' ||
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
      if (std::iscntrl(value) != 0 && ch != '\t' && ch != ' ') {
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
    if (param.is<json::object>() && param.contains("line") && !has_line) {
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
        name = s.name();
      }
      if (!name || name[0] == '\0') {
        name = s.namewhat();
      }
      if (!name || name[0] == '\0') {
        name = s.what();
      }
      if (!name || name[0] == '\0') {
        name = s.source();
      }
      data["func"] = json::value(name);
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

    std::string dispatch_error;
    if (!console_adapter_.run_command(command, dispatch_error)) {
      set_structured_error(
          response, lrdb::response_error::InternalError,
          "failed to execute command", "command",
          error_data("request", "command",
                     dispatch_error.empty() ? "unknown command dispatch error"
                                            : dispatch_error));
      return send_response(response);
    }

    return send_response(response);
  }

  void execute_request(const lrdb::request_message& req) {
    ++metrics_.requests_received;
    typedef bool (basic_server::*exec_cmd_fn)(lrdb::response_message & response,
                                              const json::value& param);

    static const std::map<std::string, exec_cmd_fn> cmd_map = {
#define LRDB_DEBUG_COMMAND_TABLE(NAME) {#NAME, &basic_server::NAME##_request}
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
        LRDB_DEBUG_COMMAND_TABLE(command),
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
  static constexpr int kConnectionWaitTimeoutMs = 100;
  static constexpr int kMaxObjectDepth = 8;
  static constexpr size_t kMaxEvalChunkBytes = 16 * 1024;
  static constexpr size_t kMaxConsoleCommandBytes = 2048;
  static constexpr size_t kMaxConsoleCommandNameBytes = 128;

  bool wait_for_connect_;
  bool attached_;
  bool disposed_;
  bool has_active_connection_;
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
  lrdb::debugger debugger_;
  StreamType command_stream_;
  console_adapter console_adapter_;
};
