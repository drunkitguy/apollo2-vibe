#include "src/platform/windows/playnite_protocol.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using platf::playnite::Message;
using platf::playnite::MessageType;

static std::vector<uint8_t> bytes(const std::string &s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

TEST(PlayniteProtocol_Parse, EmptyBytes_ReturnsUnknown) {
  auto m = platf::playnite::parse({});
  EXPECT_EQ(m.type, MessageType::Unknown);
}

TEST(PlayniteProtocol_Parse, InvalidJson_ReturnsUnknownNoThrow) {
  auto m = platf::playnite::parse(bytes("{not-json"));
  EXPECT_EQ(m.type, MessageType::Unknown);
}

TEST(PlayniteProtocol_Parse, UnknownType_ReturnsUnknown) {
  auto m = platf::playnite::parse(bytes("{\"type\":\"wat\"}"));
  EXPECT_EQ(m.type, MessageType::Unknown);
}

TEST(PlayniteProtocol_Parse, Categories_ParsesAndSkipsEmpty) {
  std::string j = R"({
    "type":"categories",
    "payload":[{"id":"1","name":"A"},{},{"id":"","name":""}]
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Categories);
  ASSERT_EQ(m.categories.size(), 1u);
  EXPECT_EQ(m.categories[0].id, "1");
  EXPECT_EQ(m.categories[0].name, "A");
}

TEST(PlayniteProtocol_Parse, Games_PopulatesBasicFields) {
  std::string j = R"({
    "type":"games",
    "payload":[{
      "id":"g1",
      "name":"Game",
      "exe":"C:/g.exe",
      "args":"--x",
      "workingDir":"C:/w",
      "categories":["C1","C2"],
      "playtimeMinutes": 42,
      "lastPlayed":"2024-01-02T03:04:05Z",
      "boxArtPath":"C:/box.png",
      "iconPath":"C:/icon.png",
      "description":"desc",
      "tags":["t1","t2"],
      "installed": true
    }]
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 1u);
  const auto &g = m.games[0];
  EXPECT_EQ(g.id, "g1");
  EXPECT_EQ(g.name, "Game");
  EXPECT_EQ(g.exe, "C:/g.exe");
  EXPECT_EQ(g.args, "--x");
  EXPECT_EQ(g.working_dir, "C:/w");
  ASSERT_EQ(g.categories.size(), 2u);
  EXPECT_EQ(g.categories[0], "C1");
  EXPECT_EQ(g.playtime_minutes, 42u);
  EXPECT_EQ(g.last_played, "2024-01-02T03:04:05Z");
  EXPECT_EQ(g.box_art_path, "C:/box.png");
  EXPECT_EQ(g.icon_path, "C:/icon.png");
  ASSERT_EQ(g.tags.size(), 2u);
  EXPECT_TRUE(g.installed);
}

TEST(PlayniteProtocol_Parse, Games_PlaytimeMinutes_StringParses) {
  std::string j = R"({
    "type":"games","payload":[{"id":"g","playtimeMinutes":"123"}]})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 1u);
  EXPECT_EQ(m.games[0].playtime_minutes, 123u);
}

TEST(PlayniteProtocol_Parse, Games_PlaytimeMinutes_InvalidStringDefaultsZero) {
  std::string j = R"({
    "type":"games","payload":[{"id":"g","playtimeMinutes":"abc"}]})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 1u);
  EXPECT_EQ(m.games[0].playtime_minutes, 0u);
}

TEST(PlayniteProtocol_Parse, Games_InstalledField_PrecedenceAndDefault) {
  std::string j = R"({
    "type":"games",
    "payload":[
      {"id":"a","installed":false},
      {"id":"b","isInstalled":true},
      {"id":"c","installed":false,"isInstalled":false},
      {"id":"d"}
    ]})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 4u);
  EXPECT_FALSE(m.games[0].installed);
  EXPECT_TRUE(m.games[1].installed);
  EXPECT_FALSE(m.games[2].installed);
  EXPECT_TRUE(m.games[3].installed);  // default true when neither present
}

TEST(PlayniteProtocol_Parse, Games_SkipsMissingId) {
  std::string j = R"({"type":"games","payload":[{"name":"no-id"}]})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 0u);
}

TEST(PlayniteProtocol_Parse, Status_ParsesFields) {
  std::string j = R"({
    "type":"status",
    "status": {"name":"gameStarted","id":"guid-1","installDir":"C:/Games/X","exe":"C:/Games/X/game.exe"}
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Status);
  EXPECT_EQ(m.status_name, "gameStarted");
  EXPECT_EQ(m.status_game_id, "guid-1");
  EXPECT_EQ(m.status_install_dir, "C:/Games/X");
  EXPECT_EQ(m.status_exe, "C:/Games/X/game.exe");
}

TEST(PlayniteProtocol_Parse, Status_ParsesFieldsWithUtf8Bom) {
  std::string j = "\xEF\xBB\xBF{";
  j += R"("type":"status","status":{"name":"gameStarted","id":"bom-id"}})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Status);
  EXPECT_EQ(m.status_name, "gameStarted");
  EXPECT_EQ(m.status_game_id, "bom-id");
}

TEST(PlayniteProtocol_Platforms, NewConnectorPayloadIsParsed) {
  std::string j = R"json({
    "type":"games",
    "payload":[{
      "id":"g1",
      "name":"Example Game",
      "installed": true,
      "platforms":[{"name":"PC (Windows)","specId":"pc_windows"},{"name":"Nintendo Switch","specId":"nintendo_switch"}],
      "source":"Example Source",
      "emulated": true,
      "emulatorName":"Example Emulator",
      "emulatorBuiltInId":"ryujinx",
      "emulatorPlatforms":["nintendo_switch"]
    }]
  })json";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 1u);
  const auto &g = m.games[0];
  EXPECT_TRUE(g.has_platform_info);
  ASSERT_EQ(g.platforms.size(), 2u);
  EXPECT_EQ(g.platforms[0].name, "PC (Windows)");
  EXPECT_EQ(g.platforms[0].spec_id, "pc_windows");
  EXPECT_EQ(g.platforms[1].name, "Nintendo Switch");
  EXPECT_EQ(g.platforms[1].spec_id, "nintendo_switch");
  EXPECT_EQ(g.source_name, "Example Source");
  EXPECT_TRUE(g.emulated);
  EXPECT_EQ(g.emulator_name, "Example Emulator");
  EXPECT_EQ(g.emulator_builtin_id, "ryujinx");
  ASSERT_EQ(g.emulator_platforms.size(), 1u);
  EXPECT_EQ(g.emulator_platforms[0], "nintendo_switch");
}

TEST(PlayniteProtocol_Platforms, SingleElementArraysSerializedAsScalarsAreAccepted) {
  // Windows PowerShell may turn a one-element array into the element itself.
  std::string j = R"({
    "type":"games",
    "payload":[{
      "id":"g1",
      "name":"Example Game",
      "platforms":{"name":"Nintendo 3DS","specId":"nintendo_3ds"},
      "emulated": true,
      "emulatorPlatforms":"nintendo_3ds"
    },{
      "id":"g2",
      "name":"Example Game 2",
      "platforms":["Custom System", 5, null, {"name":"","specId":""}]
    }]
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.games.size(), 2u);
  ASSERT_EQ(m.games[0].platforms.size(), 1u);
  EXPECT_EQ(m.games[0].platforms[0].name, "Nintendo 3DS");
  EXPECT_EQ(m.games[0].platforms[0].spec_id, "nintendo_3ds");
  ASSERT_EQ(m.games[0].emulator_platforms.size(), 1u);
  EXPECT_EQ(m.games[0].emulator_platforms[0], "nintendo_3ds");
  ASSERT_EQ(m.games[1].platforms.size(), 1u);
  EXPECT_EQ(m.games[1].platforms[0].name, "Custom System");
  EXPECT_TRUE(m.games[1].platforms[0].spec_id.empty());
}

TEST(PlayniteProtocol_Platforms, OldConnectorPayloadHasNoPlatformInfo) {
  std::string j = R"({
    "type":"games",
    "payload":[{"id":"g1","name":"Example Game","pluginName":"Example Library","installed":true}]
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.games.size(), 1u);
  const auto &g = m.games[0];
  EXPECT_FALSE(g.has_platform_info);
  EXPECT_TRUE(g.platforms.empty());
  EXPECT_TRUE(g.source_name.empty());
  EXPECT_FALSE(g.emulated);
  EXPECT_TRUE(g.emulator_name.empty());
  EXPECT_TRUE(g.emulator_builtin_id.empty());
  EXPECT_TRUE(g.emulator_platforms.empty());
}

TEST(PlayniteProtocol_Platforms, EmptyPlatformListStillMarksNewConnector) {
  std::string j = R"({"type":"games","payload":[{"id":"g1","name":"Example Game","platforms":[]}]})";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.games.size(), 1u);
  EXPECT_TRUE(m.games[0].has_platform_info);
  EXPECT_TRUE(m.games[0].platforms.empty());
}

TEST(PlayniteProtocol_Platforms, MalformedPlatformFieldsNeverDropTheGame) {
  std::string j = R"({
    "type":"games",
    "payload":[{
      "id":"g1",
      "name":"Example Game",
      "platforms":42,
      "source":null,
      "emulated":"yes please",
      "emulatorName":{"nested":true},
      "emulatorBuiltInId":7,
      "emulatorPlatforms":{"not":"a list"}
    },{
      "id":"g2",
      "name":"Example Game 2",
      "platforms":null,
      "emulated":1
    }]
  })";
  auto m = platf::playnite::parse(bytes(j));
  ASSERT_EQ(m.type, MessageType::Games);
  ASSERT_EQ(m.games.size(), 2u);
  EXPECT_EQ(m.games[0].id, "g1");
  EXPECT_TRUE(m.games[0].has_platform_info);
  EXPECT_TRUE(m.games[0].platforms.empty());
  EXPECT_TRUE(m.games[0].source_name.empty());
  EXPECT_FALSE(m.games[0].emulated);
  EXPECT_TRUE(m.games[0].emulator_name.empty());
  EXPECT_TRUE(m.games[0].emulator_builtin_id.empty());
  EXPECT_TRUE(m.games[0].emulator_platforms.empty());
  EXPECT_EQ(m.games[1].id, "g2");
  EXPECT_TRUE(m.games[1].has_platform_info);
  EXPECT_TRUE(m.games[1].emulated);
}
