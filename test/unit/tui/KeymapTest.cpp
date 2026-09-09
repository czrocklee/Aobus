// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/Keymap.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "tui/ShellInteractionModel.h"
#include <ao/Error.h>
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    uimodel::KeyChord chord(std::string_view const text)
    {
      auto optChord = uimodel::KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }

    std::string const& actionId(KeyAction const action)
    {
      auto const descriptors = actionDescriptors();
      auto const descriptor = std::ranges::find(descriptors, action, &ActionDescriptor::action);
      REQUIRE(descriptor != descriptors.end());
      return descriptor->actionId;
    }
  } // namespace

  TEST_CASE("Keymap - editing validates terminal aliases and reserved or unsupported events", "[tui][unit][keymap]")
  {
    auto keymap = uimodel::KeymapModel{};
    REQUIRE(keymap.tryBind("tui.shell.openSettings", *uimodel::KeyChord::parse("F12")));
    CHECK(validateActionBindings(keymap, "tui.shell.openSettings"));

    for (auto const* chord : {"Ctrl+C", "Escape", "Ctrl+Alt+F12"})
    {
      INFO(chord);
      auto candidate = uimodel::KeymapModel{};
      REQUIRE(candidate.tryBind("tui.shell.openSettings", *uimodel::KeyChord::parse(chord)));
      CHECK_FALSE(validateActionBindings(candidate, "tui.shell.openSettings"));
    }

    auto aliases = uimodel::KeymapModel{};
    REQUIRE(aliases.tryBind("tui.shell.openSettings", *uimodel::KeyChord::parse("Enter")));
    REQUIRE(aliases.tryBind("tui.shell.quit", *uimodel::KeyChord::parse("Ctrl+M")));
    auto conflictRes = validateActionBindings(aliases, "tui.shell.openSettings");
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().code == Error::Code::Conflict);
    CHECK(conflictRes.error().message == "tui.shell.quit");
  }

  TEST_CASE("Keymap - descriptors have stable unique identities and valid defaults", "[tui][unit][keymap]")
  {
    constexpr auto kExpected = std::to_array<std::pair<KeyAction, std::string_view>>({
      {KeyAction::OpenSettings, "tui.shell.openSettings"},
      {KeyAction::Quit, "tui.shell.quit"},
      {KeyAction::ToggleListChooser, "tui.shell.toggleListChooser"},
      {KeyAction::ToggleDetails, "tui.shell.toggleTrackDetail"},
      {KeyAction::ToggleAudioPipeline, "tui.shell.toggleAudioQuality"},
      {KeyAction::ToggleOutputDevices, "tui.shell.toggleOutputDevices"},
      {KeyAction::TogglePresentations, "tui.shell.togglePresentationChooser"},
      {KeyAction::ToggleNotifications, "tui.shell.toggleNotifications"},
      {KeyAction::ShowHelp, "tui.shell.showHelp"},
      {KeyAction::OpenCommandPalette, "tui.shell.openCommandPalette"},
      {KeyAction::OpenQuickFilter, "tui.library.openQuickFilter"},
      {KeyAction::RevealCurrentTrack, "workspace.revealCurrentTrack"},
      {KeyAction::ClearFilter, "tui.library.clearFilter"},
      {KeyAction::Reload, "tui.library.reloadActiveList"},
      {KeyAction::Scan, "tui.library.scan"},
      {KeyAction::ScanCancel, "tui.library.scanCancel"},
      {KeyAction::SelectToggle, "tui.library.selectToggle"},
      {KeyAction::SelectVisual, "tui.library.selectVisual"},
      {KeyAction::SelectAll, "tui.library.selectAll"},
      {KeyAction::SelectClear, "tui.library.selectClear"},
      {KeyAction::EditProperties, "tui.library.editProperties"},
      {KeyAction::PlaySelection, "tui.library.playSelection"},
      {KeyAction::PreviousTrack, "tui.library.previousTrack"},
      {KeyAction::NextTrack, "tui.library.nextTrack"},
      {KeyAction::PreviousSection, "tui.library.previousSection"},
      {KeyAction::NextSection, "tui.library.nextSection"},
      {KeyAction::SeekBackward, "tui.playback.seekBackward"},
      {KeyAction::SeekForward, "tui.playback.seekForward"},
      {KeyAction::VolumeDown, "tui.playback.volumeDown"},
      {KeyAction::VolumeUp, "tui.playback.volumeUp"},
      {KeyAction::PlaybackPlayPause, "playback.playPause"},
      {KeyAction::PlaybackStop, "playback.stop"},
    });
    auto ids = std::set<std::string_view>{};
    auto actions = std::set<KeyAction>{};

    REQUIRE(actionDescriptors().size() == kExpected.size());

    for (std::size_t index = 0; index < kExpected.size(); ++index)
    {
      auto const& descriptor = actionDescriptors()[index];
      CHECK_FALSE(descriptor.actionId.empty());
      CHECK(ids.insert(descriptor.actionId).second);
      CHECK(actions.insert(descriptor.action).second);
      CHECK(descriptor.action == kExpected[index].first);
      CHECK(descriptor.actionId == kExpected[index].second);

      for (auto const text : descriptor.defaultChords)
      {
        CHECK(uimodel::KeyChord::parse(text));
      }
    }

    CHECK(kExpected.size() == static_cast<std::size_t>(KeyAction::Count));
  }

  TEST_CASE("Keymap - terminal defaults extend but do not mutate shared defaults", "[tui][unit][keymap]")
  {
    auto const sharedBefore = uimodel::defaultKeymap();
    auto const tuiDefaults = defaultKeymap();
    auto const sharedAfter = uimodel::defaultKeymap();

    CHECK(sharedAfter == sharedBefore);
    CHECK_FALSE(sharedBefore.contains(actionId(KeyAction::ToggleListChooser)));
    REQUIRE(tuiDefaults.contains(actionId(KeyAction::ToggleListChooser)));
    CHECK(tuiDefaults.at(actionId(KeyAction::ToggleListChooser)) == std::vector{chord("L")});

    auto const& playPause = tuiDefaults.at(actionId(KeyAction::PlaybackPlayPause));
    REQUIRE(playPause.size() == 4);
    CHECK(playPause[0] == chord("Space"));
    CHECK(playPause[1] == chord("Ctrl+P"));
    CHECK(playPause[2] == chord("Media:Play"));
    CHECK(playPause[3] == chord("Media:Pause"));

    auto const& stop = tuiDefaults.at(actionId(KeyAction::PlaybackStop));
    REQUIRE(stop.size() == 2);
    CHECK(stop[0] == chord("S"));
    CHECK(stop[1] == chord("Media:Stop"));
  }

  TEST_CASE("Keymap - no-location global store retains defaults without persistence", "[tui][unit][keymap]")
  {
    auto store = rt::ConfigStore{rt::ConfigStore::NoLocation{}};
    auto const model = uimodel::loadKeymap(store, defaultKeymap());
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleListChooser);
    CHECK(plan.shortcutFor(KeyAction::OpenQuickFilter) == "/");
  }

  TEST_CASE("Keymap - default plan dispatches and advertises only executable chords", "[tui][unit][keymap]")
  {
    auto const model = uimodel::KeymapModel{defaultKeymap()};
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleListChooser);
    CHECK(plan.actionFor(ftxui::Event::Character("m")) == KeyAction::SelectToggle);
    CHECK(plan.actionFor(ftxui::Event::Character("A")) == KeyAction::SelectAll);
    CHECK(plan.actionFor(ftxui::Event::Character("a")) == KeyAction::ToggleAudioPipeline);
    CHECK(plan.actionFor(ftxui::Event::Character("V")) == KeyAction::SelectVisual);
    CHECK(plan.actionFor(ftxui::Event::Character("v")) == KeyAction::SelectVisual);
    CHECK(plan.actionFor(ftxui::Event::Character("p")) == KeyAction::TogglePresentations);
    CHECK(plan.actionFor(ftxui::Event::Character("j")) == KeyAction::NextTrack);
    CHECK(plan.actionFor(ftxui::Event::Character("k")) == KeyAction::PreviousTrack);
    CHECK(plan.actionFor(ftxui::Event::Return) == KeyAction::PlaySelection);
    CHECK(plan.actionFor(ftxui::Event::Character(" ")) == KeyAction::PlaybackPlayPause);
    CHECK(plan.actionFor(ftxui::Event::CtrlP) == KeyAction::PlaybackPlayPause);
    CHECK(plan.actionFor(ftxui::Event::CtrlL) == KeyAction::RevealCurrentTrack);
    CHECK(plan.shortcutFor(KeyAction::ToggleListChooser) == "l");
    CHECK(plan.shortcutFor(KeyAction::SelectToggle) == "m");
    CHECK(plan.shortcutFor(KeyAction::SelectClear) == "u");
    CHECK(plan.shortcutFor(KeyAction::SelectVisual) == "v");
    CHECK(plan.shortcutFor(KeyAction::TogglePresentations) == "p");
    CHECK(plan.shortcutFor(KeyAction::PlaybackPlayPause) == "Space");
    CHECK(plan.shortcutFor(KeyAction::PlaybackStop) == "s");

    for (auto const& descriptor : actionDescriptors())
    {
      auto const& chords = model.bindings().at(descriptor.actionId);

      if (chords.empty())
      {
        CHECK(plan.shortcutFor(descriptor.action).empty());
        continue;
      }

      CHECK_FALSE(plan.shortcutFor(descriptor.action).empty());

      for (auto const& candidate : chords)
      {
        if (auto const optEvent = eventForChord(candidate); optEvent)
        {
          CHECK(plan.actionFor(*optEvent) == descriptor.action);
        }
      }
    }
  }

  TEST_CASE("Keymap - terminal projection is explicit and accounts for protocol aliases", "[tui][unit][keymap]")
  {
    CHECK(eventForChord(chord("Q")) == ftxui::Event::Character("q"));
    CHECK(eventForChord(chord("M")) == ftxui::Event::Character("m"));
    CHECK(eventForChord(chord("U")) == ftxui::Event::Character("u"));
    CHECK(eventForChord(chord("7")) == ftxui::Event::Character("7"));
    CHECK(eventForChord(chord("=")) == ftxui::Event::Character("="));
    CHECK(eventForChord(chord("Shift+Q")) == ftxui::Event::Character("Q"));
    CHECK(eventForChord(chord("Enter")) == ftxui::Event::Return);
    CHECK(eventForChord(chord("Escape")) == ftxui::Event::Escape);
    CHECK(eventForChord(chord("Space")) == ftxui::Event::Character(" "));
    CHECK(eventForChord(chord("Tab")) == ftxui::Event::Tab);
    CHECK(eventForChord(chord("Backspace")) == ftxui::Event::Backspace);
    CHECK(eventForChord(chord("Insert")) == ftxui::Event::Insert);
    CHECK(eventForChord(chord("Delete")) == ftxui::Event::Delete);
    CHECK(eventForChord(chord("Left")) == ftxui::Event::ArrowLeft);
    CHECK(eventForChord(chord("Right")) == ftxui::Event::ArrowRight);
    CHECK(eventForChord(chord("Up")) == ftxui::Event::ArrowUp);
    CHECK(eventForChord(chord("Down")) == ftxui::Event::ArrowDown);
    CHECK(eventForChord(chord("Home")) == ftxui::Event::Home);
    CHECK(eventForChord(chord("End")) == ftxui::Event::End);
    CHECK(eventForChord(chord("PageUp")) == ftxui::Event::PageUp);
    CHECK(eventForChord(chord("PageDown")) == ftxui::Event::PageDown);
    CHECK(eventForChord(chord("F1")) == ftxui::Event::F1);
    CHECK(eventForChord(chord("F12")) == ftxui::Event::F12);
    CHECK(eventForChord(chord("Ctrl+I")) == ftxui::Event::Tab);
    CHECK(eventForChord(chord("Ctrl+J")) == ftxui::Event::Return);
    CHECK(eventForChord(chord("Ctrl+M")) == ftxui::Event::Return);
    CHECK(eventForChord(chord("Ctrl+H")) == ftxui::Event::Backspace);
    CHECK(eventForChord(chord("Ctrl+[")) == ftxui::Event::Escape);
    CHECK(eventForChord(chord("Ctrl+Left")) == ftxui::Event::ArrowLeftCtrl);
    CHECK(eventForChord(chord("Ctrl+Right")) == ftxui::Event::ArrowRightCtrl);
    CHECK(eventForChord(chord("Ctrl+Up")) == ftxui::Event::ArrowUpCtrl);
    CHECK(eventForChord(chord("Ctrl+Down")) == ftxui::Event::ArrowDownCtrl);
    CHECK(eventForChord(chord("Shift+Tab")) == ftxui::Event::TabReverse);
    CHECK(eventForChord(chord("Ctrl+C")) == ftxui::Event::CtrlC);
    CHECK_FALSE(eventForChord(chord("Shift+F1")));
    CHECK_FALSE(eventForChord(chord("Ctrl+1")));
    CHECK_FALSE(eventForChord(chord("Alt+Q")));
    CHECK_FALSE(eventForChord(chord("Ctrl+Shift+Q")));
    CHECK_FALSE(eventForChord(chord("Super+Q")));
    CHECK_FALSE(eventForChord(chord("Media:Play")));
    CHECK_FALSE(eventForChord(chord("翼")));
  }

  TEST_CASE("Keymap - override changes dispatch and the selected hint together", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({{actionId(KeyAction::ToggleDetails), {"F2"}}});
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("d")));
    CHECK(plan.actionFor(ftxui::Event::F2) == KeyAction::ToggleDetails);
    CHECK(plan.shortcutFor(KeyAction::ToggleDetails) == "F2");
  }

  TEST_CASE("Keymap - persisted override drives dispatch and rendered hint", "[tui][unit][keymap]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "tui.yaml";
    auto output = std::ofstream{configPath};
    output << "shortcuts:\n"
              "  tui.shell.toggleListChooser:\n"
              "    - F2\n";
    output.close();

    auto store = rt::ConfigStore{configPath};
    auto const model = uimodel::loadKeymap(store, defaultKeymap());
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("l")));
    CHECK(plan.actionFor(ftxui::Event::F2) == KeyAction::ToggleListChooser);
    CHECK(overlayHint(ao::test::englishMessageCatalog(), plan, Overlay::ListChooser) ==
          "F2 toggle  Enter open  Esc close");
  }

  TEST_CASE("Keymap - ordinary global preference writes preserve untouched shortcuts", "[tui][unit][keymap]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "tui.yaml";
    auto output = std::ofstream{configPath};
    output << "shortcuts:\n"
              "  tui.shell.toggleListChooser:\n"
              "    - F2\n"
              "  plugin.futureAction:\n"
              "    - Ctrl+\n";
    output.close();

    auto store = rt::ConfigStore{configPath};
    auto const model = uimodel::loadKeymap(store, defaultKeymap());
    REQUIRE(model.chordsFor(actionId(KeyAction::ToggleListChooser)) == std::vector{chord("F2")});

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "night";
    REQUIRE(rt::saveAppPrefs(store, prefs));

    auto input = std::ifstream{configPath};
    auto const contents = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    CHECK(contents.contains("plugin.futureAction"));
    CHECK(contents.contains("Ctrl+"));
    CHECK(contents.contains("F2"));
  }

  TEST_CASE("Keymap - explicit unbinding removes dispatch and hint", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({{actionId(KeyAction::ToggleListChooser), {}}});
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("l")));
    CHECK(plan.shortcutFor(KeyAction::ToggleListChooser).empty());
  }

  TEST_CASE("Keymap - root protocol events are representable but never installed or advertised", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {actionId(KeyAction::Quit), {"Ctrl+C", "Up", "Home", "PageUp"}},
      {actionId(KeyAction::ToggleListChooser), {"Escape", "Down", "End", "PageDown"}},
    });
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::CtrlC));
    CHECK_FALSE(plan.actionFor(ftxui::Event::Escape));
    CHECK_FALSE(plan.actionFor(ftxui::Event::ArrowUp));
    CHECK_FALSE(plan.actionFor(ftxui::Event::ArrowDown));
    CHECK_FALSE(plan.actionFor(ftxui::Event::Home));
    CHECK_FALSE(plan.actionFor(ftxui::Event::End));
    CHECK_FALSE(plan.actionFor(ftxui::Event::PageUp));
    CHECK_FALSE(plan.actionFor(ftxui::Event::PageDown));
    CHECK(plan.shortcutFor(KeyAction::Quit).empty());
    CHECK(plan.shortcutFor(KeyAction::ToggleListChooser).empty());
  }

  TEST_CASE("Keymap - unsupported and unknown bindings are local omissions", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {actionId(KeyAction::ToggleDetails), {"Super+D", "F3"}},
      {"unknown.future.action", {"L"}},
    });
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::F3) == KeyAction::ToggleDetails);
    CHECK(plan.shortcutFor(KeyAction::ToggleDetails) == "F3");
    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleListChooser);
  }

  TEST_CASE("Keymap - projected collision keeps the earlier descriptor deterministically", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {actionId(KeyAction::ToggleListChooser), {"Tab"}},
      {actionId(KeyAction::ToggleDetails), {"Ctrl+I"}},
    });
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Tab) == KeyAction::ToggleListChooser);
    CHECK(plan.shortcutFor(KeyAction::ToggleListChooser) == "Tab");
    CHECK(plan.shortcutFor(KeyAction::ToggleDetails).empty());
  }
} // namespace ao::tui::test
