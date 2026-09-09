// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TuiPreferences.h"

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
  TEST_CASE("TuiPreferences - rejects malformed and unsupported saved values without rewriting them",
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
                                 "preferences: {version: 1, language: unsupported}",
                                 "preferences: {version: 1, unknown: true}"})
    {
      INFO(document);
      std::ofstream{path} << document;
      auto store = rt::ConfigStore{path};
      CHECK_FALSE(loadTuiPreferences(store));
      CHECK(ao::test::readFile(path) == document);
    }
  }

  TEST_CASE("TuiPreferences - missing groups use defaults and valid numeric boundaries round trip",
            "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};
    auto loadedRes = loadTuiPreferences(store);
    REQUIRE(loadedRes);
    CHECK(*loadedRes == TuiPreferences{});

    for (auto const& preferences : {TuiPreferences{.wheelStep = 1, .seekSeconds = 1, .volumePercent = 1},
                                    TuiPreferences{.wheelStep = 10, .seekSeconds = 60, .volumePercent = 10}})
    {
      REQUIRE(saveTuiPreferences(store, preferences));
      auto reopened = rt::ConfigStore{path};
      auto rereadRes = loadTuiPreferences(reopened);
      REQUIRE(rereadRes);
      CHECK(*rereadRes == preferences);
    }
  }

  TEST_CASE("TuiPreferences - round trips language and preserves sibling keymap writes", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    std::ofstream{path} << "foreign:\n  retained: yes\n";
    auto store = rt::ConfigStore{path};
    auto preferences = TuiPreferences{.language = "zh-Hant",
                                      .coverArtMode = "blocks",
                                      .dimBackdrop = false,
                                      .reducedMotion = true,
                                      .mouseEnabled = false,
                                      .qualityHover = false,
                                      .wheelStep = 7,
                                      .seekSeconds = 30,
                                      .volumePercent = 1};
    REQUIRE(saveTuiPreferences(store, preferences));
    auto keymap = uimodel::KeymapModel{uimodel::defaultKeymap()};
    REQUIRE(keymap.tryBind("foreign.action", *uimodel::KeyChord::parse("F12")));
    REQUIRE(uimodel::saveKeymap(store, keymap));
    auto reopened = rt::ConfigStore{path};
    auto loadedRes = loadTuiPreferences(reopened);
    REQUIRE(loadedRes);
    CHECK(*loadedRes == preferences);
    CHECK(ao::test::readFile(path).contains("retained: yes"));
    preferences.language.clear();
    REQUIRE(saveTuiPreferences(reopened, preferences));
    CHECK(uimodel::loadKeymap(reopened, uimodel::defaultKeymap()).chordsFor("foreign.action") ==
          keymap.chordsFor("foreign.action"));
    auto systemRes = loadTuiPreferences(reopened);
    REQUIRE(systemRes);
    CHECK(systemRes->language.empty());
  }

  TEST_CASE("TuiPreferences - accepts every selectable catalog locale and excludes pseudo", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};

    for (auto const& locale : i18n::availableCatalogLocales())
    {
      INFO(locale.tag);
      auto preferences = TuiPreferences{.language = std::string{locale.tag}};
      REQUIRE(saveTuiPreferences(store, preferences));
      auto reopened = rt::ConfigStore{path};
      auto loadedRes = loadTuiPreferences(reopened);
      REQUIRE(loadedRes);
      CHECK(loadedRes->language == locale.tag);
    }

    auto const original = ao::test::readFile(path);
    CHECK_FALSE(saveTuiPreferences(store, TuiPreferences{.language = "qps-ploc"}));
    CHECK(ao::test::readFile(path) == original);
  }

  TEST_CASE("TuiPreferences - rejects invalid values without replacing the saved document", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    auto store = rt::ConfigStore{path};
    REQUIRE(saveTuiPreferences(store, TuiPreferences{}));
    auto const original = ao::test::readFile(path);
    auto candidate = TuiPreferences{.language = "en_US!"};
    CHECK_FALSE(saveTuiPreferences(store, candidate));
    candidate.language.clear();
    candidate.wheelStep = 0;
    CHECK_FALSE(saveTuiPreferences(store, candidate));
    CHECK(ao::test::readFile(path) == original);
    auto noLocation = rt::ConfigStore{rt::ConfigStore::NoLocation{}};
    CHECK_FALSE(saveTuiPreferences(noLocation, TuiPreferences{}));
  }
} // namespace ao::tui::test
