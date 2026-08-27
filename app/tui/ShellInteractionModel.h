// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
#include <ao/i18n/MessageCatalog.h>
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

  enum class ShellInputMode : std::uint8_t
  {
    None,
    QuickFilter,
    Command,
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
    i18n::MessageId detail;
    i18n::MessageId category;
    /**
     * @brief The action whose key this entry advertises, when not its own.
     *
     * `:view <name>` selects a track view outright while `v` opens the panel to
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
    i18n::MessageId detail;
    i18n::MessageId category;
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
  /**
   * @brief Whether @p overlay blocks interaction with the workspace beneath it.
   *
   * Separate from whether an overlay is on screen: Detail is a live inspector
   * that follows the track table while the user keeps browsing it, so it is
   * visible without being modal. Ask this when the question is "may the
   * workspace still be driven"; ask @ref ShellInteractionModel::overlay when
   * the question is "is another surface open".
   */
  bool isModalOverlay(Overlay overlay) noexcept;
  std::optional<Command> parseCommand(std::string_view input);
  std::string_view overlayLabel(i18n::MessageCatalog const& textCatalog, Overlay overlay);
  std::string overlayHint(i18n::MessageCatalog const& textCatalog, Overlay overlay);

  class ShellInteractionModel final
  {
  public:
    bool isInputActive() const noexcept;
    ShellInputMode inputMode() const noexcept;
    std::string const& inputDraft() const noexcept;
    bool isInputTouched() const noexcept;
    std::optional<rt::CompletionResult> const& commandCompletion() const noexcept;
    std::int32_t commandCompletionSelection() const noexcept;
    Overlay overlay() const noexcept;

    void beginInput(ShellInputMode mode, std::string draft = {});
    void appendInputText(std::string_view text);
    void backspaceInput();
    void closeInput();
    void setCommandCompletion(std::optional<rt::CompletionResult> optCompletion);
    bool moveCommandCompletion(std::int32_t delta);
    bool moveCommandCompletionByPage(std::int32_t delta);
    bool applyCommandCompletion();
    void clearCommandCompletion();

    void openOverlay(Overlay overlay) noexcept;
    void closeOverlay() noexcept;

  private:
    ShellInputMode _inputMode = ShellInputMode::None;
    std::string _inputDraft{};
    bool _inputTouched = false;
    CommandCompletionState _completion{};
    Overlay _overlay = Overlay::None;
  };
} // namespace ao::tui
