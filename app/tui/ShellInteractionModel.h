// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
#include "TuiTextCatalog.h"
#include <ao/rt/completion/CompletionResult.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::tui
{
  enum class Overlay : std::uint8_t
  {
    None,
    ListChooser,
    DetailPanel,
    QualityPanel,
    OutputDevices,
    PresentationPanel,
    Notifications,
    Help,
  };

  enum class CommandAction : std::uint8_t
  {
    QuickFilter,
    OpenLists,
    OpenDetail,
    OpenQuality,
    OpenOutputDevices,
    OpenPresentationPanel,
    OpenNotifications,
    CloseOverlay,
    ShowHelp,
    RevealCurrentTrack,
    SetPresentation,
    ClearFilter,
    Reload,
    Play,
    TogglePlayback,
    Stop,
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
    TuiTextId detail;
    TuiTextId category;
    /**
     * @brief The action whose key this entry advertises, when not its own.
     *
     * `/view <name>` selects a track view outright while `v` opens the panel to
     * pick one, so they are different actions that reach the same place. Naming
     * the action rather than the key is what keeps the hint from drifting: a
     * rebound key moves the hint with it, and an unbound one shows nothing.
     */
    std::optional<CommandAction> optShortcutAction{};
  };

  struct CommandAliasSpec final
  {
    std::string_view alias;
    CommandAction action;
    TuiTextId detail;
    TuiTextId category;
  };

  /**
   * @brief A key that runs a command without the command line, and what it runs.
   *
   * The key is written the way a reader sees it rather than as a terminal
   * event, so this stays the one declaration behind both what the shell listens
   * for and what it tells the user. An action may appear more than once when
   * more than one key runs it.
   */
  struct KeyBindingSpec final
  {
    std::string_view key;
    CommandAction action;
  };

  std::span<CommandPrefixSpec const> commandPrefixSpecs();
  std::span<CommandAliasSpec const> commandAliasSpecs();
  std::span<KeyBindingSpec const> keyBindingSpecs();

  /// The first key that runs @p action, or empty when no key does.
  std::string_view shortcutFor(CommandAction action);
  Command parseCommand(std::string_view input);
  std::string_view overlayLabel(TuiTextCatalog const& textCatalog, Overlay overlay);
  std::string_view overlayHint(TuiTextCatalog const& textCatalog, Overlay overlay);

  class ShellInteractionModel final
  {
  public:
    bool isCommandActive() const noexcept;
    std::string const& commandDraft() const noexcept;
    std::optional<rt::CompletionResult> const& commandCompletion() const noexcept;
    std::int32_t commandCompletionSelection() const noexcept;
    Overlay overlay() const noexcept;

    void beginCommand(std::string draft = {});
    void appendCommandText(std::string_view text);
    void backspaceCommand();
    void cancelCommand();
    Command submitCommand();
    void setCommandCompletion(std::optional<rt::CompletionResult> optCompletion);
    bool moveCommandCompletion(std::int32_t delta);
    bool applyCommandCompletion();
    void clearCommandCompletion();

    void openOverlay(Overlay overlay) noexcept;
    void closeOverlay() noexcept;

  private:
    bool _commandActive = false;
    std::string _commandDraft{};
    CommandCompletionState _completion{};
    Overlay _overlay = Overlay::None;
  };
} // namespace ao::tui
