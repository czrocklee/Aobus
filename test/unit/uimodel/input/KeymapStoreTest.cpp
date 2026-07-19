// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    KeyChord chord(std::string const& text)
    {
      auto const optChord = KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }

    KeymapBindings sampleDefaults()
    {
      return KeymapBindings{
        {"playback.playPause", {chord("Ctrl+P")}},
        {"playback.next", {chord("Ctrl+Right")}},
      };
    }
  } // namespace

  TEST_CASE("loadKeymap returns defaults when the config group is absent", "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto store = rt::ConfigStore{tempDir.path() / "config.yaml"};

    auto const keymap = loadKeymap(store, sampleDefaults());
    CHECK(keymap.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
  }

  TEST_CASE("saveKeymap then loadKeymap round-trips a customization", "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    {
      auto store = rt::ConfigStore{configPath};
      auto keymap = KeymapModel{sampleDefaults()};
      keymap.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N", "Media:Next"}}});
      saveKeymap(store, keymap);
    }

    auto store = rt::ConfigStore{configPath};
    auto const reloaded = loadKeymap(store, sampleDefaults());

    CHECK(reloaded.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+N"), chord("Media:Next")});
    // Unmodified action still resolves to its default.
    CHECK(reloaded.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
  }

  TEST_CASE("saveKeymap only persists deltas from defaults", "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    {
      auto store = rt::ConfigStore{configPath};
      saveKeymap(store, KeymapModel{sampleDefaults()}); // no customization
    }

    // Reload with *different* defaults: since nothing was persisted, the new defaults win.
    auto newDefaults = KeymapBindings{{"playback.playPause", {chord("Ctrl+Shift+P")}}};
    auto store = rt::ConfigStore{configPath};
    auto const reloaded = loadKeymap(store, newDefaults);
    CHECK(reloaded.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+Shift+P")});
  }

  TEST_CASE("saveKeymap rejects an empty action id", "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = rt::ConfigStore{configPath};
    auto keymap = KeymapModel{sampleDefaults()};
    REQUIRE(keymap.bind("", chord("Ctrl+E")));

    saveKeymap(store, keymap);

    CHECK_FALSE(std::filesystem::exists(configPath));
  }

  TEST_CASE("loadKeymap rejects malformed YAML as one candidate", "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto output = std::ofstream{configPath};
    output << "shortcuts:\n"
              "  playback.playPause:\n"
              "    - Ctrl+Shift+P\n"
              "  playback.next:\n"
              "    - nested: invalid\n";
    output.close();

    auto store = rt::ConfigStore{configPath};
    auto const keymap = loadKeymap(store, sampleDefaults());

    CHECK(keymap.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
    CHECK(keymap.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
  }

  TEST_CASE("loadKeymap preserves dynamic action ids and skips invalid chord strings semantically",
            "[uimodel][unit][input][keymapstore]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto output = std::ofstream{configPath};
    output << "shortcuts:\n"
              "  plugin.futureAction:\n"
              "    - Ctrl+F\n"
              "    - Ctrl+\n";
    output.close();

    auto store = rt::ConfigStore{configPath};
    auto const keymap = loadKeymap(store, sampleDefaults());

    CHECK(keymap.chordsFor("plugin.futureAction") == std::vector<KeyChord>{chord("Ctrl+F")});
    CHECK(keymap.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
  }
} // namespace ao::uimodel::test
