// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "Keymap.h"
#include <ao/i18n/MessageCatalog.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tui
{
  enum class CommandAction : std::uint8_t
  {
    QuickFilter,
    OpenLists,
    TogglePinnedLists,
    OpenDetail,
    OpenQuality,
    OpenOutputDevices,
    OpenPresentationPanel,
    OpenNotifications,
    CloseOverlay,
    ShowHelp,
    OpenGoTo,
    RevealCurrentTrack,
    OpenCurrentArtist,
    OpenCurrentAlbum,
    SetPresentation,
    ClearFilter,
    Reload,
    Scan,
    ScanCancel,
    SelectToggle,
    SelectVisual,
    SelectAll,
    SelectClear,
    EditProperties,
    EditTags,
    OpenSettings,
    Play,
    TogglePlayback,
    Stop,
    Previous,
    Next,
    Shuffle,
    Repeat,
    Back,
    Forward,
    Quit,
  };

  struct Command final
  {
    CommandAction action = CommandAction::QuickFilter;
    std::string argument{};
  };

  struct CommandPrefixSpec final
  {
    std::string_view prefix;
    CommandAction action;
    i18n::MessageId detail;
    i18n::MessageId category;
    /**
     * @brief The action whose key this entry advertises, when not its own.
     *
     * A prefix can lead to a related interactive path: `/` opens Quick Filter
     * editing for `:filter`, while `p` opens the chooser for `:view <name>`.
     * Naming that action rather than its key keeps the hint from drifting: a
     * rebound key moves the hint with it, and an unbound one shows nothing.
     */
    std::optional<KeyAction> optShortcutAction{};
  };

  struct CommandAliasSpec final
  {
    std::string_view alias;
    /// Fixed mnemonic in the Go to menu, empty for commands outside that menu.
    std::string_view goToKey{};
    CommandAction action;
    i18n::MessageId detail;
    i18n::MessageId category;
  };

  std::span<CommandPrefixSpec const> commandPrefixSpecs();
  std::span<CommandAliasSpec const> commandAliasSpecs();
  /// The root shortcut worth showing beside a command alias, when one has the same semantics.
  std::optional<KeyAction> shortcutActionForCommand(CommandAction action) noexcept;
  /// The command action with the same semantics as a root key action.
  std::optional<CommandAction> commandActionForKeyAction(KeyAction action) noexcept;
  /// Effective direct shortcut, or the Go to menu path when no direct key is bound.
  std::string commandShortcut(KeymapPlan const& keymapPlan, CommandAction action);
  std::optional<Command> parseCommand(std::string_view input);
} // namespace ao::tui
