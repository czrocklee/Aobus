// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/Keymap.h"

#include "test/unit/TestFixtureSupport.h"
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
      {KeyAction::ToggleLists, "tui.shell.toggleListChooser"},
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
      {KeyAction::PreviousRow, "tui.library.previousRow"},
      {KeyAction::NextRow, "tui.library.nextRow"},
      {KeyAction::PreviousSection, "tui.library.previousSection"},
      {KeyAction::NextSection, "tui.library.nextSection"},
      {KeyAction::SeekBackward, "tui.playback.seekBackward"},
      {KeyAction::SeekForward, "tui.playback.seekForward"},
      {KeyAction::VolumeDown, "tui.playback.volumeDown"},
      {KeyAction::VolumeUp, "tui.playback.volumeUp"},
      {KeyAction::PlaybackPlayPause, "playback.playPause"},
      {KeyAction::PlaybackStop, "playback.stop"},
      {KeyAction::PlaybackPrevious, "playback.previous"},
      {KeyAction::PlaybackNext, "playback.next"},
      {KeyAction::PlaybackShuffle, "playback.toggleShuffle"},
      {KeyAction::PlaybackRepeat, "playback.cycleRepeat"},
      {KeyAction::FocusDetails, "tui.detail.focus"},
      {KeyAction::SwitchWorkspaceFocus, "tui.workspace.switchFocus"},
      {KeyAction::OpenGoTo, "tui.navigation.openGoTo"},
      {KeyAction::OpenCurrentArtist, "tui.navigation.currentArtist"},
      {KeyAction::OpenCurrentAlbum, "tui.navigation.currentAlbum"},
      {KeyAction::WorkspaceBack, "tui.navigation.back"},
      {KeyAction::WorkspaceForward, "tui.navigation.forward"},
      {KeyAction::BeginPanelResize, "tui.workspace.beginPanelResize"},
      {KeyAction::TogglePinnedLists, "tui.workspace.togglePinnedLists"},
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

  TEST_CASE("Keymap - terminal defaults are independent of desktop defaults", "[tui][unit][keymap]")
  {
    auto const sharedBefore = uimodel::defaultKeymap();
    auto const tuiDefaults = defaultKeymap();
    auto const sharedAfter = uimodel::defaultKeymap();

    CHECK(sharedAfter == sharedBefore);
    CHECK_FALSE(sharedBefore.contains(actionId(KeyAction::ToggleLists)));
    REQUIRE(tuiDefaults.contains(actionId(KeyAction::ToggleLists)));
    CHECK(tuiDefaults.at(actionId(KeyAction::ToggleLists)) == std::vector{chord("L")});

    auto const& playPause = tuiDefaults.at(actionId(KeyAction::PlaybackPlayPause));
    CHECK(playPause == std::vector{chord("Space")});
    CHECK(tuiDefaults.at(actionId(KeyAction::PlaybackStop)) == std::vector{chord("S")});
    CHECK_FALSE(tuiDefaults.contains("track.orderMoveUp"));
  }

  TEST_CASE("Keymap - an explicit quit override preserves the lowercase chord", "[tui][unit][keymap]")
  {
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.shell.quit", {"Q"}}});
    auto const plan = KeymapPlan{keymap};

    CHECK(plan.actionFor(ftxui::Event::Character("q")) == KeyAction::Quit);
    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("Q")));
    CHECK(plan.shortcutFor(KeyAction::Quit) == "q");
  }

  TEST_CASE("Keymap - no-location global store retains defaults without persistence", "[tui][unit][keymap]")
  {
    auto store = rt::ConfigStore{rt::ConfigStore::NoLocation{}};
    auto const model = uimodel::loadKeymap(store, defaultKeymap());
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleLists);
    CHECK(plan.shortcutFor(KeyAction::OpenQuickFilter) == "/");
  }

  TEST_CASE("Keymap - default plan dispatches and advertises only executable chords", "[tui][unit][keymap]")
  {
    auto const model = uimodel::KeymapModel{defaultKeymap()};
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("q")));
    CHECK(plan.actionFor(ftxui::Event::Character("Q")) == KeyAction::Quit);
    CHECK(plan.shortcutFor(KeyAction::Quit) == "Q");
    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleLists);
    CHECK(plan.actionFor(ftxui::Event::Character("m")) == KeyAction::SelectToggle);
    CHECK(plan.actionFor(ftxui::Event::Character("A")) == KeyAction::SelectAll);
    CHECK(plan.actionFor(ftxui::Event::Character("a")) == KeyAction::ToggleAudioPipeline);
    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("V")));
    CHECK(plan.actionFor(ftxui::Event::Character("v")) == KeyAction::SelectVisual);
    CHECK(plan.actionFor(ftxui::Event::Character("p")) == KeyAction::TogglePresentations);
    CHECK(plan.actionFor(ftxui::Event::Character("j")) == KeyAction::NextRow);
    CHECK(plan.actionFor(ftxui::Event::Character("k")) == KeyAction::PreviousRow);
    CHECK(plan.actionFor(ftxui::Event::Return) == KeyAction::PlaySelection);
    CHECK(plan.actionFor(ftxui::Event::Character(" ")) == KeyAction::PlaybackPlayPause);
    CHECK_FALSE(plan.actionFor(ftxui::Event::CtrlP));
    CHECK_FALSE(plan.actionFor(ftxui::Event::CtrlL));
    CHECK(plan.actionFor(ftxui::Event::Special("\x17")) == KeyAction::BeginPanelResize);
    CHECK(plan.actionFor(ftxui::Event::Character("c")) == KeyAction::RevealCurrentTrack);
    CHECK(plan.actionFor(ftxui::Event::Character("C")) == KeyAction::ClearFilter);
    CHECK(plan.actionFor(ftxui::Event::Character("r")) == KeyAction::PlaybackRepeat);
    CHECK(plan.actionFor(ftxui::Event::Character("R")) == KeyAction::Reload);
    CHECK(plan.actionFor(ftxui::Event::Character("S")) == KeyAction::PlaybackShuffle);
    CHECK(plan.actionFor(ftxui::Event::Character("<")) == KeyAction::PlaybackPrevious);
    CHECK(plan.actionFor(ftxui::Event::Character(">")) == KeyAction::PlaybackNext);
    CHECK(plan.actionFor(ftxui::Event::ArrowLeft) == KeyAction::SeekBackward);
    CHECK(plan.actionFor(ftxui::Event::ArrowRight) == KeyAction::SeekForward);
    CHECK(plan.actionFor(ftxui::Event::ArrowLeftCtrl) == KeyAction::PlaybackPrevious);
    CHECK(plan.actionFor(ftxui::Event::ArrowRightCtrl) == KeyAction::PlaybackNext);
    CHECK(plan.actionFor(ftxui::Event::F1) == KeyAction::ShowHelp);
    CHECK(plan.shortcutFor(KeyAction::ToggleLists) == "l");
    CHECK(plan.shortcutFor(KeyAction::SelectToggle) == "m");
    CHECK(plan.shortcutFor(KeyAction::SelectClear) == "u");
    CHECK(keyChordLabel(chord("C")) == "c");
    CHECK(keyChordLabel(chord("Shift+C")) == "C");
    CHECK(keyChordLabel(chord("Ctrl+C")) == "Ctrl+C");
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
    CHECK(plan.actionFor(ftxui::Event::F2) == KeyAction::ToggleLists);
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
    REQUIRE(model.chordsFor(actionId(KeyAction::ToggleLists)) == std::vector{chord("F2")});

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
    model.applyOverrides({{actionId(KeyAction::ToggleLists), {}}});
    auto const plan = KeymapPlan{model};

    CHECK_FALSE(plan.actionFor(ftxui::Event::Character("l")));
    CHECK(plan.shortcutFor(KeyAction::ToggleLists).empty());
  }

  TEST_CASE("Keymap - root protocol events are representable but never installed or advertised", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {actionId(KeyAction::Quit), {"Ctrl+C", "Up", "Home", "PageUp"}},
      {actionId(KeyAction::ToggleLists), {"Escape", "Down", "End", "PageDown"}},
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
    CHECK(plan.shortcutFor(KeyAction::ToggleLists).empty());
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
    CHECK(plan.actionFor(ftxui::Event::Character("l")) == KeyAction::ToggleLists);
  }

  TEST_CASE("Keymap - projected collision keeps the earlier descriptor deterministically", "[tui][unit][keymap]")
  {
    auto model = uimodel::KeymapModel{defaultKeymap()};
    model.applyOverrides({
      {actionId(KeyAction::ToggleLists), {"Tab"}},
      {actionId(KeyAction::ToggleDetails), {"Ctrl+I"}},
    });
    auto const plan = KeymapPlan{model};

    CHECK(plan.actionFor(ftxui::Event::Tab) == KeyAction::ToggleLists);
    CHECK(plan.shortcutFor(KeyAction::ToggleLists) == "Tab");
    CHECK(plan.shortcutFor(KeyAction::ToggleDetails).empty());
  }
} // namespace ao::tui::test
