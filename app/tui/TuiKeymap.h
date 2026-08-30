// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <ftxui/component/event.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  /// An application action the terminal shell can execute at its root scope.
  enum class TuiKeyAction : std::uint8_t
  {
    Quit,
    ToggleListChooser,
    ToggleDetails,
    ToggleAudioPipeline,
    ToggleOutputDevices,
    TogglePresentations,
    ToggleNotifications,
    ShowHelp,
    OpenCommandPalette,
    OpenQuickFilter,
    RevealCurrentTrack,
    ClearFilter,
    Reload,
    PlaySelection,
    PreviousSection,
    NextSection,
    SeekBackward,
    SeekForward,
    VolumeDown,
    VolumeUp,
    PlaybackPlayPause,
    PlaybackStop,
    Count,
  };

  struct TuiActionDescriptor final
  {
    std::string actionId;
    TuiKeyAction action = TuiKeyAction::Quit;
    std::span<std::string_view const> defaultChords{};
  };

  /// Stable action identities and TUI-local default additions, in conflict-winner order.
  std::span<TuiActionDescriptor const> tuiActionDescriptors();

  /// Shared application defaults plus TUI-only defaults and preferred terminal aliases.
  uimodel::KeymapBindings tuiDefaultKeymap();

  /// Projects one neutral chord when the pinned terminal protocol can represent it safely.
  std::optional<ftxui::Event> tuiEventForChord(uimodel::KeyChord const& chord);

  /**
   * @brief Immutable executable projection of one effective keymap.
   *
   * Descriptor order, then chord order, decides projected collisions. The
   * first retained chord executable in the requesting scope is also the
   * shortcut shown there, so behavior and hints cannot select different
   * winners.
   */
  class TuiKeymapPlan final
  {
  public:
    explicit TuiKeymapPlan(uimodel::KeymapModel const& keymap);

    std::optional<TuiKeyAction> actionFor(ftxui::Event const& event) const;
    std::string_view shortcutFor(TuiKeyAction action,
                                 std::span<ftxui::Event const> unavailableEvents = {}) const noexcept;

  private:
    struct Entry final
    {
      ftxui::Event event;
      TuiKeyAction action = TuiKeyAction::Quit;
      std::string_view actionId;
      std::string shortcut;
    };

    std::vector<Entry> _entries{};
  };
} // namespace ao::tui
