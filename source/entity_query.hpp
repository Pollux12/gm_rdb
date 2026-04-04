#pragma once

#include <GarrysMod/Lua/Interface.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tier1/strtools.h>

#include <picojson.h>

class EntityQuery {
 public:
  explicit EntityQuery(GarrysMod::Lua::ILuaBase* lua)
      : lua_(lua), last_total_(0) {}

  picojson::array list_entities(int offset, int limit, int filter_id,
                                const std::string& filter_class) {
    picojson::array entities;
    last_total_ = 0;
    if (lua_ == nullptr || limit < 0) {
      return entities;
    }

    if (offset < 0) {
      offset = 0;
    }
    if (limit > kMaxPageLimit) {
      limit = kMaxPageLimit;
    }

    stack_guard guard(lua_);

    if (filter_id > 0) {
      if (!push_entity_by_index(filter_id)) {
        return entities;
      }
      if (!lua_->IsType(-1, GarrysMod::Lua::Type::Entity)) {
        return entities;
      }
      picojson::object summary = build_entity_summary(-1);
      if (matches_class_filter(summary, filter_class)) {
        last_total_ = 1;
        if (offset == 0 && limit > 0) {
          entities.emplace_back(summary);
        }
      }
      return entities;
    }

    if (!push_ents_table()) {
      return entities;
    }

    const int ents_table = abs_index(-1);
    lua_->GetField(ents_table, "GetAll");
    if (!is_type(-1, "function")) {
      return entities;
    }

    pcall_checked(0, 1, "ents.GetAll");

    if (!is_type(-1, "table")) {
      return entities;
    }

    const int all_entities = abs_index(-1);
    lua_->PushNil();

    int matched = 0;
    while (lua_->Next(all_entities) != 0) {
      // Skip non-entity values in the table.
      if (!lua_->IsType(-1, GarrysMod::Lua::Type::Entity)) {
        lua_->Pop(1);
        continue;
      }

      const picojson::object summary = build_entity_summary(-1);
      lua_->Pop(1);  // Pop entity value, keep iterator key.

      if (!matches_class_filter(summary, filter_class)) {
        continue;
      }

      if (matched >= offset && static_cast<int>(entities.size()) < limit) {
        entities.emplace_back(summary);
      }
      ++matched;
    }

    last_total_ = matched;
    return entities;
  }

  picojson::object get_entity_detail(int entity_index) {
    picojson::object detail;
    detail["index"] = picojson::value(static_cast<double>(entity_index));
    detail["valid"] = picojson::value(false);

    if (entity_index <= 0 || lua_ == nullptr) {
      return detail;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return detail;
    }

    const int entity = abs_index(-1);
    detail = build_entity_summary(entity);

    double parent_index = 0.0;
    bool has_parent = false;
    try {
      if (invoke_entity_method(entity, "GetParent", 0, 1)) {
        const int parent = abs_index(-1);
        bool parent_valid = false;
        try_get_entity_bool(parent, "IsValid", parent_valid);
        if (parent_valid &&
            try_get_entity_number(parent, "EntIndex", parent_index)) {
          has_parent = true;
        }
        lua_->Pop(1);
      }
    } catch (const std::exception&) {
      has_parent = false;
    }

    detail["parent_index"] =
        has_parent ? picojson::value(parent_index) : picojson::value();

    double health = 0.0;
    if (try_get_entity_number(entity, "Health", health)) {
      detail["health"] = picojson::value(health);
    }

    // Entity:GetTable() values are loaded lazily by get_entity_table.
    detail["properties"] = picojson::value(picojson::object());
    return detail;
  }

  picojson::array get_entity_table_entries(int entity_index,
                                           const std::string& filter) {
    picojson::array entries;
    if (entity_index <= 0 || lua_ == nullptr) {
      return entries;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return entries;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return entries;
    }

    if (!invoke_entity_method(entity, "GetTable", 0, 1)) {
      return entries;
    }

    if (!is_type(-1, "table")) {
      lua_->Pop(1);
      return entries;
    }

    const int table_index = abs_index(-1);
    lua_->PushNil();

    int captured = 0;
    while (lua_->Next(table_index) != 0) {
      std::string key;
      if (is_type(-2, "string")) {
        key = get_string_or_empty(-2);
      } else if (is_type(-2, "number")) {
        key = format_number(lua_->GetNumber(-2));
      }

      if (key.empty()) {
        lua_->Pop(1);
        continue;
      }

      picojson::object entry;
      if (build_entity_table_entry(key, -1, filter, entry)) {
        entries.emplace_back(entry);
        ++captured;
      }

      lua_->Pop(1);  // Pop table value, keep key for iteration.
      if (captured >= kMaxEntityTableEntries) {
        lua_->Pop(1);  // Pop iterator key before exiting loop.
        break;
      }
    }

    lua_->Pop(1);  // Pop Entity:GetTable() result.

    return entries;
  }

  bool set_entity_table_field(int entity_index, const std::string& field_name,
                              const picojson::value& field_value) {
    if (lua_ == nullptr || entity_index <= 0 ||
        !is_valid_field_name(field_name)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    if (!invoke_entity_method(entity, "GetTable", 0, 1)) {
      return false;
    }

    if (!is_type(-1, "table")) {
      lua_->Pop(1);
      return false;
    }

    const int table_index = abs_index(-1);
    if (!push_json_value(field_value)) {
      return false;
    }

    lua_->SetField(table_index, field_name.c_str());
    return true;
  }

  picojson::array get_entity_network_var_entries(int entity_index) {
    picojson::array entries;
    if (entity_index <= 0 || lua_ == nullptr) {
      return entries;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return entries;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return entries;
    }

    if (!invoke_entity_method(entity, "GetNetworkVars", 0, 1)) {
      return entries;
    }

    if (!is_type(-1, "table")) {
      lua_->Pop(1);
      return entries;
    }

    const int vars_table = abs_index(-1);
    lua_->PushNil();

    int captured = 0;
    while (lua_->Next(vars_table) != 0) {
      std::string key;
      if (is_type(-2, "string")) {
        key = get_string_or_empty(-2);
      } else if (is_type(-2, "number")) {
        key = format_number(lua_->GetNumber(-2));
      }

      if (key.empty()) {
        lua_->Pop(1);
        continue;
      }

      picojson::object entry;
      if (build_entity_table_entry(key, -1, "", entry)) {
        entries.emplace_back(entry);
        ++captured;
      }

      lua_->Pop(1);  // Pop table value, keep key for iteration.
      if (captured >= kMaxEntityTableEntries) {
        lua_->Pop(1);  // Pop iterator key before exiting loop.
        break;
      }
    }

    lua_->Pop(1);  // Pop network vars table.
    return entries;
  }

  bool set_entity_field(int entity_index, const std::string& field_name,
                        const picojson::value& field_value) {
    if (lua_ == nullptr || entity_index <= 0 ||
        !is_valid_field_name(field_name)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    if (!push_json_primitive(field_value)) {
      return false;
    }

    lua_->SetField(entity, field_name.c_str());
    return true;
  }

  bool set_entity_network_var(int entity_index, const std::string& var_name,
                              const picojson::value& field_value) {
    if (lua_ == nullptr || entity_index <= 0 || !is_valid_field_name(var_name)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    const std::string setter = "Set" + var_name;
    lua_->GetField(entity, setter.c_str());
    if (!is_type(-1, "function")) {
      lua_->Pop(1);
      return false;
    }

    lua_->Push(entity);
    if (!push_json_value(field_value)) {
      lua_->Pop(2);
      return false;
    }

    pcall_checked(2, 0, setter.c_str());
    return true;
  }

  bool set_entity_pos(int entity_index, double x, double y, double z) {
    if (lua_ == nullptr || entity_index <= 0 || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    lua_->GetField(entity, "SetPos");
    if (!is_type(-1, "function")) {
      return false;
    }

    lua_->Push(entity);

    Vector vec;
    vec.x = static_cast<float>(x);
    vec.y = static_cast<float>(y);
    vec.z = static_cast<float>(z);
    lua_->PushVector(vec);

    pcall_checked(2, 0, "Entity:SetPos");
    return true;
  }

  bool set_entity_angles(int entity_index, double pitch, double yaw,
                         double roll) {
    if (lua_ == nullptr || entity_index <= 0 || !std::isfinite(pitch) ||
        !std::isfinite(yaw) || !std::isfinite(roll)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    if (try_invoke_entity_angle_setter(entity, "SetAngles", pitch, yaw,
                                       roll)) {
      return true;
    }

    // Players may reject SetAngles; SetEyeAngles is the reliable fallback.
    return try_invoke_entity_angle_setter(entity, "SetEyeAngles", pitch, yaw,
                                          roll);
  }

  bool set_entity_health(int entity_index, double health) {
    if (lua_ == nullptr || entity_index <= 0 || !std::isfinite(health)) {
      return false;
    }

    stack_guard guard(lua_);
    if (!push_entity_by_index(entity_index)) {
      return false;
    }

    const int entity = abs_index(-1);
    bool is_valid = false;
    if (!try_get_entity_bool(entity, "IsValid", is_valid) || !is_valid) {
      return false;
    }

    lua_->GetField(entity, "SetHealth");
    if (is_type(-1, "function")) {
      try {
        lua_->Push(entity);
        lua_->PushNumber(health);
        pcall_checked(2, 0, "Entity:SetHealth");
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    lua_->Pop(1);
    return false;
  }

  int last_total() const { return last_total_; }

 private:
  class stack_guard {
   public:
    explicit stack_guard(GarrysMod::Lua::ILuaBase* lua)
        : lua_(lua), top_(lua ? lua->Top() : 0) {}

    ~stack_guard() {
      if (lua_ == nullptr) {
        return;
      }
      const int current_top = lua_->Top();
      if (current_top > top_) {
        lua_->Pop(current_top - top_);
      }
    }

   private:
    GarrysMod::Lua::ILuaBase* lua_;
    int top_;
  };

  int abs_index(int index) const {
    if (index > 0) {
      return index;
    }
    return lua_->Top() + index + 1;
  }

  bool is_type(int index, const char* type_name) const {
    const char* actual = lua_->GetTypeName(lua_->GetType(index));
    return actual != nullptr && std::strcmp(actual, type_name) == 0;
  }

  static bool is_valid_field_name(const std::string& field_name) {
    if (field_name.empty() || field_name.size() > 64) {
      return false;
    }

    if (field_name.size() >= 2 && field_name[0] == '_' && field_name[1] == '_') {
      return false;
    }

    for (const char ch : field_name) {
      if (!V_isalnum(ch) && ch != '_') {
        return false;
      }
    }

    return true;
  }

  static bool contains_case_insensitive(const std::string& text,
                                        const std::string& needle) {
    if (needle.empty()) {
      return true;
    }

    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [](char lhs, char rhs) {
                         return std::tolower(static_cast<unsigned char>(lhs)) ==
                                std::tolower(static_cast<unsigned char>(rhs));
                       }) != text.end();
  }

  std::string get_string_or_empty(int index) const {
    const char* value = lua_->GetString(index);
    return value == nullptr ? std::string() : std::string(value);
  }

  void pcall_checked(int args, int results, const char* context) {
    if (lua_->PCall(args, results, 0) == 0) {
      return;
    }

    std::string error = get_string_or_empty(-1);
    lua_->Pop(1);

    throw std::runtime_error(
        std::string(context) + ": " +
        (error.empty() ? "unknown lua runtime error" : error));
  }

  bool push_ents_table() {
    lua_->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua_->GetField(-1, "ents");
    if (!is_type(-1, "table")) {
      lua_->Pop(2);
      return false;
    }

    lua_->Remove(-2);  // Remove global table.
    return true;
  }

  bool push_entity_by_index(int entity_index) {
    if (entity_index <= 0 || !push_ents_table()) {
      return false;
    }

    const int ents_table = abs_index(-1);
    lua_->GetField(ents_table, "GetByIndex");
    if (!is_type(-1, "function")) {
      lua_->Pop(2);
      return false;
    }

    lua_->PushNumber(static_cast<double>(entity_index));
    pcall_checked(1, 1, "ents.GetByIndex");

    lua_->Remove(-2);  // Remove ents table.
    return !is_type(-1, "nil");
  }

  bool invoke_entity_method(int entity_index, const char* method_name,
                            int args, int results) {
    lua_->GetField(entity_index, method_name);
    if (!is_type(-1, "function")) {
      lua_->Pop(1);
      return false;
    }

    lua_->Push(entity_index);
    pcall_checked(args + 1, results, method_name);
    return true;
  }

  bool try_get_entity_number(int entity_index, const char* method_name,
                             double& out) {
    try {
      if (!invoke_entity_method(entity_index, method_name, 0, 1)) {
        return false;
      }

      if (!is_type(-1, "number")) {
        lua_->Pop(1);
        return false;
      }

      out = lua_->GetNumber(-1);
      lua_->Pop(1);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool try_get_entity_bool(int entity_index, const char* method_name,
                           bool& out) {
    try {
      if (!invoke_entity_method(entity_index, method_name, 0, 1)) {
        return false;
      }

      if (!is_type(-1, "bool")) {
        lua_->Pop(1);
        return false;
      }

      out = lua_->GetBool(-1);
      lua_->Pop(1);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool try_get_entity_string(int entity_index, const char* method_name,
                             std::string& out) {
    try {
      if (!invoke_entity_method(entity_index, method_name, 0, 1)) {
        return false;
      }

      if (!is_type(-1, "string")) {
        lua_->Pop(1);
        return false;
      }

      out = get_string_or_empty(-1);
      lua_->Pop(1);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool try_get_entity_vector(int entity_index, const char* method_name,
                            picojson::array& out) {
    try {
      if (!invoke_entity_method(entity_index, method_name, 0, 1)) {
        return false;
      }

      if (!lua_->IsType(-1, GarrysMod::Lua::Type::Vector)) {
        lua_->Pop(1);
        return false;
      }

      const Vector& vec = lua_->GetVector(-1);
      out = {picojson::value(static_cast<double>(vec.x)),
             picojson::value(static_cast<double>(vec.y)),
             picojson::value(static_cast<double>(vec.z))};
      lua_->Pop(1);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool try_get_entity_angle(int entity_index, const char* method_name,
                            picojson::array& out) {
    try {
      if (!invoke_entity_method(entity_index, method_name, 0, 1)) {
        return false;
      }

      if (!lua_->IsType(-1, GarrysMod::Lua::Type::Angle)) {
        lua_->Pop(1);
        return false;
      }

      const QAngle& ang = lua_->GetAngle(-1);
      out = {picojson::value(static_cast<double>(ang.x)),
             picojson::value(static_cast<double>(ang.y)),
             picojson::value(static_cast<double>(ang.z))};
      lua_->Pop(1);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool try_invoke_entity_angle_setter(int entity_index, const char* method_name,
                                      double pitch, double yaw,
                                      double roll) {
    try {
      lua_->GetField(entity_index, method_name);
      if (!is_type(-1, "function")) {
        lua_->Pop(1);
        return false;
      }

      lua_->Push(entity_index);

      QAngle ang;
      ang.x = static_cast<float>(pitch);
      ang.y = static_cast<float>(yaw);
      ang.z = static_cast<float>(roll);
      lua_->PushAngle(ang);

      pcall_checked(2, 0, method_name);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  static std::string format_number(double value) {
    if (!std::isfinite(value)) {
      return "nan";
    }

    std::ostringstream stream;
    stream.precision(6);
    stream << std::fixed << value;
    std::string formatted = stream.str();
    while (!formatted.empty() && formatted.back() == '0') {
      formatted.pop_back();
    }
    if (!formatted.empty() && formatted.back() == '.') {
      formatted.pop_back();
    }
    return formatted.empty() ? "0" : formatted;
  }

  static std::string format_vec3(double x, double y, double z) {
    return "[" + format_number(x) + ", " + format_number(y) + ", " +
           format_number(z) + "]";
  }

  bool build_entity_table_entry(const std::string& key, int value_index,
                                const std::string& filter,
                                picojson::object& out_entry) {
    std::string display;
    bool editable = false;
    picojson::value raw_value;
    bool has_raw_value = false;

    if (is_type(value_index, "number")) {
      const double number = lua_->GetNumber(value_index);
      display = format_number(number);
      if (std::isfinite(number)) {
        editable = true;
        raw_value = picojson::value(number);
        has_raw_value = true;
      }
    } else if (is_type(value_index, "string")) {
      display = get_string_or_empty(value_index);
      editable = true;
      raw_value = picojson::value(display);
      has_raw_value = true;
    } else if (is_type(value_index, "bool")) {
      const bool value = lua_->GetBool(value_index);
      display = value ? "true" : "false";
      editable = true;
      raw_value = picojson::value(value);
      has_raw_value = true;
    } else if (lua_->IsType(value_index, GarrysMod::Lua::Type::Vector)) {
      const Vector& vec = lua_->GetVector(value_index);
      display = format_vec3(vec.x, vec.y, vec.z);
    } else if (lua_->IsType(value_index, GarrysMod::Lua::Type::Angle)) {
      const QAngle& ang = lua_->GetAngle(value_index);
      display = format_vec3(ang.x, ang.y, ang.z);
    } else if (lua_->IsType(value_index, GarrysMod::Lua::Type::Entity)) {
      display = "<entity>";
    } else if (is_type(value_index, "nil")) {
      display = "nil";
    } else if (is_type(value_index, "table")) {
      display = "<table>";
    } else if (is_type(value_index, "function")) {
      display = "<function>";
    } else if (is_type(value_index, "userdata")) {
      display = "<userdata>";
    } else if (is_type(value_index, "thread")) {
      display = "<thread>";
    } else {
      display = "<" + std::string(lua_->GetTypeName(lua_->GetType(value_index))) + ">";
    }

    if (!filter.empty() && !contains_case_insensitive(key, filter) &&
        !contains_case_insensitive(display, filter)) {
      return false;
    }

    out_entry["key"] = picojson::value(key);
    out_entry["display"] = picojson::value(display);
    out_entry["editable"] = picojson::value(editable);
    if (has_raw_value) {
      out_entry["value"] = raw_value;
    }
    return true;
  }

  static std::string read_class_name(const picojson::object& summary) {
    const auto match = summary.find("class");
    if (match == summary.end() || !match->second.is<std::string>()) {
      return std::string();
    }

    return match->second.get<std::string>();
  }

  static bool matches_class_filter(const picojson::object& summary,
                                   const std::string& filter_class) {
    if (filter_class.empty()) {
      return true;
    }

    return contains_case_insensitive(read_class_name(summary), filter_class);
  }

  picojson::object build_entity_summary(int entity_index) {
    const int entity = abs_index(entity_index);

    picojson::object summary;
    summary["index"] = picojson::value(0.0);
    summary["class"] = picojson::value(std::string());
    summary["model"] = picojson::value(std::string());
    summary["valid"] = picojson::value(false);
    summary["pos"] = picojson::value(
        picojson::array{picojson::value(0.0), picojson::value(0.0),
                        picojson::value(0.0)});
    summary["angles"] = picojson::value(
        picojson::array{picojson::value(0.0), picojson::value(0.0),
                        picojson::value(0.0)});

    // Verify this is actually an Entity type before calling methods on it.
    if (!lua_->IsType(entity, GarrysMod::Lua::Type::Entity)) {
      return summary;
    }

    double ent_index = 0.0;
    if (try_get_entity_number(entity, "EntIndex", ent_index)) {
      summary["index"] = picojson::value(ent_index);
    }

    bool valid = false;
    if (try_get_entity_bool(entity, "IsValid", valid)) {
      summary["valid"] = picojson::value(valid);
    }

    // Skip expensive method calls for invalid entities.
    if (!valid) {
      return summary;
    }

    std::string class_name;
    if (try_get_entity_string(entity, "GetClass", class_name)) {
      summary["class"] = picojson::value(class_name);
    }

    std::string model_name;
    if (try_get_entity_string(entity, "GetModel", model_name)) {
      summary["model"] = picojson::value(model_name);
    }

    picojson::array position{picojson::value(0.0), picojson::value(0.0),
                             picojson::value(0.0)};
    if (try_get_entity_vector(entity, "GetPos", position)) {
      summary["pos"] = picojson::value(position);
    }

    picojson::array angles{picojson::value(0.0), picojson::value(0.0),
                           picojson::value(0.0)};
    if (try_get_entity_angle(entity, "GetAngles", angles)) {
      summary["angles"] = picojson::value(angles);
    }

    return summary;
  }

  bool push_table_get_keys(int entity_index) {
    if (!is_type(entity_index, "table")) {
      return false;
    }

    lua_->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua_->GetField(-1, "table");
    if (!is_type(-1, "table")) {
      lua_->Pop(2);
      return false;
    }

    const int table_lib = abs_index(-1);
    lua_->GetField(table_lib, "GetKeys");
    if (!is_type(-1, "function")) {
      lua_->Pop(3);
      return false;
    }

    lua_->Push(entity_index);
    pcall_checked(1, 1, "table.GetKeys");

    if (!is_type(-1, "table")) {
      lua_->Pop(3);
      return false;
    }

    lua_->Remove(-2);  // Remove table library.
    lua_->Remove(-2);  // Remove global table.
    return true;
  }

  picojson::object read_entity_properties(int entity_index) {
    picojson::object properties;
    if (!push_table_get_keys(entity_index)) {
      return properties;
    }

    const int keys_table = abs_index(-1);
    lua_->PushNil();

    int captured = 0;
    while (lua_->Next(keys_table) != 0) {
      std::string key;
      if (is_type(-1, "string")) {
        key = get_string_or_empty(-1);
      }

      lua_->Pop(1);  // Pop keys table value (property name).

      if (key.empty()) {
        continue;
      }

      lua_->GetField(entity_index, key.c_str());
      if (is_type(-1, "number")) {
        properties[key] = picojson::value(lua_->GetNumber(-1));
        ++captured;
      } else if (is_type(-1, "string")) {
        properties[key] = picojson::value(get_string_or_empty(-1));
        ++captured;
      } else if (is_type(-1, "bool")) {
        properties[key] = picojson::value(lua_->GetBool(-1));
        ++captured;
      }
      lua_->Pop(1);

      if (captured >= kMaxDetailProperties) {
        lua_->Pop(1);  // Pop iterator key before exiting loop.
        break;
      }
    }

    lua_->Pop(1);  // Pop keys table.
    return properties;
  }

  bool push_json_primitive(const picojson::value& value) {
    if (value.is<std::string>()) {
      lua_->PushString(value.get<std::string>().c_str());
      return true;
    }

    if (value.is<double>()) {
      const double number = value.get<double>();
      if (!std::isfinite(number)) {
        return false;
      }
      lua_->PushNumber(number);
      return true;
    }

    if (value.is<bool>()) {
      lua_->PushBool(value.get<bool>());
      return true;
    }

    return false;
  }

  bool push_json_value(const picojson::value& value) {
    if (push_json_primitive(value)) {
      return true;
    }

    if (value.is<picojson::array>()) {
      const picojson::array& arr = value.get<picojson::array>();
      if (arr.size() != 3 || !arr[0].is<double>() || !arr[1].is<double>() ||
          !arr[2].is<double>()) {
        return false;
      }

      const double x = arr[0].get<double>();
      const double y = arr[1].get<double>();
      const double z = arr[2].get<double>();
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
      }

      Vector vec;
      vec.x = static_cast<float>(x);
      vec.y = static_cast<float>(y);
      vec.z = static_cast<float>(z);
      lua_->PushVector(vec);
      return true;
    }

    return false;
  }

  static constexpr int kMaxPageLimit = 200;
  static constexpr int kMaxDetailProperties = 512;
  static constexpr int kMaxEntityTableEntries = 2048;

  GarrysMod::Lua::ILuaBase* lua_;
  int last_total_;
};
