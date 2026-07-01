// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandCompletionState.h"
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
    Help,
  };

  enum class CommandAction : std::uint8_t
  {
    QuickFilter,
    OpenLists,
    OpenDetail,
    OpenQuality,
    OpenOutputDevices,
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
    std::string_view detail;
  };

  struct CommandAliasSpec final
  {
    std::string_view alias;
    CommandAction action;
    std::string_view detail;
  };

  std::span<CommandPrefixSpec const> commandPrefixSpecs();
  std::span<CommandAliasSpec const> commandAliasSpecs();
  Command parseCommand(std::string_view input);
  std::string overlayLabel(Overlay overlay);

  class ShellModel final
  {
  public:
    bool commandActive() const noexcept;
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
