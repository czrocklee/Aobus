// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Command.h"

#include "tui/Keymap.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    Command requiredCommand(std::string_view const input)
    {
      auto const optCommand = parseCommand(input);
      REQUIRE(optCommand);
      return *optCommand;
    }
  } // namespace

  TEST_CASE("Command - command parser recognizes terminal app commands", "[tui][unit][shell]")
  {
    CHECK(requiredCommand(":lists").action == CommandAction::OpenLists);
    CHECK(requiredCommand(":detail").action == CommandAction::OpenDetail);
    CHECK(requiredCommand("details").action == CommandAction::OpenDetail);
    CHECK(requiredCommand(":quality").action == CommandAction::OpenQuality);
    CHECK(requiredCommand("audio").action == CommandAction::OpenQuality);
    CHECK(requiredCommand(":pipeline").action == CommandAction::OpenQuality);
    CHECK(requiredCommand(":output").action == CommandAction::OpenOutputDevices);
    CHECK(requiredCommand(":devices").action == CommandAction::OpenOutputDevices);
    CHECK(requiredCommand(":views").action == CommandAction::OpenPresentationPanel);
    CHECK(requiredCommand(":p").action == CommandAction::OpenPresentationPanel);
    CHECK(requiredCommand(":notifications").action == CommandAction::OpenNotifications);
    CHECK(requiredCommand(":n").action == CommandAction::OpenNotifications);
    CHECK(requiredCommand("help").action == CommandAction::ShowHelp);
    CHECK(requiredCommand(":current").action == CommandAction::RevealCurrentTrack);
    auto presentationCommand = requiredCommand(":view albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "albums");
    presentationCommand = requiredCommand(":presentation tagging");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "tagging");
    presentationCommand = requiredCommand(":preset Albums");
    CHECK(presentationCommand.action == CommandAction::SetPresentation);
    CHECK(presentationCommand.argument == "Albums");
    CHECK(requiredCommand("now").action == CommandAction::RevealCurrentTrack);
    CHECK(requiredCommand("reveal").action == CommandAction::RevealCurrentTrack);
    CHECK(requiredCommand("clear").action == CommandAction::ClearFilter);
    CHECK(requiredCommand("reload").action == CommandAction::Reload);
    CHECK(requiredCommand("refresh").action == CommandAction::Reload);
    CHECK(requiredCommand("scan").action == CommandAction::Scan);
    CHECK(requiredCommand("rescan").action == CommandAction::Scan);
    CHECK(requiredCommand("scan cancel").action == CommandAction::ScanCancel);
    CHECK(requiredCommand(":scan cancel").action == CommandAction::ScanCancel);
    CHECK(requiredCommand("select toggle").action == CommandAction::SelectToggle);
    CHECK(requiredCommand("select visual").action == CommandAction::SelectVisual);
    CHECK(requiredCommand("select all").action == CommandAction::SelectAll);
    CHECK(requiredCommand("select clear").action == CommandAction::SelectClear);
    CHECK(requiredCommand("settings").action == CommandAction::OpenSettings);
    CHECK(requiredCommand("config").action == CommandAction::OpenSettings);
    CHECK(requiredCommand("edit").action == CommandAction::EditProperties);
    CHECK(requiredCommand("properties").action == CommandAction::EditProperties);
    CHECK(requiredCommand("play").action == CommandAction::Play);
    CHECK(requiredCommand("pause").action == CommandAction::TogglePlayback);
    CHECK(requiredCommand("stop").action == CommandAction::Stop);
    CHECK(requiredCommand("quit").action == CommandAction::Quit);
    CHECK(requiredCommand("close").action == CommandAction::CloseOverlay);
    CHECK(requiredCommand("hide").action == CommandAction::CloseOverlay);
    CHECK(requiredCommand("esc").action == CommandAction::CloseOverlay);
  }

  TEST_CASE("Command - command and root key actions share one relation", "[tui][unit][keymap]")
  {
    constexpr auto kRelations = std::to_array<std::pair<CommandAction, KeyAction>>({
      {CommandAction::OpenLists, KeyAction::ToggleListChooser},
      {CommandAction::OpenDetail, KeyAction::ToggleDetails},
      {CommandAction::OpenQuality, KeyAction::ToggleAudioPipeline},
      {CommandAction::OpenOutputDevices, KeyAction::ToggleOutputDevices},
      {CommandAction::OpenPresentationPanel, KeyAction::TogglePresentations},
      {CommandAction::OpenNotifications, KeyAction::ToggleNotifications},
      {CommandAction::ShowHelp, KeyAction::ShowHelp},
      {CommandAction::RevealCurrentTrack, KeyAction::RevealCurrentTrack},
      {CommandAction::ClearFilter, KeyAction::ClearFilter},
      {CommandAction::Reload, KeyAction::Reload},
      {CommandAction::Scan, KeyAction::Scan},
      {CommandAction::ScanCancel, KeyAction::ScanCancel},
      {CommandAction::SelectToggle, KeyAction::SelectToggle},
      {CommandAction::SelectVisual, KeyAction::SelectVisual},
      {CommandAction::SelectAll, KeyAction::SelectAll},
      {CommandAction::SelectClear, KeyAction::SelectClear},
      {CommandAction::OpenSettings, KeyAction::OpenSettings},
      {CommandAction::EditProperties, KeyAction::EditProperties},
      {CommandAction::Play, KeyAction::PlaySelection},
      {CommandAction::TogglePlayback, KeyAction::PlaybackPlayPause},
      {CommandAction::Stop, KeyAction::PlaybackStop},
      {CommandAction::Quit, KeyAction::Quit},
    });

    for (auto const& [command, key] : kRelations)
    {
      CHECK(shortcutActionForCommand(command) == key);
      CHECK(commandActionForKeyAction(key) == command);
    }

    CHECK_FALSE(shortcutActionForCommand(CommandAction::QuickFilter));
    CHECK_FALSE(shortcutActionForCommand(CommandAction::CloseOverlay));
    CHECK_FALSE(shortcutActionForCommand(CommandAction::SetPresentation));
    CHECK_FALSE(commandActionForKeyAction(KeyAction::OpenCommandPalette));
    CHECK_FALSE(commandActionForKeyAction(KeyAction::OpenQuickFilter));
    CHECK_FALSE(commandActionForKeyAction(KeyAction::PreviousSection));
  }

  TEST_CASE("Command - only explicit filter commands become quick filters", "[tui][unit][shell]")
  {
    CHECK_FALSE(parseCommand("/aimer midnight"));
    CHECK_FALSE(parseCommand("/lists"));
    CHECK_FALSE(parseCommand("/filter live acoustic"));
    CHECK_FALSE(parseCommand("unknown"));
    CHECK_FALSE(parseCommand("scan foo"));
    CHECK_FALSE(parseCommand("scan  cancel"));
    CHECK_FALSE(parseCommand("select"));
    CHECK_FALSE(parseCommand("  "));

    auto command = requiredCommand(":filter live acoustic");
    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "live acoustic");

    command = requiredCommand("  :filter   spaced query   ");

    CHECK(command.action == CommandAction::QuickFilter);
    CHECK(command.argument == "spaced query");
  }
} // namespace ao::tui::test
