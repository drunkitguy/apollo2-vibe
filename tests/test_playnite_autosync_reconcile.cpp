#include "src/platform/windows/playnite_sync_policy.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unordered_set>

using namespace platf::playnite;
using namespace platf::playnite::sync;
using namespace platf::playnite::sync::policy;

static Game G(std::string id, std::string last, bool installed = true, std::vector<std::string> cats = {}, std::string plugin = {}) {
  Game g;
  g.id = id;
  g.name = id;
  g.last_played = last;
  g.installed = installed;
  g.categories = cats;
  g.plugin_id = plugin;
  return g;
}

TEST(PlayniteAutosync_Reconcile, AddsSelectedGamesToEmptyApps) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true), G("B", "2024-01-01T00:00:00Z", true, {"RPG"})};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all,
                     /*recentN*/ 1,
                     /*recentAgeDays*/ 0,
                     /*delete_after_days*/ 0,
                     /*require_repl*/ true,
                     /*sync_all_installed*/ false,
                     /*categories*/ std::vector<std::string> {"RPG"},
                     /*include_plugins*/ std::vector<std::string> {},
                     /*exclude_categories*/ std::vector<std::string> {},
                     /*exclude_ids*/ std::vector<std::string> {},
                     /*exclude_plugins*/ std::vector<std::string> {},
                     /*remove_uninstalled*/ true,
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 2u);  // A from recent, B from category
  std::unordered_set<std::string> ids;
  for (const auto &app : root["apps"]) {
    ids.insert(app["playnite-id"].get<std::string>());
  }
  EXPECT_TRUE(ids.contains("A"));
  EXPECT_TRUE(ids.contains("B"));
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludeIds) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, false, {}, {}, {}, {"a"}, {}, true, changed, matched);
  EXPECT_FALSE(changed);
  EXPECT_EQ(root["apps"].size(), 0u);
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludeCategories) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {
    G("A", "2025-01-01T00:00:00Z", true, {"Steam"}),
    G("B", "2024-12-01T00:00:00Z", true, {"Indie"})
  };
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all,
                     /*recentN*/ 2,
                     /*recentAgeDays*/ 0,
                     /*delete_after_days*/ 0,
                     /*require_repl*/ true,
                     /*sync_all_installed*/ false,
                     /*categories*/ std::vector<std::string> {},
                     /*include_plugins*/ std::vector<std::string> {},
                     /*exclude_categories*/ std::vector<std::string> {"Steam"},
                     /*exclude_ids*/ {},
                     /*exclude_plugins*/ std::vector<std::string> {},
                     /*remove_uninstalled*/ true,
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "B");
}

TEST(PlayniteAutosync_Reconcile, HonorsExcludePlugins) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {
    G("A", "2025-01-01T00:00:00Z", true, {}, "CB91DFC9-B977-43BF-8E70-55F46E410FAB"),
    G("B", "2025-01-02T00:00:00Z", true, {}, "83DD83A4-0CF7-49FB-9138-8547F6B60C18")
  };
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all,
                     /*recentN*/ 2,
                     /*recentAgeDays*/ 0,
                     /*delete_after_days*/ 0,
                     /*require_repl*/ true,
                     /*sync_all_installed*/ false,
                     /*categories*/ std::vector<std::string> {},
                     /*include_plugins*/ std::vector<std::string> {},
                     /*exclude_categories*/ std::vector<std::string> {},
                     /*exclude_ids*/ {},
                     /*exclude_plugins*/ std::vector<std::string> {"cb91dfc9-b977-43bf-8e70-55f46e410fab"},
                     /*remove_uninstalled*/ true,
                     changed,
                     matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "B");
}

TEST(PlayniteAutosync_Reconcile, UpdatesExistingAndSetsManagedFields) {
  // Existing app matching by id gets annotated and not duplicated
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json a;
  a["playnite-id"] = "A";
  a["uuid"] = "OLD";
  root["apps"].push_back(a);
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, false, {}, {}, {}, {}, {}, true, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][0]["uuid"], "A");
  EXPECT_EQ(root["apps"][0]["playnite-managed"], "auto");
}

TEST(PlayniteAutosync_Reconcile, MatchesExistingEntriesCaseInsensitively) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json a;
  a["playnite-id"] = "ABC-DEF";
  a["uuid"] = "OLD";
  root["apps"].push_back(a);

  std::vector<Game> all {G("abc-def", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, false, {}, {}, {}, {}, {}, true, changed, matched);

  EXPECT_TRUE(changed);
  EXPECT_EQ(matched, 1u);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "abc-def");
  EXPECT_EQ(root["apps"][0]["uuid"], "ABC-DEF");
}

TEST(PlayniteAutosync_Reconcile, RemovesDuplicateAutoEntriesByPlayniteId) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();

  nlohmann::json a1;
  a1["playnite-id"] = "ABC-DEF";
  a1["playnite-managed"] = "auto";
  root["apps"].push_back(a1);

  nlohmann::json a2;
  a2["playnite-id"] = "abc-def";
  a2["playnite-managed"] = "auto";
  root["apps"].push_back(a2);

  std::vector<Game> all {G("abc-def", "2025-01-01T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 1, 0, 0, true, false, {}, {}, {}, {}, {}, true, changed, matched);

  EXPECT_TRUE(changed);
  EXPECT_EQ(matched, 1u);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "abc-def");
}

TEST(PlayniteAutosync_Reconcile, SyncPluginsIncludesGames) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {
    G("A", "2025-01-01T00:00:00Z", true, {}, "PLUGIN-ONE"),
    G("B", "2025-01-02T00:00:00Z", true, {}, "PLUGIN-TWO"),
    G("C", "2025-01-03T00:00:00Z", true, {}, "")
  };
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 0, 0, 0, true, false, {}, std::vector<std::string> {"plugin-one"}, {}, {}, {}, true, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
  EXPECT_EQ(root["apps"][0]["playnite-source"], "plugin");
}

TEST(PlayniteAutosync_Reconcile, SyncAllInstalledIncludesAllGames) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true), G("B", "2025-01-02T00:00:00Z", true)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 0, 0, 0, true, true, {}, {}, {}, {}, {}, true, changed, matched);
  EXPECT_TRUE(changed);
  ASSERT_EQ(root["apps"].size(), 2u);
  std::unordered_set<std::string> ids;
  for (auto &app : root["apps"]) {
    ids.insert(app["playnite-id"].get<std::string>());
    EXPECT_EQ(app["playnite-source"], "installed");
  }
  EXPECT_TRUE(ids.contains("A"));
  EXPECT_TRUE(ids.contains("B"));
}

TEST(PlayniteAutosync_Reconcile, AutoRemoveUninstalledHonorsFlag) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  nlohmann::json entry;
  entry["playnite-id"] = "A";
  entry["playnite-managed"] = "auto";
  root["apps"].push_back(entry);
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", false)};
  bool changed = false;
  std::size_t matched = 0;
  autosync_reconcile(root, all, 0, 0, 0, true, false, {}, {}, {}, {}, {}, true, changed, matched);
  EXPECT_TRUE(changed);
  EXPECT_EQ(root["apps"].size(), 0u);

  // Repeat with removal disabled; entry should remain.
  root["apps"] = nlohmann::json::array();
  root["apps"].push_back(entry);
  changed = false;
  matched = 0;
  autosync_reconcile(root, all, 0, 0, 0, true, false, {}, {}, {}, {}, {}, false, changed, matched);
  EXPECT_FALSE(changed);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0]["playnite-id"], "A");
}

namespace {
  Game platform_game(std::string id, std::vector<PlatformRef> platforms, bool emulated, std::vector<std::string> emulator_platforms = {}) {
    Game g = G(std::move(id), "2025-01-01T00:00:00Z", true);
    g.name = "Example Game " + g.id;
    g.has_platform_info = true;
    g.platforms = std::move(platforms);
    g.emulated = emulated;
    g.emulator_platforms = std::move(emulator_platforms);
    return g;
  }

  void reconcile(nlohmann::json &root, const std::vector<Game> &all, int recent_count = 0) {
    bool changed = false;
    std::size_t matched = 0;
    autosync_reconcile(root, all, recent_count, 0, 0, false, false, {}, {}, {}, {}, {}, false, changed, matched);
  }
}  // namespace

TEST(PlayniteAutosync_Platform, BackfillsExistingLinkedAppsAndKeepsManualPlatform) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  root["apps"].push_back({{"name", "Example Game A"}, {"playnite-id", "A"}, {"platform", "My Group"}, {"platform-id", "my_group"}});
  root["apps"].push_back({{"name", "Example Game B"}, {"playnite-id", "B"}});
  std::vector<Game> all {
    platform_game("A", {{"PC (Windows)", "pc_windows"}, {"Nintendo Switch", "nintendo_switch"}}, true, {"nintendo_switch"}),
    platform_game("B", {{"PC (Windows)", "pc_windows"}}, false),
  };
  reconcile(root, all);
  ASSERT_EQ(root["apps"].size(), 2u);
  const auto &a = root["apps"][0];
  EXPECT_EQ(a.value("playnite-platform", ""), "Nintendo Switch");
  EXPECT_EQ(a.value("playnite-platform-id", ""), "nintendo_switch");
  EXPECT_EQ(a.value("platform", ""), "My Group");
  EXPECT_EQ(a.value("platform-id", ""), "my_group");
  const auto &b = root["apps"][1];
  EXPECT_EQ(b.value("playnite-platform", ""), "PC (Windows)");
  EXPECT_EQ(b.value("playnite-platform-id", ""), "pc_windows");
  EXPECT_FALSE(b.contains("platform"));
}

TEST(PlayniteAutosync_Platform, NewAutoEntriesCarryPlatform) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  reconcile(root, {platform_game("A", {{"Nintendo 3DS", "nintendo_3ds"}}, true, {"nintendo_3ds"})}, 1);
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0].value("playnite-platform", ""), "Nintendo 3DS");
  EXPECT_EQ(root["apps"][0].value("playnite-platform-id", ""), "nintendo_3ds");
  EXPECT_FALSE(root["apps"][0].contains("platform"));
}

TEST(PlayniteAutosync_Platform, OldConnectorWritesNothingAndKeepsEarlierValues) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  root["apps"].push_back({{"name", "Example Game A"}, {"playnite-id", "A"}, {"playnite-platform", "Nintendo GameCube"}, {"playnite-platform-id", "nintendo_gamecube"}});
  root["apps"].push_back({{"name", "Example Game B"}, {"playnite-id", "B"}});
  std::vector<Game> all {G("A", "2025-01-01T00:00:00Z", true), G("B", "2025-01-01T00:00:00Z", true)};
  reconcile(root, all);
  ASSERT_EQ(root["apps"].size(), 2u);
  EXPECT_EQ(root["apps"][0].value("playnite-platform", ""), "Nintendo GameCube");
  EXPECT_EQ(root["apps"][0].value("playnite-platform-id", ""), "nintendo_gamecube");
  EXPECT_FALSE(root["apps"][1].contains("playnite-platform"));
  EXPECT_FALSE(root["apps"][1].contains("playnite-platform-id"));
}

TEST(PlayniteAutosync_Platform, CustomPlatformWithoutSpecIdClearsStaleId) {
  nlohmann::json root;
  root["apps"] = nlohmann::json::array();
  root["apps"].push_back({{"name", "Example Game A"}, {"playnite-id", "A"}, {"playnite-platform", "Nintendo Switch"}, {"playnite-platform-id", "nintendo_switch"}});
  reconcile(root, {platform_game("A", {{"Example Console", ""}}, true)});
  ASSERT_EQ(root["apps"].size(), 1u);
  EXPECT_EQ(root["apps"][0].value("playnite-platform", ""), "Example Console");
  EXPECT_FALSE(root["apps"][0].contains("playnite-platform-id"));
}
