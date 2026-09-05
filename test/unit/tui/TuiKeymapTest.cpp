// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TuiKeymap.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "tui/ShellInteractionModel.h"
#include <ao/rt/AppState.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>

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

    std::string const& actionId(TuiKeyAction const action)
    {
      auto const descriptors = tuiActionDescriptors();
      auto const descriptor = std::ranges::find(descriptors, action, &TuiActionDescriptor::action);
      REQUIRE(descriptor != descriptors.end());
      return descriptor->actionId;
    }
  } // namespace

  TEST_CASE("TuiKeymap - descriptors have stable unique identities and valid defaults", "[tui][unit][keymap]")
  {
    constexpr auto kExpected = std::to_array<std::pair<TuiKeyAction, std::string_view>>({
      {TuiKeyAction::Quit, "tui.shell.quit"},
      {TuiKeyAction::ToggleListChooser, "tui.shell.toggleListChooser"},
      {TuiKeyAction::ToggleDetails, "tui.shell.toggleTrackDetail"},
      {TuiKeyAction::ToggleAudioPipeline, "tui.shell.toggleAudioQuality"},
      {TuiKeyAction::ToggleOutputDevices, "tui.shell.toggleOutputDevices"},
      {TuiKeyAction::TogglePresentations, "tui.shell.togglePresentationChooser"},
      {TuiKeyAction::ToggleNotifications, "tui.shell.toggleNotifications"},
      {TuiKeyAction::ShowHelp, "tui.shell.showHelp"},
      {TuiKeyAction::OpenCommandPalette, "tui.shell.openCommandPalette"},
      {TuiKeyAction::OpenQuickFilter, "tui.library.openQuickFilter"},
      {TuiKeyAction::RevealCurrentTrack, "workspace.revealCurrentTrack"},
      {TuiKeyAction::ClearFilter, "tui.library.clearFilter"},
      {TuiKeyAction::Reload, "tui.library.reloadActiveList"},
      {TuiKeyAction::Scan, "tui.library.scan"},
      {TuiKeyAction::ScanCancel, "tui.library.scanCancel"},
      {TuiKeyAction::SelectToggle, "tui.library.selectToggle"},
      {TuiKeyAction::SelectVisual, "tui.library.selectVisual"},
      {TuiKeyAction::SelectAll, "tui.library.selectAll"},
      {TuiKeyAction::SelectClear, "tui.library.selectClear"},
      {TuiKeyAction::PlaySelection, "tui.library.playSelection"},
      {TuiKeyAction::PreviousTrack, "tui.library.previousTrack"},
      {TuiKeyAction::NextTrack, "tui.library.nextTrack"},
      {TuiKeyAction::PreviousSection, "tui.library.previousSection"},
      {TuiKeyAction::NextSection, "tui.library.nextSection"},
      {TuiKeyAction::SeekBackward, "tui.playback.seekBackward"},
      {TuiKeyAction::SeekForward, "tui.playback.seekForward"},
      {TuiKeyAction::VolumeDown, "tui.playback.volumeDown"},
      {TuiKeyAction::VolumeUp, "tui.playback.volumeUp"},
      {TuiKeyAction::PlaybackPlayPause, "playback.playPause"},
      {TuiKeyAction::PlaybackStop, "playback.stop"},
    });
    auto ids = std::set<std::string_view>{};
    auto actions = std::set<TuiKeyAction>{};

    REQUIRE(tuiActionDescriptors().size() == kExpected.size());

    for (std::size_t index = 0; index < kExpected.size(); ++index)
    {
      auto const& descriptor = tuiActionDescriptors()[index];
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

    CHECK(kExpected.size() == static_cast<std::size_t>(TuiKeyAction::Count));
  }

  TEST_CASE("TuiKeymap - terminal defaults extend but do not mutate shared defaults", "[tui][unit][keymap]")
  {
    auto const sharedBefore = uimodel::defaultKeymap();
    auto const tuiDefaults = tuiDefaultKeymap();
    auto const sharedAfter = uimodel::defaultKeymap();

    CHECK(sharedAfter == sharedBefore);
    CHECK_FALSE(sharedBefore.contains(actionId(TuiKeyAction::ToggleListChooser)));
    REQUIRE(tuiDefaults.contains(actionId(TuiKeyAction::ToggleListChooser)));
    CHECK(tuiDefaults.at(actionId(TuiKeyAction::ToggleListChooser)) == std::vector{chord("L")});

    auto const& playPause = tuiDefaults.at(actionId(TuiKeyAction::PlaybackPlayPause));
    REQUIRE(playPause.size() == 4);
    CHECK(playPause[0] == chord("Space"));
    CHECK(playPause[1] == chord("Ctrl+P"));
    CHECK(playPause[2] == chord("Media:Play"));
    CHECK(playPause[3] == chord("Media:Pause"));

    auto const& stop = tuiDefaults.at(actionId(TuiKeyAction::PlaybackStop));
    REQUIRE(stop.size() == 2);
    CHECK(stop[0] == chord("S"));
    CHECK(stop[1] == chord("Media:Stop"));
  }

  TEST_CASE("TuiKeymap - no-location global store retains defaults without persistence", "[tui][unit][keymap]")
  {
    auto store = rt::ConfigStore{rt::ConfigStore::NoLocation{}};
    auto const model = uimodel::loadKeymap(store, tuiDefaultKeymap());
    auto const plan = TuiKeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Character("l")) == TuiKeyAction::ToggleListChooser);
    CHECK(plan.shortcutFor(TuiKeyAction::OpenQuickFilter) == "/");
  }

  TEST_CASE("TuiKeymap - default plan dispatches and advertises only executable chords", "[tui][unit][keymap]")
  {
    auto const model = uimodel::KeymapModel{tuiDefaultKeymap()};
    auto const plan = TuiKeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Character("l")) == TuiKeyAction::ToggleListChooser);
    CHECK(plan.actionFor(ftxui::Event::Character("m")) == TuiKeyAction::SelectToggle);
    CHECK(plan.actionFor(ftxui::Event::Character("A")) == TuiKeyAction::SelectAll);
    CHECK(plan.actionFor(ftxui::Event::Character("a")) == TuiKeyAction::ToggleAudioPipeline);
    CHECK(plan.actionFor(ftxui::Event::Character("V")) == TuiKeyAction::SelectVisual);
    CHECK(plan.actionFor(ftxui::Event::Character("v")) == TuiKeyAction::SelectVisual);
    CHECK(plan.actionFor(ftxui::Event::Character("p")) == TuiKeyAction::TogglePresentations);
    CHECK(plan.actionFor(ftxui::Event::Character("j")) == TuiKeyAction::NextTrack);
    CHECK(plan.actionFor(ftxui::Event::Character("k")) == TuiKeyAction::PreviousTrack);
    CHECK(plan.actionFor(ftxui::Event::Return) == TuiKeyAction::PlaySelection);
    CHECK(plan.actionFor(ftxui::Event::Character(" ")) == TuiKeyAction::PlaybackPlayPause);
    CHECK(plan.actionFor(ftxui::Event::CtrlP) == TuiKeyAction::PlaybackPlayPause);
    CHECK(plan.actionFor(ftxui::Event::CtrlL) == TuiKeyAction::RevealCurrentTrack);
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleListChooser) == "l");
    CHECK(plan.shortcutFor(TuiKeyAction::SelectToggle) == "m");
    CHECK(plan.shortcutFor(TuiKeyAction::SelectClear) == "u");
    CHECK(plan.shortcutFor(TuiKeyAction::SelectVisual) == "v");
    CHECK(plan.shortcutFor(TuiKeyAction::TogglePresentations) == "p");
    CHECK(plan.shortcutFor(TuiKeyAction::PlaybackPlayPause) == "Space");
    CHECK(plan.shortcutFor(TuiKeyAction::PlaybackStop) == "s");

    for (auto const& descriptor : tuiActionDescriptors())
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
        if (auto const optEvent = tuiEventForChord(candidate); optEvent)
        {
          CHECK(plan.actionFor(*optEvent) == descriptor.action);
        }
      }
    }
  }

  TEST_CASE("TuiKeymap - terminal projection is explicit and accounts for protocol aliases", "[tui][unit][keymap]")
  {
    CHECK(tuiEventForChord(chord("Q")) == ftxui::Event::Character("q"));
    CHECK(tuiEventForChord(chord("M")) == ftxui::Event::Character("m"));
    CHECK(tuiEventForChord(chord("U")) == ftxui::Event::Character("u"));
    CHECK(tuiEventForChord(chord("7")) == ftxui::Event::Character("7"));
    CHECK(tuiEventForChord(chord("=")) == ftxui::Event::Character("="));
    CHECK(tuiEventForChord(chord("Shift+Q")) == ftxui::Event::Character("Q"));
    CHECK(tuiEventForChord(chord("Enter")) == ftxui::Event::Return);
    CHECK(tuiEventForChord(chord("Escape")) == ftxui::Event::Escape);
    CHECK(tuiEventForChord(chord("Space")) == ftxui::Event::Character(" "));
    CHECK(tuiEventForChord(chord("Tab")) == ftxui::Event::Tab);
    CHECK(tuiEventForChord(chord("Backspace")) == ftxui::Event::Backspace);
    CHECK(tuiEventForChord(chord("Insert")) == ftxui::Event::Insert);
    CHECK(tuiEventForChord(chord("Delete")) == ftxui::Event::Delete);
    CHECK(tuiEventForChord(chord("Left")) == ftxui::Event::ArrowLeft);
    CHECK(tuiEventForChord(chord("Right")) == ftxui::Event::ArrowRight);
    CHECK(tuiEventForChord(chord("Up")) == ftxui::Event::ArrowUp);
    CHECK(tuiEventForChord(chord("Down")) == ftxui::Event::ArrowDown);
    CHECK(tuiEventForChord(chord("Home")) == ftxui::Event::Home);
    CHECK(tuiEventForChord(chord("End")) == ftxui::Event::End);
    CHECK(tuiEventForChord(chord("PageUp")) == ftxui::Event::PageUp);
    CHECK(tuiEventForChord(chord("PageDown")) == ftxui::Event::PageDown);
    CHECK(tuiEventForChord(chord("F1")) == ftxui::Event::F1);
    CHECK(tuiEventForChord(chord("F12")) == ftxui::Event::F12);
    CHECK(tuiEventForChord(chord("Ctrl+I")) == ftxui::Event::Tab);
    CHECK(tuiEventForChord(chord("Ctrl+J")) == ftxui::Event::Return);
    CHECK(tuiEventForChord(chord("Ctrl+M")) == ftxui::Event::Return);
    CHECK(tuiEventForChord(chord("Ctrl+H")) == ftxui::Event::Backspace);
    CHECK(tuiEventForChord(chord("Ctrl+[")) == ftxui::Event::Escape);
    CHECK(tuiEventForChord(chord("Ctrl+Left")) == ftxui::Event::ArrowLeftCtrl);
    CHECK(tuiEventForChord(chord("Ctrl+Right")) == ftxui::Event::ArrowRightCtrl);
    CHECK(tuiEventForChord(chord("Ctrl+Up")) == ftxui::Event::ArrowUpCtrl);
    CHECK(tuiEventForChord(chord("Ctrl+Down")) == ftxui::Event::ArrowDownCtrl);
    CHECK(tuiEventForChord(chord("Shift+Tab")) == ftxui::Event::TabReverse);
    CHECK(tuiEventForChord(chord("Ctrl+C")) == ftxui::Event::CtrlC);
    CHECK_FALSE(tuiEventForChord(chord("Shift+F1")));
    CHECK_FALSE(tuiEventForChord(chord("Ctrl+1")));
    CHECK_FALSE(tuiEventForChord(chord("Alt+Q")));
    CHECK_FALSE(tuiEventForChord(chord("Ctrl+Shift+Q")));
    CHECK_FALSE(tuiEventForChord(chord("Super+Q")));
    CHECK_FALSE(tuiEventForChord(chord("Media:Play")));
    CHECK_FALSE(tuiEventForChord(chord("翼")));
  }

  TEST_CASE("TuiKeymap - override changes dispatch and the selected hint together", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({{actionId(TuiKeyAction::ToggleDetails), {"F2"}}});
    auto const plan = TuiKeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("d")));
    CHECK(plan.actionFor(ftxui::Event::F2) == TuiKeyAction::ToggleDetails);
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleDetails) == "F2");
  }

  TEST_CASE("TuiKeymap - persisted override drives dispatch and rendered hint", "[tui][unit][keymap]")
  {
    auto const tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "tui.yaml";
    auto output = std::ofstream{configPath};
    output << "shortcuts:\n"
              "  tui.shell.toggleListChooser:\n"
              "    - F2\n";
    output.close();

    auto store = rt::ConfigStore{configPath};
    auto const model = uimodel::loadKeymap(store, tuiDefaultKeymap());
    auto const plan = TuiKeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("l")));
    CHECK(plan.actionFor(ftxui::Event::F2) == TuiKeyAction::ToggleListChooser);
    CHECK(overlayHint(ao::test::englishMessageCatalog(), plan, Overlay::ListChooser) ==
          "F2 toggle  Enter open  Esc close");
  }

  TEST_CASE("TuiKeymap - ordinary global preference writes preserve untouched shortcuts", "[tui][unit][keymap]")
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
    auto const model = uimodel::loadKeymap(store, tuiDefaultKeymap());
    REQUIRE(model.chordsFor(actionId(TuiKeyAction::ToggleListChooser)) == std::vector{chord("F2")});

    auto prefs = rt::AppPrefsState{};
    prefs.lastThemePreset = "night";
    REQUIRE(rt::saveAppPrefs(store, prefs));

    auto input = std::ifstream{configPath};
    auto const contents = std::string{std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    CHECK(contents.contains("plugin.futureAction"));
    CHECK(contents.contains("Ctrl+"));
    CHECK(contents.contains("F2"));
  }

  TEST_CASE("TuiKeymap - explicit unbinding removes dispatch and hint", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({{actionId(TuiKeyAction::ToggleListChooser), {}}});
    auto const plan = TuiKeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("l")));
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleListChooser).empty());
  }

  TEST_CASE("TuiKeymap - root protocol events are representable but never installed or advertised",
            "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({
      {actionId(TuiKeyAction::Quit), {"Ctrl+C", "Up", "Home", "PageUp"}},
      {actionId(TuiKeyAction::ToggleListChooser), {"Escape", "Down", "End", "PageDown"}},
    });
    auto const plan = TuiKeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::CtrlC));
    CHECK_FALSE(plan.actionFor(ftxui::Event::Escape));
    CHECK_FALSE(plan.actionFor(ftxui::Event::ArrowUp));
    CHECK_FALSE(plan.actionFor(ftxui::Event::ArrowDown));
    CHECK_FALSE(plan.actionFor(ftxui::Event::Home));
    CHECK_FALSE(plan.actionFor(ftxui::Event::End));
    CHECK_FALSE(plan.actionFor(ftxui::Event::PageUp));
    CHECK_FALSE(plan.actionFor(ftxui::Event::PageDown));
    CHECK(plan.shortcutFor(TuiKeyAction::Quit).empty());
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleListChooser).empty());
  }

  TEST_CASE("TuiKeymap - unsupported and unknown bindings are local omissions", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({
      {actionId(TuiKeyAction::ToggleDetails), {"Super+D", "F3"}},
      {"unknown.future.action", {"L"}},
    });
    auto const plan = TuiKeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::F3) == TuiKeyAction::ToggleDetails);
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleDetails) == "F3");
    CHECK(plan.actionFor(ftxui::Event::Character("l")) == TuiKeyAction::ToggleListChooser);
  }

  TEST_CASE("TuiKeymap - projected collision keeps the earlier descriptor deterministically", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{tuiDefaultKeymap()};
    model.applyOverrides({
      {actionId(TuiKeyAction::ToggleListChooser), {"Tab"}},
      {actionId(TuiKeyAction::ToggleDetails), {"Ctrl+I"}},
    });
    auto const plan = TuiKeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Tab) == TuiKeyAction::ToggleListChooser);
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleListChooser) == "Tab");
    CHECK(plan.shortcutFor(TuiKeyAction::ToggleDetails).empty());
  }
} // namespace ao::tui::test
