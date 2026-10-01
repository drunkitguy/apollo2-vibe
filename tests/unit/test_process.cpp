/**
 * @file tests/unit/test_process.cpp
 * @brief Deterministic application catalog, artwork, and lifecycle policy tests.
 */
#include "../tests_common.h"

#include <src/app_catalog_policy.h>
#include <src/deferred_action.h>

#include <algorithm>
#include <map>

namespace {
  using proc::catalog::alias_state_t;
  using proc::catalog::app_identity_t;

  proc::catalog::byte_buffer_t png(std::uint8_t payload = 0) {
    return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, payload};
  }

  struct fake_images_t {
    std::map<std::string, proc::catalog::byte_buffer_t> files;

    std::optional<proc::catalog::byte_buffer_t> read(const std::string &path) const {
      const auto found = files.find(path);
      return found == files.end() ? std::nullopt : std::optional {found->second};
    }
  };

  TEST(ProcessLifecycle, DeferredActionPersistsUntilConsumedOrCleared) {
    lifecycle::deferred_action_t action;
    action.defer();
    EXPECT_TRUE(action.consume());
    EXPECT_FALSE(action.consume());

    action.defer();
    action.clear();
    EXPECT_FALSE(action.consume());
  }

  TEST(ProcessArtwork, PngSignatureRequiresAllEightBytes) {
    EXPECT_TRUE(proc::catalog::has_png_signature(png()));
    EXPECT_FALSE(proc::catalog::has_png_signature({}));
    EXPECT_FALSE(proc::catalog::has_png_signature(proc::catalog::byte_buffer_t {0x89, 0x50, 0x4e, 0x47}));
    EXPECT_FALSE(proc::catalog::has_png_signature(proc::catalog::byte_buffer_t(8, 0)));
  }

  TEST(ProcessArtwork, ValidationUsesInjectedAssetsAndNeverTouchesDisk) {
    const std::string assets = "virtual-assets";
    const std::string fallback = "virtual-assets/box.png";
    const auto asset_cover = proc::catalog::asset_path(assets, "cover.png");
    fake_images_t images {{{asset_cover, png(1)}, {"custom.PNG", png(2)}, {"bad.png", {1, 2, 3}}}};
    const auto read = [&](const std::string &path) { return images.read(path); };

    EXPECT_EQ(proc::catalog::validate_image_path("", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("cover.jpg", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("missing.png", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("bad.png", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("cover.png", assets, fallback, read), asset_cover);
    EXPECT_EQ(proc::catalog::validate_image_path("custom.PNG", assets, fallback, read), "custom.PNG");
    EXPECT_EQ(
      proc::catalog::validate_image_path("./assets/steam.png", assets, fallback, read),
      proc::catalog::asset_path(assets, "steam.png"));
  }

  TEST(ProcessCatalog, FirstSeenUuidSeedsStableUuidOnlyId) {
    app_identity_t app {"Game", "11111111-1111-1111-1111-111111111111", "ignored-cover", "sha256:first"};
    std::set<std::string> ids;
    std::set<std::string> active;
    std::map<std::string, alias_state_t> state;
    bool changed = false;

    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_EQ(app.id, std::get<0>(proc::catalog::calculate_ids(app.name, app.uuid, {}, 0)));
    EXPECT_EQ(app.art_version, "sha256:first");
    EXPECT_TRUE(app.aliases.empty());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, CoverChangeRotatesCurrentIdAndRetainsOldAlias) {
    const std::string uuid = "22222222-2222-2222-2222-222222222222";
    std::map<std::string, alias_state_t> state;
    std::set<std::string> ids;
    std::set<std::string> active;
    bool changed = false;
    app_identity_t first {"Game", uuid, {}, "sha256:first"};
    proc::catalog::assign_compatible_id(first, 0, ids, state, active, changed);

    ids.clear();
    active.clear();
    changed = false;
    app_identity_t second {"Game", uuid, {}, "sha256:second"};
    proc::catalog::assign_compatible_id(second, 0, ids, state, active, changed);
    EXPECT_NE(second.id, first.id);
    EXPECT_NE(std::find(second.aliases.begin(), second.aliases.end(), first.id), second.aliases.end());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, DeletedAppsAndConflictingAliasesArePruned) {
    std::map<std::string, alias_state_t> state {
      {"active-a", {"101", "a", {"202", "999"}}},
      {"active-b", {"202", "b", {"999"}}},
      {"deleted", {"303", "c", {"404"}}}
    };
    std::vector<app_identity_t> apps {
      {"A", "active-a", {}, "a", "101", {"202", "999"}},
      {"B", "active-b", {}, "b", "202", {"999"}}
    };
    bool changed = false;
    proc::catalog::prune_and_filter_aliases(apps, state, {"active-a", "active-b"}, changed);

    EXPECT_FALSE(state.contains("deleted"));
    EXPECT_TRUE(apps[0].aliases.empty());
    EXPECT_TRUE(apps[1].aliases.empty());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, ResolverPrefersUuidAndRejectsAmbiguousAliases) {
    const std::vector<app_identity_t> apps {
      {"A", "uuid-a", {}, "a", "101", {"999"}},
      {"B", "uuid-b", {}, "b", "202", {"999"}}
    };
    EXPECT_EQ(proc::catalog::resolve_app(apps, "101"), 0u);
    EXPECT_EQ(proc::catalog::resolve_app(apps, "bad", "uuid-b"), 1u);
    EXPECT_FALSE(proc::catalog::resolve_app(apps, "999").has_value());
    EXPECT_FALSE(proc::catalog::resolve_app(apps, "0").has_value());
  }

  TEST(ProcessCatalog, LegacyAppsIncludeImageIdentityAndIndexFallback) {
    const auto first = proc::catalog::calculate_ids("Legacy", "", "cover-a", 0);
    const auto second = proc::catalog::calculate_ids("Legacy", "", "cover-b", 0);
    const auto indexed = proc::catalog::calculate_ids("Legacy", "", "cover-a", 1);
    EXPECT_NE(std::get<0>(first), std::get<0>(second));
    EXPECT_NE(std::get<1>(first), std::get<1>(indexed));
  }

  proc::catalog::platform_source_t game_with_command() {
    proc::catalog::platform_source_t source;
    source.has_cmd = true;
    return source;
  }

  TEST(ProcessPlatform, ManualPlatformWinsOverPlayniteData) {
    auto source = game_with_command();
    source.manual_name = "My Group";
    source.manual_id = "my_group";
    source.playnite_name = "Nintendo Switch";
    source.playnite_id = "nintendo_switch";
    const auto platform = proc::catalog::resolve_platform(source);
    EXPECT_EQ(platform.name, "My Group");
    EXPECT_EQ(platform.id, "my_group");
  }

  TEST(ProcessPlatform, ManualPlatformWithoutIdDoesNotBorrowPlayniteId) {
    proc::catalog::platform_source_t source;
    source.has_playnite_game = true;
    source.manual_name = "  Handhelds  ";
    source.playnite_name = "Nintendo 3DS";
    source.playnite_id = "nintendo_3ds";
    const auto platform = proc::catalog::resolve_platform(source);
    EXPECT_EQ(platform.name, "Handhelds");
    EXPECT_TRUE(platform.id.empty());
  }

  TEST(ProcessPlatform, BlankManualPlatformFallsThroughToPlaynite) {
    proc::catalog::platform_source_t source;
    source.has_playnite_game = true;
    source.manual_name = "   ";
    source.playnite_name = "Nintendo GameCube";
    source.playnite_id = "nintendo_gamecube";
    const auto platform = proc::catalog::resolve_platform(source);
    EXPECT_EQ(platform.name, "Nintendo GameCube");
    EXPECT_EQ(platform.id, "nintendo_gamecube");
  }

  TEST(ProcessPlatform, PlayniteGameWithoutPlatformDataIsUnknown) {
    // An outdated Playnite connector stores no platform; the app must not be
    // mislabeled as a utility just because synced games carry no cmd.
    proc::catalog::platform_source_t source;
    source.has_playnite_game = true;
    const auto platform = proc::catalog::resolve_platform(source);
    EXPECT_TRUE(platform.name.empty());
    EXPECT_TRUE(platform.id.empty());
  }

  TEST(ProcessPlatform, AppsWithoutCommandAreApps) {
    const auto platform = proc::catalog::resolve_platform({});
    EXPECT_EQ(platform.name, "Apps");
    EXPECT_EQ(platform.id, "apps");
  }

  TEST(ProcessPlatform, PlayniteLauncherIsApps) {
    auto source = game_with_command();
    source.playnite_fullscreen = true;
    const auto platform = proc::catalog::resolve_platform(source);
    EXPECT_EQ(platform.name, "Apps");
    EXPECT_EQ(platform.id, "apps");
  }

  TEST(ProcessPlatform, CommandWithoutPlatformDataIsUnknown) {
    const auto platform = proc::catalog::resolve_platform(game_with_command());
    EXPECT_TRUE(platform.name.empty());
    EXPECT_TRUE(platform.id.empty());
  }

  TEST(ProcessPlatform, BuiltInEntriesReportApps) {
    const auto platform = proc::catalog::apps_platform();
    EXPECT_EQ(platform.name, "Apps");
    EXPECT_EQ(platform.id, "apps");
  }

  TEST(ProcessPlatform, ValuesAreTrimmedCleanedAndCapped) {
    EXPECT_EQ(proc::catalog::sanitize_platform_value("\t Example\nSystem \r\n"), "Example System");
    EXPECT_EQ(proc::catalog::sanitize_platform_value(std::string {'A', '\x01', 'B'}), "AB");
    EXPECT_EQ(proc::catalog::sanitize_platform_value(""), "");
    const std::string long_value(100, 'x');
    EXPECT_EQ(proc::catalog::sanitize_platform_value(long_value).size(), proc::catalog::kMaxPlatformLength);
    // 63 ASCII bytes followed by a two byte UTF-8 sequence: the cut must not split it.
    const std::string utf8 = std::string(63, 'a') + "\xC3\xA9" + "tail";
    const auto capped = proc::catalog::sanitize_platform_value(utf8);
    EXPECT_EQ(capped, std::string(63, 'a'));
    const std::string exact = std::string(62, 'a') + "\xC3\xA9";
    EXPECT_EQ(proc::catalog::sanitize_platform_value(exact), exact);
  }
}  // namespace
