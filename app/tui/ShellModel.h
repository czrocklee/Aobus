// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
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
    Help,
  };

  enum class CommandAction : std::uint8_t
  {
    QuickFilter,
    OpenLists,
    OpenDetail,
    OpenQuality,
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

  Command parseCommand(std::string_view input);
  std::string overlayLabel(Overlay overlay);

  class ShellModel final
  {
  public:
    bool commandActive() const noexcept;
    std::string const& commandDraft() const noexcept;
    Overlay overlay() const noexcept;

    void beginCommand(std::string draft = {});
    void appendCommandText(std::string_view text);
    void backspaceCommand();
    void cancelCommand();
    Command submitCommand();

    void openOverlay(Overlay overlay) noexcept;
    void closeOverlay() noexcept;

  private:
    bool _commandActive = false;
    std::string _commandDraft{};
    Overlay _overlay = Overlay::None;
  };
} // namespace ao::tui
