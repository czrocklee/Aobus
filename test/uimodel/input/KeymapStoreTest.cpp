// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeymapStore.h>

#include "test/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
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

  TEST_CASE("loadKeymap returns defaults when the config group is absent")
  {
    auto const tempDir = ao::test::TempDir{};
    auto store = rt::ConfigStore{tempDir.path() / "config.yaml"};

    auto const keymap = loadKeymap(store, sampleDefaults());
    CHECK(keymap.bindings() == sampleDefaults());
  }

  TEST_CASE("saveKeymap then loadKeymap round-trips a customization")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    auto overrides = KeymapOverrides{};
    auto expectedChords = std::vector<KeyChord>{};

    SECTION("nonempty customization")
    {
      overrides["playback.next"] = {"Ctrl+N", "Media:Next"};
      expectedChords = {chord("Ctrl+N"), chord("Media:Next")};
    }

    SECTION("explicit empty override remains unbound after reload")
    {
      overrides["playback.next"] = {};
    }

    {
      auto store = rt::ConfigStore{configPath};
      auto keymap = KeymapModel{sampleDefaults()};
      keymap.applyOverrides(overrides);
      REQUIRE(keymap.chordsFor("playback.next") == expectedChords);
      REQUIRE(saveKeymap(store, keymap));
    }

    auto store = rt::ConfigStore{configPath};
    auto const reloaded = loadKeymap(store, sampleDefaults());

    CHECK(reloaded.chordsFor("playback.next") == expectedChords);
    // Unmodified action still resolves to its default.
    CHECK(reloaded.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
  }

  TEST_CASE("saveKeymap only persists deltas from defaults")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";

    {
      auto store = rt::ConfigStore{configPath};
      REQUIRE(saveKeymap(store, KeymapModel{sampleDefaults()}));
    }

    // Reload with *different* defaults: since nothing was persisted, the new defaults win.
    auto newDefaults = KeymapBindings{{"playback.playPause", {chord("Ctrl+Shift+P")}}};
    auto store = rt::ConfigStore{configPath};
    auto const reloaded = loadKeymap(store, newDefaults);
    CHECK(reloaded.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+Shift+P")});
  }

  TEST_CASE("saveKeymap rejects an empty action id")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = rt::ConfigStore{configPath};
    auto keymap = KeymapModel{sampleDefaults()};
    REQUIRE(keymap.tryBind("", chord("Ctrl+E")));

    auto const savedRes = saveKeymap(store, keymap);
    REQUIRE_FALSE(savedRes);
    CHECK(savedRes.error().code == Error::Code::InvalidState);

    CHECK_FALSE(std::filesystem::exists(configPath));
  }

  TEST_CASE("loadKeymap rejects malformed YAML as one candidate")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto output = std::ofstream{configPath};
    REQUIRE(output.is_open());
    output << "shortcuts:\n"
              "  version: 1\n"
              "  bindings:\n"
              "    playback.playPause:\n"
              "      - Ctrl+Shift+P\n"
              "    playback.next:\n"
              "      - nested: invalid\n";
    output.close();
    REQUIRE(output.good());

    auto store = rt::ConfigStore{configPath};
    auto const keymap = loadKeymap(store, sampleDefaults());

    CHECK(keymap.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
    CHECK(keymap.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
  }

  TEST_CASE("loadKeymap preserves dynamic action ids and skips invalid chord strings semantically")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto output = std::ofstream{configPath};
    REQUIRE(output.is_open());
    output << "shortcuts:\n"
              "  version: 1\n"
              "  bindings:\n"
              "    plugin.futureAction:\n"
              "      - Ctrl+F\n"
              "      - Ctrl+\n";
    output.close();
    REQUIRE(output.good());

    auto store = rt::ConfigStore{configPath};
    auto const keymap = loadKeymap(store, sampleDefaults());

    CHECK(keymap.chordsFor("plugin.futureAction") == std::vector<KeyChord>{chord("Ctrl+F")});
    CHECK(keymap.chordsFor("playback.playPause") == std::vector<KeyChord>{chord("Ctrl+P")});
  }

  TEST_CASE("saveKeymap writes the bindings under a version")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto store = rt::ConfigStore{configPath};
    auto keymap = KeymapModel{sampleDefaults()};
    keymap.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
    REQUIRE(saveKeymap(store, keymap));

    CHECK(ao::test::readFile(configPath) == "shortcuts:\n"
                                            "  version: 1\n"
                                            "  bindings:\n"
                                            "    playback.next:\n"
                                            "      - \"Ctrl+N\"\n");
  }

  TEST_CASE("a shortcuts group of a newer version is kept and never overwritten")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    auto const document = std::string{"shortcuts:\n"
                                      "  version: 2\n"
                                      "  bindings:\n"
                                      "    playback.next:\n"
                                      "      - Ctrl+N\n"
                                      "  layers: {}\n"};
    {
      auto output = std::ofstream{configPath, std::ios::binary};
      output << document;
    }

    auto store = rt::ConfigStore{configPath};
    auto keymap = loadKeymap(store, sampleDefaults());
    CHECK(keymap.bindings() == sampleDefaults());
    CHECK(store.hasNewerGroup(kKeymapConfigGroup));

    REQUIRE(keymap.tryBind("playback.next", chord("Ctrl+M")));
    auto const savedRes = saveKeymap(store, keymap);
    REQUIRE_FALSE(savedRes);
    CHECK(savedRes.error().code == Error::Code::NotSupported);
    CHECK(ao::test::readFile(configPath) == document);
  }

  TEST_CASE("an unversioned shortcuts map is malformed rather than newer")
  {
    // The flat shape that development builds wrote before version 1.
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "config.yaml";
    {
      auto output = std::ofstream{configPath, std::ios::binary};
      output << "shortcuts:\n"
                "  playback.next:\n"
                "    - Ctrl+M\n";
    }

    auto store = rt::ConfigStore{configPath};
    auto keymap = loadKeymap(store, sampleDefaults());
    CHECK(keymap.bindings() == sampleDefaults());
    CHECK_FALSE(store.hasNewerGroup(kKeymapConfigGroup));

    REQUIRE(saveKeymap(store, keymap));
    CHECK(ao::test::readFile(configPath) == "shortcuts:\n"
                                            "  version: 1\n"
                                            "  bindings: {}\n");
  }
} // namespace ao::uimodel::test
