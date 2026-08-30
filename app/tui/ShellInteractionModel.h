// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
#include "TuiKeymap.h"
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
     * A prefix can lead to a related interactive path: `/` opens Quick Filter
     * editing for `:filter`, while `v` opens the chooser for `:view <name>`.
     * Naming that action rather than its key keeps the hint from drifting: a
     * rebound key moves the hint with it, and an unbound one shows nothing.
     */
    std::optional<TuiKeyAction> optShortcutAction{};
  };

  struct CommandAliasSpec final
  {
    std::string_view alias;
    CommandAction action;
    i18n::MessageId detail;
    i18n::MessageId category;
  };

  std::span<CommandPrefixSpec const> commandPrefixSpecs();
  std::span<CommandAliasSpec const> commandAliasSpecs();
  /// The root shortcut worth showing beside a command alias, when one has the same semantics.
  std::optional<TuiKeyAction> shortcutActionForCommand(CommandAction action) noexcept;
  /// The command action with the same semantics as a root key action.
  std::optional<CommandAction> commandActionForKeyAction(TuiKeyAction action) noexcept;
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
  /// The first binding that reaches an overlay's toggle after its fixed local protocol handles input.
  std::string_view overlayToggleShortcut(TuiKeymapPlan const& keymapPlan, Overlay overlay);
  std::string overlayHint(i18n::MessageCatalog const& textCatalog, TuiKeymapPlan const& keymapPlan, Overlay overlay);

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
