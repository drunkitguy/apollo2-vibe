/**
 * @file src/app_catalog_policy.h
 * @brief Pure application artwork and stable-ID catalog policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace proc::catalog {
  using byte_buffer_t = std::vector<std::uint8_t>;
  using image_reader_t = std::function<std::optional<byte_buffer_t>(const std::string &)>;

  bool has_png_signature(std::span<const std::uint8_t> bytes);
  std::string asset_path(const std::string &assets_root, const std::string &relative_path);
  std::string validate_image_path(
    std::string image_path,
    const std::string &assets_root,
    const std::string &default_image,
    const image_reader_t &read_image);

  std::tuple<std::string, std::string> calculate_ids(
    const std::string &app_name,
    const std::string &app_uuid,
    const std::string &legacy_image_identity,
    int index);
  std::tuple<std::string, std::string> calculate_versioned_ids(
    const std::string &app_uuid,
    const std::string &cover_fingerprint,
    int index);

  struct alias_state_t {
    std::string current_id;
    std::string cover_fingerprint;
    std::set<std::string> aliases;
  };

  struct app_identity_t {
    std::string name;
    std::string uuid;
    std::string legacy_image_identity;
    std::string art_version;
    std::string id;
    std::vector<std::string> aliases;
  };

  void assign_compatible_id(
    app_identity_t &app,
    int index,
    std::set<std::string> &occupied_ids,
    std::map<std::string, alias_state_t> &persisted,
    std::set<std::string> &active_uuids,
    bool &state_changed);
  void prune_and_filter_aliases(
    std::vector<app_identity_t> &apps,
    std::map<std::string, alias_state_t> &persisted,
    const std::set<std::string> &active_uuids,
    bool &state_changed);
  std::optional<std::size_t> resolve_app(
    const std::vector<app_identity_t> &apps,
    std::string app_id,
    const std::string &app_uuid = {});

  /// Longest platform name or id reported to clients, in bytes.
  inline constexpr std::size_t kMaxPlatformLength = 64;

  /// Platform group of an app as reported in /applist; both empty when unknown.
  struct platform_t {
    std::string name;
    std::string id;
  };

  /// Raw apps.json values that decide an app's platform. Strings are empty when
  /// the key is missing or not a string.
  struct platform_source_t {
    std::string manual_name;  ///< "platform": set by hand, always wins.
    std::string manual_id;  ///< "platform-id": optional companion of "platform".
    std::string playnite_name;  ///< "playnite-platform": written by the Playnite sync.
    std::string playnite_id;  ///< "playnite-platform-id": written by the Playnite sync.
    bool playnite_fullscreen = false;  ///< The app launches Playnite itself.
    bool has_cmd = false;  ///< The app has a non-empty "cmd".
    bool has_detached = false;  ///< The app has at least one non-empty "detached" command.
    bool has_playnite_game = false;  ///< The app has a non-empty "playnite-id".
  };

  /// The group built-in entries (Desktop, Virtual Display, Remote Input, Terminate) report.
  platform_t apps_platform();

  /// Trims whitespace, drops control characters and caps the value at
  /// kMaxPlatformLength bytes without splitting a UTF-8 sequence.
  std::string sanitize_platform_value(std::string_view value);

  /**
   * Resolves an app's platform: the manual "platform" / "platform-id" keys,
   * else the Playnite sync values, else "Apps" for the Playnite launcher and
   * for apps that run neither a command nor a detached command and are not
   * Playnite games (Desktop style entries), else nothing.
   */
  platform_t resolve_platform(const platform_source_t &source);

  /// apps.json keys that hold an app's platform group.
  inline constexpr const char *kPlatformKeys[] = {"platform", "platform-id", "playnite-platform", "playnite-platform-id"};

  /**
   * Keeps the platform keys of an app that is being replaced by an edited copy.
   *
   * Editors that only send the fields they know would otherwise drop them. A
   * key present in the edited copy wins; an explicit null removes it.
   */
  void carry_over_platform_keys(const nlohmann::json &existing, nlohmann::json &edited) noexcept;
}  // namespace proc::catalog
