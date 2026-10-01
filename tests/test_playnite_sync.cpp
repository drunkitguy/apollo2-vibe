#include "src/platform/windows/playnite_sync_policy.h"

#include <gtest/gtest.h>

using namespace platf::playnite;
using namespace platf::playnite::sync::policy;

TEST(PlayniteSync_TimeParse, ParsesZuluAndOffset) {
  std::time_t t1 = 0, t2 = 0, t3 = 0;
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19T12:34:56Z", t1));
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19T14:34:56+02:00", t2));
  EXPECT_TRUE(parse_iso8601_utc("2024-08-19 12:34:56", t3));
  EXPECT_EQ(t1, t2);  // +02 offset converts to same UTC
}

static Game make_game(std::string id, std::string last, bool installed = true, std::vector<std::string> cats = {}, std::string plugin = {}) {
  Game g;
  g.id = id;
  g.name = id;
  g.last_played = last;
  g.installed = installed;
  g.categories = cats;
  g.plugin_id = plugin;
  return g;
}

TEST(PlayniteSync_Select, RecentSelectionHonorsAgeAndExclude) {
  // Two games, one recent, one old; exclude the recent by id
  std::vector<Game> installed {
    make_game("A", "2024-08-19T12:34:56Z", true),
    make_game("B", "2020-01-01T00:00:00Z", true)
  };
  std::unordered_set<std::string> excl {to_lower_copy(std::string("a"))};
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 1, 30, 1724070896, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 0u);  // recent candidate excluded; no fallback
}

TEST(PlayniteSync_Select, RecentSelectionSkipsExcludedCategories) {
  std::vector<Game> installed {
    make_game("A", "2024-08-19T12:34:56Z", true, {"Steam"}),
    make_game("B", "2024-08-19T12:34:56Z", true, {"Indie"})
  };
  std::unordered_set<std::string> excl_ids;
  std::unordered_set<std::string> excl_categories {to_lower_copy(std::string("steam"))};
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 2, 0, 1724070896, excl_ids, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "B");
  EXPECT_EQ(flags["B"] & 0x1, 0x1);
}

TEST(PlayniteSync_Select, RecentSelectionSkipsExcludedPlugins) {
  std::vector<Game> installed {
    make_game("A", "2024-08-19T12:34:56Z", true, {}, "cb91dfc9-b977-43bf-8e70-55f46e410fab"),
    make_game("B", "2024-08-19T12:34:56Z", true, {}, "83dd83a4-0cf7-49fb-9138-8547f6b60c18")
  };
  std::unordered_set<std::string> excl_ids;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins {to_lower_copy(std::string("cb91dfc9-b977-43bf-8e70-55f46e410fab"))};
  std::unordered_map<std::string, int> flags;
  auto sel = select_recent_installed_games(installed, 2, 0, 1724070896, excl_ids, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "B");
}

TEST(PlayniteSync_Select, CategorySelectionMatchesAnyCategory) {
  std::vector<Game> installed {
    make_game("A", "2024-08-01T00:00:00Z", true, {"RPG", "Indie"}),
    make_game("B", "2024-08-01T00:00:00Z", true, {"Action"})
  };
  std::unordered_set<std::string> excl;
  std::unordered_set<std::string> excl_categories;
  std::unordered_set<std::string> excl_plugins;
  std::unordered_map<std::string, int> flags;
  std::vector<std::string> cats {"indie"};
  auto sel = select_category_games(installed, cats, excl, excl_categories, excl_plugins, flags);
  ASSERT_EQ(sel.size(), 1u);
  EXPECT_EQ(sel[0].id, "A");
  EXPECT_EQ(flags["A"] & 0x2, 0x2);
}

TEST(PlayniteSync_Purge, TTLAndReplacementPolicy) {
  // Setup a minimal apps.json and selection
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  // Existing auto app older than TTL and never played
  nlohmann::json app;
  app["playnite-id"] = "X";
  app["playnite-managed"] = "auto";
  app["playnite-added-at"] = "2000-01-01T00:00:00Z";
  root["apps"].push_back(app);
  std::unordered_set<std::string> uninstalled;  // not uninstalled
  constexpr std::time_t now = 1724070896;
  std::unordered_map<std::string, std::time_t> last_played;  // empty => never played
  std::unordered_set<std::string> selected_ids;  // not selected
  bool changed = false;
  purge_uninstalled_and_ttl(root, uninstalled, 1 /*days*/, now, last_played, true /*recent*/, true /*require repl*/, true /*remove uninstalled*/, false /*sync all*/, selected_ids, changed);
  EXPECT_TRUE(changed);
  EXPECT_EQ(root["apps"].size(), 0);
}

TEST(PlayniteSync_MetadataPolicy, KeepsBoxArtWhenIconResolutionFails) {
  nlohmann::json app = {
    {"playnite-icon-path", "previous-icon.png"}
  };

  apply_box_art_path(app, "covers/playnite_fallback-icon.png");
  apply_icon_path(app, {});

  EXPECT_EQ(app["image-path"].get<std::string>(), "covers/playnite_fallback-icon.png");
  EXPECT_FALSE(app.contains("playnite-icon-path"));
}

TEST(PlayniteSync_ArtPolicy, ReconvertOnlyWhenCacheDoesNotDescribeSource) {
  EXPECT_FALSE(should_reconvert_playnite_image(true, "cover-a|12|100", "cover-a|12|100"));
  EXPECT_TRUE(should_reconvert_playnite_image(false, "cover-a|12|100", "cover-a|12|100"));
  // A different source path forces reconversion even if its source mtime is older.
  EXPECT_TRUE(should_reconvert_playnite_image(true, "cover-a|12|100", "cover-b|12|1"));
  EXPECT_TRUE(should_reconvert_playnite_image(true, "cover-a|12|100", ""));
}

static Game platform_game(std::vector<PlatformRef> platforms, bool emulated = false, std::vector<std::string> emulator_platforms = {}, std::string builtin_id = {}, std::string emulator_name = {}) {
  Game g;
  g.id = "example-id";
  g.name = "Example Game";
  g.installed = true;
  g.has_platform_info = true;
  g.platforms = std::move(platforms);
  g.emulated = emulated;
  g.emulator_platforms = std::move(emulator_platforms);
  g.emulator_builtin_id = std::move(builtin_id);
  g.emulator_name = std::move(emulator_name);
  return g;
}

TEST(PlayniteSync_DerivePlatform, OldConnectorGivesNothing) {
  Game g;
  g.id = "example-id";
  g.name = "Example Game";
  g.emulated = true;  // ignored without platform info
  g.platforms = {{"Nintendo Switch", "nintendo_switch"}};
  EXPECT_TRUE(derive_platform(g).empty());
}

TEST(PlayniteSync_DerivePlatform, EmulatedMultiPlatformGamePrefersEmulatorPlatform) {
  // Metadata sources often attach PC and several consoles to one game.
  auto g = platform_game({{"PC (Windows)", "pc_windows"}, {"Nintendo 3DS", "nintendo_3ds"}, {"Nintendo Switch", "nintendo_switch"}}, true, {"nintendo_switch"}, "ryujinx", "Example Emulator");
  const auto guess = derive_platform(g);
  EXPECT_EQ(guess.name, "Nintendo Switch");
  EXPECT_EQ(guess.spec_id, "nintendo_switch");
}

TEST(PlayniteSync_DerivePlatform, NativeGameWithPcAndConsolePlatformsIsPc) {
  auto g = platform_game({{"Nintendo Switch", "nintendo_switch"}, {"PC (Windows)", "pc_windows"}});
  const auto guess = derive_platform(g);
  EXPECT_EQ(guess.name, "PC (Windows)");
  EXPECT_EQ(guess.spec_id, "pc_windows");
}

TEST(PlayniteSync_DerivePlatform, NativeGameWithoutPlatformsIsPc) {
  const auto guess = derive_platform(platform_game({}));
  EXPECT_EQ(guess.name, "PC (Windows)");
  EXPECT_EQ(guess.spec_id, "pc_windows");
}

TEST(PlayniteSync_DerivePlatform, NativeGameKeepsItsOnlyPlatform) {
  const auto guess = derive_platform(platform_game({{"PC (DOS)", "pc_dos"}}));
  EXPECT_EQ(guess.name, "PC (DOS)");
  EXPECT_EQ(guess.spec_id, "pc_dos");
}

TEST(PlayniteSync_DerivePlatform, EmulatedGameSkipsPcPlatformWhenEmulatorPlatformsDoNotMatch) {
  auto g = platform_game({{"PC (Windows)", "pc_windows"}, {"Sony PlayStation 2", "sony_playstation2"}}, true, {"nintendo_gamecube"});
  const auto guess = derive_platform(g);
  EXPECT_EQ(guess.name, "Sony PlayStation 2");
  EXPECT_EQ(guess.spec_id, "sony_playstation2");
}

TEST(PlayniteSync_DerivePlatform, EmulatedGameSkipsCustomPcPlatformName) {
  auto g = platform_game({{"PC", ""}, {"Example Console", ""}}, true);
  const auto guess = derive_platform(g);
  EXPECT_EQ(guess.name, "Example Console");
  EXPECT_TRUE(guess.spec_id.empty());
}

TEST(PlayniteSync_DerivePlatform, EmulatedGameWithoutPlatformsUsesEmulatorPlatform) {
  const auto guess = derive_platform(platform_game({}, true, {"nintendo_gamecube", "nintendo_wii"}));
  EXPECT_EQ(guess.name, "Nintendo GameCube");
  EXPECT_EQ(guess.spec_id, "nintendo_gamecube");
}

TEST(PlayniteSync_DerivePlatform, EmulatorPlatformNamesFromCustomProfiles) {
  const auto custom = derive_platform(platform_game({}, true, {"Example Handheld"}));
  EXPECT_EQ(custom.name, "Example Handheld");
  EXPECT_TRUE(custom.spec_id.empty());
  const auto unknown_spec = derive_platform(platform_game({}, true, {"example_system"}));
  EXPECT_EQ(unknown_spec.name, "Example System");
  EXPECT_EQ(unknown_spec.spec_id, "example_system");
}

TEST(PlayniteSync_DerivePlatform, KnownEmulatorsByBuiltInIdOrName) {
  struct expectation_t {
    std::string builtin_id;
    std::string name;
    std::string spec_id;
    std::string display;
  };

  const std::vector<expectation_t> cases {
    {"ryujinx", "", "nintendo_switch", "Nintendo Switch"},
    {"", "Eden", "nintendo_switch", "Nintendo Switch"},
    {"", "Citron", "nintendo_switch", "Nintendo Switch"},
    {"citra", "", "nintendo_3ds", "Nintendo 3DS"},
    {"", "Azahar", "nintendo_3ds", "Nintendo 3DS"},
    {"dolphin", "", "nintendo_gamecube", "Nintendo GameCube"},
    {"cemu", "", "nintendo_wiiu", "Nintendo Wii U"},
    {"pcsx2", "", "sony_playstation2", "Sony PlayStation 2"},
    {"rpcs3", "", "sony_playstation3", "Sony PlayStation 3"},
    {"duckstation", "", "sony_playstation", "Sony PlayStation"},
    {"ppsspp", "", "sony_psp", "Sony PlayStation Portable"},
    {"", "melonDS", "nintendo_ds", "Nintendo DS"},
    {"mgba", "", "nintendo_gameboyadvance", "Nintendo Game Boy Advance"},
  };
  for (const auto &c : cases) {
    const auto guess = derive_platform(platform_game({}, true, {}, c.builtin_id, c.name));
    EXPECT_EQ(guess.spec_id, c.spec_id) << c.builtin_id << c.name;
    EXPECT_EQ(guess.name, c.display) << c.builtin_id << c.name;
  }
}

TEST(PlayniteSync_DerivePlatform, UnknownEmulatorFallsBackToEmulated) {
  const auto guess = derive_platform(platform_game({}, true, {}, "", "Example Emulator"));
  EXPECT_EQ(guess.name, "Emulated");
  EXPECT_TRUE(guess.spec_id.empty());
}

TEST(PlayniteSync_DerivePlatform, PlatformWithOnlySpecIdGetsDisplayName) {
  const auto guess = derive_platform(platform_game({{"", "nintendo_wiiu"}}, true, {"nintendo_wiiu"}));
  EXPECT_EQ(guess.name, "Nintendo Wii U");
  EXPECT_EQ(guess.spec_id, "nintendo_wiiu");
}

TEST(PlayniteSync_DerivePlatform, DisplayNames) {
  EXPECT_EQ(platform_display_name("nintendo_3ds"), "Nintendo 3DS");
  EXPECT_EQ(platform_display_name("SONY_PLAYSTATION2"), "Sony PlayStation 2");
  EXPECT_EQ(platform_display_name("example_system"), "Example System");
  EXPECT_EQ(platform_display_name(""), "");
}
