#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <tier1/strtools.h>
#include <utility>

class error_aggregator {
 public:
  std::pair<std::string, int> add_error(const std::string& message) {
    const std::string normalized = normalize_message(message);
    const std::string fingerprint = to_fingerprint(normalized);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = errors_[fingerprint];
    if (entry.count == 0) {
      entry.message = trim(message);
    }
    ++entry.count;
    entry.last_seen = std::chrono::steady_clock::now();
    return std::make_pair(fingerprint, entry.count);
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    errors_.clear();
  }

 private:
  struct error_entry {
    std::string message;
    int count = 0;
    std::chrono::steady_clock::time_point last_seen;
  };

  static std::string trim(const std::string& input) {
    size_t first = 0;
    while (first < input.size() && V_isspace(input[first])) {
      ++first;
    }
    if (first == input.size()) {
      return std::string();
    }

    size_t last = input.size();
    while (last > first && V_isspace(input[last - 1])) {
      --last;
    }

    return input.substr(first, last - first);
  }

  static std::string normalize_message(const std::string& raw) {
    std::string normalized = trim(raw);

    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });

    normalized = std::regex_replace(
        normalized,
        std::regex(R"((?:[a-zA-Z]:)?(?:[\\/][^:\s]+)+)", std::regex::icase),
        "<path>");
    normalized = std::regex_replace(
        normalized, std::regex(R"(@?[^:\s]+\.lua)", std::regex::icase),
        "<path>");
    normalized = std::regex_replace(
        normalized,
        std::regex(
            R"(^\s*(?:[a-zA-Z]:)?[^:\r\n]*[\\/][^:\r\n]*:\s*)",
            std::regex::icase),
        "<path>: ");
    normalized = std::regex_replace(
        normalized,
        std::regex(R"(^\s*[^:\r\n]+\.lua:\s*)", std::regex::icase),
        "<path>: ");

    normalized = std::regex_replace(
        normalized, std::regex(R"(\bline\s+\d+\b)", std::regex::icase),
        "line N");
    normalized =
        std::regex_replace(normalized, std::regex(R"(:\d+:)"), ":N:");
    normalized =
        std::regex_replace(normalized, std::regex(R"(\[\d+\])"), "[N]");

    return trim(normalized);
  }

  static std::string to_fingerprint(const std::string& normalized_message) {
    std::string fingerprint;
    fingerprint.reserve(normalized_message.size());

    bool prev_dash = false;
    for (const unsigned char c : normalized_message) {
      if (V_isalnum(c)) {
        fingerprint.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        prev_dash = false;
      } else if (!prev_dash) {
        fingerprint.push_back('-');
        prev_dash = true;
      }
    }

    while (!fingerprint.empty() && fingerprint.front() == '-') {
      fingerprint.erase(fingerprint.begin());
    }
    while (!fingerprint.empty() && fingerprint.back() == '-') {
      fingerprint.pop_back();
    }

    if (fingerprint.empty()) {
      return "unknown-error";
    }

    return fingerprint;
  }

  std::map<std::string, error_entry> errors_;
  std::mutex mutex_;
};
