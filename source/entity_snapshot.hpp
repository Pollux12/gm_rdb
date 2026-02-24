#pragma once

#include <GarrysMod/InterfacePointers.hpp>

#include <algorithm>
#include <cctype>
#include <string>

#include <eiface.h>
#include <edict.h>
#include <engine/ICollideable.h>
#include <iserverentity.h>
#include <picojson.h>
#include <string_t.h>

class EntitySnapshotQuery {
 public:
  EntitySnapshotQuery() : last_total_(0) {}

  picojson::array list_entities(int offset, int limit, int filter_id,
                                const std::string& filter_class) {
    picojson::array entities;
    last_total_ = 0;

    if (limit < 0) {
      return entities;
    }

    if (offset < 0) {
      offset = 0;
    }

    if (limit > kMaxPageLimit) {
      limit = kMaxPageLimit;
    }

    IVEngineServer* engine = InterfacePointers::VEngineServer();
    if (engine == nullptr) {
      return entities;
    }

    if (filter_id > 0) {
      picojson::object summary;
      if (!try_build_valid_summary(engine, filter_id, summary)) {
        return entities;
      }

      if (matches_class_filter(summary, filter_class)) {
        last_total_ = 1;
        if (offset == 0 && limit > 0) {
          entities.emplace_back(summary);
        }
      }

      return entities;
    }

    const int entity_count = engine->GetEntityCount();
    int matched = 0;

    for (int index = 1; index <= entity_count; ++index) {
      picojson::object summary;
      if (!try_build_valid_summary(engine, index, summary)) {
        continue;
      }

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
    detail["class"] = picojson::value(std::string());
    detail["model"] = picojson::value(std::string());
    detail["pos"] = picojson::value(default_triplet());
    detail["angles"] = picojson::value(default_triplet());
    detail["parent_index"] = picojson::value(-1.0);
    detail["health"] = picojson::value(0.0);
    detail["properties"] = picojson::value(picojson::object());

    if (entity_index <= 0) {
      return detail;
    }

    IVEngineServer* engine = InterfacePointers::VEngineServer();
    if (engine == nullptr) {
      return detail;
    }

    picojson::object summary;
    if (!try_build_valid_summary(engine, entity_index, summary)) {
      return detail;
    }

    detail = summary;
    detail["parent_index"] = picojson::value(-1.0);
    detail["health"] = picojson::value(0.0);
    detail["properties"] = picojson::value(picojson::object());
    return detail;
  }

  int last_total() const { return last_total_; }

 private:
  static constexpr int kMaxPageLimit = 200;

  static picojson::array default_triplet() {
    return picojson::array{picojson::value(0.0), picojson::value(0.0),
                           picojson::value(0.0)};
  }

  static std::string safe_string(const char* value) {
    return value == nullptr ? std::string() : std::string(value);
  }

  static picojson::array vector_to_array(const Vector& vec) {
    return picojson::array{picojson::value(static_cast<double>(vec.x)),
                           picojson::value(static_cast<double>(vec.y)),
                           picojson::value(static_cast<double>(vec.z))};
  }

  static picojson::array angles_to_array(const QAngle& ang) {
    return picojson::array{picojson::value(static_cast<double>(ang.x)),
                           picojson::value(static_cast<double>(ang.y)),
                           picojson::value(static_cast<double>(ang.z))};
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

  static bool matches_class_filter(const picojson::object& summary,
                                   const std::string& filter_class) {
    if (filter_class.empty()) {
      return true;
    }

    const auto class_match = summary.find("class");
    if (class_match == summary.end() ||
        !class_match->second.is<std::string>()) {
      return false;
    }

    return contains_case_insensitive(class_match->second.get<std::string>(),
                                     filter_class);
  }

  static picojson::object build_entity_summary(int index, edict_t* edict) {
    picojson::object summary;
    summary["index"] = picojson::value(static_cast<double>(index));
    summary["class"] = picojson::value(std::string());
    summary["model"] = picojson::value(std::string());
    summary["valid"] = picojson::value(false);
    summary["pos"] = picojson::value(default_triplet());
    summary["angles"] = picojson::value(default_triplet());

    if (index <= 0 || edict == nullptr || edict->IsFree()) {
      return summary;
    }

    summary["valid"] = picojson::value(true);
    summary["class"] = picojson::value(safe_string(edict->GetClassName()));

    IServerEntity* server_entity = edict->GetIServerEntity();
    if (server_entity != nullptr) {
      summary["model"] =
          picojson::value(safe_string(STRING(server_entity->GetModelName())));
    }

    ICollideable* collideable = edict->GetCollideable();
    if (collideable != nullptr) {
      summary["pos"] = picojson::value(
          vector_to_array(collideable->GetCollisionOrigin()));
      summary["angles"] =
          picojson::value(angles_to_array(collideable->GetCollisionAngles()));
    }

    return summary;
  }

  static bool try_build_valid_summary(IVEngineServer* engine, int index,
                                      picojson::object& summary) {
    if (engine == nullptr || index <= 0) {
      return false;
    }

    edict_t* edict = engine->PEntityOfEntIndex(index);
    if (edict == nullptr || edict->IsFree()) {
      return false;
    }

    summary = build_entity_summary(index, edict);
    const auto class_match = summary.find("class");
    if (class_match == summary.end() || !class_match->second.is<std::string>()) {
      return false;
    }

    const std::string class_name = class_match->second.get<std::string>();
    if (class_name.empty()) {
      return false;
    }

    return true;
  }

  int last_total_;
};
