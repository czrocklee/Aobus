// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/Preferences.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

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
    CHECK(ao::test::readFile(path).contains("retained: yes"));
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

  TEST_CASE("Preferences - a newer version is reported before its fields and never overwritten", "[tui][unit][config]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
    // A field this build rejects must not decide the outcome for a newer document.
    auto const document = std::string{"preferences: {version: 2, wheelStep: 0, fromTheFuture: true}"};
    std::ofstream{path} << document;
    auto store = rt::ConfigStore{path};

    auto const loadedRes = loadPreferences(store);
    REQUIRE_FALSE(loadedRes);
    CHECK(loadedRes.error().code == Error::Code::NotSupported);
    CHECK(store.hasNewerGroup("preferences"));

    auto const savedRes = savePreferences(store, Preferences{});
    REQUIRE_FALSE(savedRes);
    CHECK(savedRes.error().code == Error::Code::NotSupported);
    CHECK(ao::test::readFile(path) == document);
  }

  namespace
  {
    /// Loads the three groups as the TUI startup does, then reports what it could not use.
    std::vector<std::string> reportedConfigLoadFailures(std::string const& document)
    {
      auto temp = ao::test::TempDir{};
      auto const path = std::filesystem::path{temp.path()} / "tui.yaml";
      std::ofstream{path} << document;
      auto store = rt::ConfigStore{path};
      std::ignore = uimodel::loadKeymap(store, uimodel::KeymapBindings{});
      auto appPrefs = rt::AppPrefsState{};
      rt::loadAppPrefs(store, appPrefs);
      auto const preferencesRes = loadPreferences(store);

      auto executor = rt::test::InlineExecutor{};
      auto asyncRuntime = async::Runtime{executor, 1};
      auto notifications = rt::NotificationService{asyncRuntime};
      reportConfigLoadFailures(notifications, ao::test::englishMessageCatalog(), store, preferencesRes);

      auto messages = std::vector<std::string>{};

      for (auto const& entry : notifications.feed().entries)
      {
        CHECK(entry.severity == rt::NotificationSeverity::Warning);
        REQUIRE(std::holds_alternative<std::string>(entry.message));
        messages.push_back(std::get<std::string>(entry.message));
      }

      return messages;
    }
  } // namespace

  TEST_CASE("Preferences - startup reports each newer group with its own warning", "[tui][integration][config]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const messages = reportedConfigLoadFailures("shortcuts: {version: 99, future: true}\n"
                                                     "runtime: {version: 99, future: true}\n"
                                                     "preferences: {version: 99, future: true}\n");

    CHECK(messages == std::vector{std::string{i18n::requiredText(catalog, i18n::MessageId::ConfigNewerShortcuts)},
                                  std::string{i18n::requiredText(catalog, i18n::MessageId::TuiConfigNewerOutput)},
                                  std::string{i18n::requiredText(catalog, i18n::MessageId::TuiConfigNewerSettings)}});
  }

  TEST_CASE("Preferences - startup reports unreadable preferences with the reason", "[tui][integration][config]")
  {
    SECTION("preferences this build rejects post the load-failure warning")
    {
      auto const messages = reportedConfigLoadFailures("preferences: {version: 1, wheelStep: 0}\n");
      auto const prefix = i18n::requiredFormat(
        ao::test::englishMessageCatalog(), i18n::MessageId::TuiSettingsLoadFailed, {{"detail", ""}});

      REQUIRE(messages.size() == 1);
      CHECK(messages.front().starts_with(prefix));
      CHECK(messages.front().size() > prefix.size());
    }

    SECTION("readable configuration posts nothing")
    {
      CHECK(reportedConfigLoadFailures("preferences: {version: 1, wheelStep: 3}\n").empty());
    }
  }
} // namespace ao::tui::test
