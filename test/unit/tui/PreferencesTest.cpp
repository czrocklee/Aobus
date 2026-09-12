// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/Preferences.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace ao::tui::test
{
  TEST_CASE("Preferences - rejects malformed and unsupported saved values without rewriting them",
            "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";

    for (auto const* document : {"preferences: [",
                                 "preferences: {version: 2}",
                                 "preferences: {version: 1, wheelStep: 0}",
                                 "preferences: {version: 1, wheelStep: 11}",
                                 "preferences: {version: 1, seekSeconds: 61}",
                                 "preferences: {version: 1, volumePercent: 0}",
                                 "preferences: {version: 1, coverArtMode: invalid}",
                                 "preferences: {version: 1, panelSeparator: triple}",
                                 "preferences: {version: 1, terminalTitleFormat: $unknown}",
                                 "preferences: {version: 1, language: unsupported}",
                                 "preferences: {version: 1, unknown: true}"})
    {
      INFO(document);
      std::ofstream{path} << document;
      auto store = rt::ConfigStore{path};
      CHECK_FALSE(loadPreferences(store));
      CHECK(ao::test::readFile(path) == document);
    }
  }

  TEST_CASE("Preferences - missing groups use defaults and valid numeric boundaries round trip", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};
    auto loadedRes = loadPreferences(store);
    REQUIRE(loadedRes);
    CHECK(*loadedRes == Preferences{});

    for (auto const& preferences : {Preferences{.wheelStep = 1, .seekSeconds = 1, .volumePercent = 1},
                                    Preferences{.wheelStep = 10, .seekSeconds = 60, .volumePercent = 10}})
    {
      REQUIRE(savePreferences(store, preferences));
      auto reopened = rt::ConfigStore{path};
      auto rereadRes = loadPreferences(reopened);
      REQUIRE(rereadRes);
      CHECK(*rereadRes == preferences);
    }
  }

  TEST_CASE("Preferences - existing version one documents default to a single panel separator", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    std::ofstream{path} << "preferences: {version: 1, coverArtMode: off}";
    auto store = rt::ConfigStore{path};
    auto loadedRes = loadPreferences(store);
    REQUIRE(loadedRes);
    CHECK(loadedRes->panelSeparator == "single");
    CHECK(loadedRes->coverArtMode == "off");
    CHECK_FALSE(loadedRes->revealIndicatorsOnHover);
  }

  TEST_CASE("Preferences - title formats preserve defaults and round trip disabled and custom values",
            "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    std::ofstream{path} << "preferences: {version: 1}";
    auto store = rt::ConfigStore{path};
    REQUIRE(loadPreferences(store));
    CHECK(loadPreferences(store)->terminalTitleFormat == Preferences{}.terminalTitleFormat);
    CHECK(loadPreferences(store)->terminalTitleSoul);

    for (auto const* format : {"", R"($title " / " %catalog)"})
    {
      auto preferences = Preferences{};
      preferences.terminalTitleFormat = format;
      preferences.terminalTitleSoul = false;
      REQUIRE(savePreferences(store, preferences));
      auto reopened = rt::ConfigStore{path};
      auto loadedRes = loadPreferences(reopened);
      REQUIRE(loadedRes);
      CHECK(loadedRes->terminalTitleFormat == format);
      CHECK_FALSE(loadedRes->terminalTitleSoul);
    }
  }

  TEST_CASE("Preferences - round trips language and preserves sibling keymap writes", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    std::ofstream{path} << "foreign:\n  retained: yes\n";
    auto store = rt::ConfigStore{path};
    auto preferences = Preferences{.language = "zh-Hant",
                                   .coverArtMode = "blocks",
                                   .panelSeparator = "double",
                                   .revealIndicatorsOnHover = true,
                                   .dimBackdrop = false,
                                   .reducedMotion = true,
                                   .mouseEnabled = false,
                                   .qualityHover = false,
                                   .wheelStep = 7,
                                   .seekSeconds = 30,
                                   .volumePercent = 1};
    REQUIRE(savePreferences(store, preferences));
    auto keymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
    REQUIRE(keymap.tryBind("foreign.action", *uimodel::KeyChord::parse("F12")));
    REQUIRE(uimodel::saveKeymap(store, keymap));
    auto reopened = rt::ConfigStore{path};
    auto loadedRes = loadPreferences(reopened);
    REQUIRE(loadedRes);
    CHECK(*loadedRes == preferences);
    CHECK(ao::test::readFile(path).contains("retained: yes"));
    preferences.language.clear();
    REQUIRE(savePreferences(reopened, preferences));
    CHECK(uimodel::loadKeymap(reopened, uimodel::defaultKeymap()).chordsFor("foreign.action") ==
          keymap.chordsFor("foreign.action"));
    auto systemRes = loadPreferences(reopened);
    REQUIRE(systemRes);
    CHECK(systemRes->language.empty());
  }

  TEST_CASE("Preferences - accepts every selectable catalog locale and excludes pseudo", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};

    for (auto const& locale : i18n::availableCatalogLocales())
    {
      INFO(locale.tag);
      auto preferences = Preferences{.language = std::string{locale.tag}};
      REQUIRE(savePreferences(store, preferences));
      auto reopened = rt::ConfigStore{path};
      auto loadedRes = loadPreferences(reopened);
      REQUIRE(loadedRes);
      CHECK(loadedRes->language == locale.tag);
    }

    auto const original = ao::test::readFile(path);
    CHECK_FALSE(savePreferences(store, Preferences{.language = "qps-ploc"}));
    CHECK(ao::test::readFile(path) == original);
  }

  TEST_CASE("Preferences - rejects invalid values without replacing the saved document", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};
    REQUIRE(savePreferences(store, Preferences{}));
    auto const original = ao::test::readFile(path);
    auto candidate = Preferences{.language = "en_US!"};
    CHECK_FALSE(savePreferences(store, candidate));
    candidate.language.clear();
    candidate.panelSeparator = "triple";
    CHECK_FALSE(savePreferences(store, candidate));
    CHECK(ao::test::readFile(path) == original);
    candidate.panelSeparator = "single";
    candidate.wheelStep = 0;
    CHECK_FALSE(savePreferences(store, candidate));
    CHECK(ao::test::readFile(path) == original);
    auto noLocation = rt::ConfigStore{rt::ConfigStore::NoLocation{}};
    CHECK_FALSE(savePreferences(noLocation, Preferences{}));
  }
} // namespace ao::tui::test
