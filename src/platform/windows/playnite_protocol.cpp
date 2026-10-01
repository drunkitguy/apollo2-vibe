/**
 * @file src/platform/windows/playnite_protocol.cpp
 */

#include "playnite_protocol.h"

#ifndef SUNSHINE_TESTS
  #include "src/logging.h"
#endif

#include <nlohmann/json.hpp>
#include <string>

using nlohmann::json;

namespace platf::playnite {

  static std::vector<std::string> to_string_list(const json &j) {
    std::vector<std::string> out;
    if (!j.is_array()) {
      return out;
    }
    out.reserve(j.size());
    for (auto &v : j) {
      if (v.is_string()) {
        out.emplace_back(v.get<std::string>());
      }
    }
    return out;
  }

  // The helpers below read the platform fields added in connector 0.5.0. They
  // never throw: a malformed value only loses that value, never the game.
  static std::string lenient_string(const json &obj, const char *key) {
    try {
      if (!obj.is_object()) {
        return {};
      }
      const auto it = obj.find(key);
      if (it != obj.end() && it->is_string()) {
        return it->get<std::string>();
      }
    } catch (...) {}
    return {};
  }

  static bool lenient_bool(const json &obj, const char *key) {
    try {
      if (!obj.is_object()) {
        return false;
      }
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return false;
      }
      if (it->is_boolean()) {
        return it->get<bool>();
      }
      if (it->is_number_integer()) {
        return it->get<long long>() != 0;
      }
      if (it->is_string()) {
        const auto text = it->get<std::string>();
        return text == "true" || text == "True" || text == "TRUE" || text == "1";
      }
    } catch (...) {}
    return false;
  }

  // PowerShell serializes a one-element array as the element itself, so a
  // single string is accepted as a list of one.
  static std::vector<std::string> lenient_string_list(const json &obj, const char *key) {
    std::vector<std::string> out;
    try {
      if (!obj.is_object()) {
        return out;
      }
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return out;
      }
      if (it->is_string()) {
        if (!it->get_ref<const std::string &>().empty()) {
          out.emplace_back(it->get<std::string>());
        }
        return out;
      }
      if (!it->is_array()) {
        return out;
      }
      for (const auto &v : *it) {
        if (v.is_string() && !v.get_ref<const std::string &>().empty()) {
          out.emplace_back(v.get<std::string>());
        }
      }
    } catch (...) {}
    return out;
  }

  static bool parse_platform_ref(const json &v, PlatformRef &out) {
    try {
      if (v.is_string()) {
        out.name = v.get<std::string>();
      } else if (v.is_object()) {
        out.name = lenient_string(v, "name");
        out.spec_id = lenient_string(v, "specId");
      }
    } catch (...) {}
    return !out.name.empty() || !out.spec_id.empty();
  }

  // Accepts an array of {name, specId} objects or plain strings, a single
  // object, or a single string.
  static std::vector<PlatformRef> lenient_platform_list(const json &obj, const char *key) {
    std::vector<PlatformRef> out;
    try {
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return out;
      }
      if (it->is_array()) {
        for (const auto &v : *it) {
          PlatformRef ref;
          if (parse_platform_ref(v, ref)) {
            out.emplace_back(std::move(ref));
          }
        }
      } else {
        PlatformRef ref;
        if (parse_platform_ref(*it, ref)) {
          out.emplace_back(std::move(ref));
        }
      }
    } catch (...) {}
    return out;
  }

  static void parse_platform_fields(const json &g, Game &game) {
    try {
      if (!g.is_object()) {
        return;
      }
      // Older connectors never send the key; its presence (even as null or an
      // empty array) is what marks the game as carrying platform data.
      game.has_platform_info = g.contains("platforms");
      game.platforms = lenient_platform_list(g, "platforms");
      game.source_name = lenient_string(g, "source");
      game.emulated = lenient_bool(g, "emulated");
      game.emulator_name = lenient_string(g, "emulatorName");
      game.emulator_builtin_id = lenient_string(g, "emulatorBuiltInId");
      game.emulator_platforms = lenient_string_list(g, "emulatorPlatforms");
    } catch (...) {
      game.has_platform_info = false;
      game.platforms.clear();
      game.source_name.clear();
      game.emulated = false;
      game.emulator_name.clear();
      game.emulator_builtin_id.clear();
      game.emulator_platforms.clear();
    }
  }

  Message parse(std::span<const uint8_t> bytes) {
    Message m;
    if (bytes.empty()) {
      return m;
    }
    std::span<const uint8_t> trimmed = bytes;
    if (trimmed.size() >= 3 && trimmed[0] == 0xEF && trimmed[1] == 0xBB && trimmed[2] == 0xBF) {
      trimmed = trimmed.subspan(3);
    }
    if (trimmed.empty()) {
      return m;
    }
    try {
      json j = json::parse(trimmed.begin(), trimmed.end());
      const std::string type = j.value("type", "");
#ifndef SUNSHINE_TESTS
      // Verbose protocol tracing: use debug to reduce noise in normal operation.
      BOOST_LOG(debug) << "Playnite protocol: parsing message type='" << type << "'";
#endif
      if (type == "categories") {
        m.type = MessageType::Categories;
        auto arr = j.value("payload", json::array());
#ifndef SUNSHINE_TESTS
        BOOST_LOG(debug) << "Playnite protocol: categories count=" << arr.size();
#endif
        for (auto &c : arr) {
          Category cat;
          cat.id = c.value("id", "");
          cat.name = c.value("name", "");
          if (!cat.id.empty() || !cat.name.empty()) {
            m.categories.emplace_back(std::move(cat));
          }
        }
      } else if (type == "plugins") {
        m.type = MessageType::Plugins;
        auto arr = j.value("payload", json::array());
#ifndef SUNSHINE_TESTS
        BOOST_LOG(debug) << "Playnite protocol: plugins count=" << arr.size();
#endif
        for (auto &p : arr) {
          Plugin plug;
          plug.id = p.value("id", "");
          plug.name = p.value("name", "");
          if (!plug.id.empty() || !plug.name.empty()) {
            m.plugins.emplace_back(std::move(plug));
          }
        }
      } else if (type == "games") {
        m.type = MessageType::Games;
        auto arr = j.value("payload", json::array());
#ifndef SUNSHINE_TESTS
        BOOST_LOG(debug) << "Playnite protocol: games count=" << arr.size();
#endif
        for (auto &g : arr) {
          Game game;
          game.id = g.value("id", "");
          game.name = g.value("name", "");
          game.exe = g.value("exe", "");
          game.args = g.value("args", "");
          game.working_dir = g.value("workingDir", "");
          game.install_dir = g.value("installDir", "");
          game.categories = to_string_list(g.value("categories", json::array()));
          game.plugin_id = g.value("pluginId", "");
          game.plugin_name = g.value("pluginName", "");
          // playtimeMinutes may arrive as number or string
          try {
            game.playtime_minutes = g.value("playtimeMinutes", (uint64_t) 0);
          } catch (...) {
            try {
              std::string pm = g.value("playtimeMinutes", std::string());
              if (!pm.empty()) {
                game.playtime_minutes = std::stoull(pm);
              }
            } catch (...) {}
          }
          game.last_played = g.value("lastPlayed", "");
          game.box_art_path = g.value("boxArtPath", "");
          game.icon_path = g.value("iconPath", "");
          game.description = g.value("description", "");
          game.tags = to_string_list(g.value("tags", json::array()));
          // Installed flag may be provided as 'installed' or 'isInstalled'.
          // If neither field is present, assume installed=true to avoid filtering out everything.
          bool has1 = g.contains("installed");
          bool has2 = g.contains("isInstalled");
          bool inst = false;
          if (has1) {
            inst = g.value("installed", false);
          }
          if (has2) {
            inst = inst || g.value("isInstalled", false);
          }
          if (!has1 && !has2) {
            inst = true;
          }
          game.installed = inst;
          parse_platform_fields(g, game);
          if (!game.id.empty()) {
            m.games.emplace_back(std::move(game));
          }
        }
      } else if (type == "snapshotStart") {
        m.type = MessageType::SnapshotStart;
      } else if (type == "snapshotComplete") {
        m.type = MessageType::SnapshotComplete;
      } else if (type == "commandResult") {
        m.type = MessageType::CommandResult;
        m.command_name = j.value("command", "");
        m.command_request_id = j.value("requestId", "");
        m.command_success = j.value("success", false);
        m.command_error = j.value("error", "");
      } else if (type == "status") {
        m.type = MessageType::Status;
        const auto &st = j.value("status", json::object());
        m.status_name = st.value("name", "");
        m.status_game_id = st.value("id", "");
        m.status_install_dir = st.value("installDir", "");
        m.status_exe = st.value("exe", "");
#ifndef SUNSHINE_TESTS
        BOOST_LOG(debug) << "Playnite protocol: status name='" << m.status_name << "' id='" << m.status_game_id << "'";
#endif
      }
    } catch (...) {
#ifndef SUNSHINE_TESTS
      BOOST_LOG(warning) << "Playnite protocol: failed to parse message";
#endif
      // fallthrough unknown
    }
    return m;
  }

}  // namespace platf::playnite
