// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
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
  enum class KeyAction : std::uint8_t
  {
    Quit,
    ToggleLists,
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
    Scan,
    ScanCancel,
    SelectToggle,
    SelectVisual,
    SelectAll,
    SelectClear,
    EditProperties,
    OpenSettings,
    PlaySelection,
    PreviousRow,
    NextRow,
    PreviousSection,
    NextSection,
    SeekBackward,
    SeekForward,
    VolumeDown,
    VolumeUp,
    PlaybackPlayPause,
    PlaybackStop,
    PlaybackPrevious,
    PlaybackNext,
    PlaybackShuffle,
    PlaybackRepeat,
    SwitchWorkspaceFocus,
    Count,
  };

  bool isPlaybackControl(KeyAction action);

  struct ActionDescriptor final
  {
    std::string actionId;
    KeyAction action = KeyAction::Quit;
    std::span<std::string_view const> defaultChords{};
  };

  /// Stable action identities and terminal defaults, in conflict-winner order.
  std::span<ActionDescriptor const> actionDescriptors();

  /// Deliberate terminal defaults using shared action identities where applicable.
  uimodel::KeymapBindings defaultKeymap();

  /// Validates only the edited action against executable terminal ownership.
  Result<> validateActionBindings(uimodel::KeymapModel const& candidate, std::string_view actionId);

  /// Projects one neutral chord when the pinned terminal protocol can represent it safely.
  std::optional<ftxui::Event> eventForChord(uimodel::KeyChord const& chord);

  /// Human-readable terminal spelling; bare letters show their actual case.
  std::string keyChordLabel(uimodel::KeyChord const& chord);

  /**
   * @brief Immutable executable projection of one effective keymap.
   *
   * Descriptor order, then chord order, decides projected collisions. The
   * first retained chord executable in the requesting scope is also the
   * shortcut shown there, so behavior and hints cannot select different
   * winners.
   */
  class KeymapPlan final
  {
  public:
    explicit KeymapPlan(uimodel::KeymapModel const& keymap);

    std::optional<KeyAction> actionFor(ftxui::Event const& event) const;
    std::string_view shortcutFor(KeyAction action, std::span<ftxui::Event const> unavailableEvents = {}) const noexcept;

  private:
    struct Entry final
    {
      ftxui::Event event;
      KeyAction action = KeyAction::Quit;
      std::string_view actionId;
      std::string shortcut;
    };

    std::vector<Entry> _entries{};
  };
} // namespace ao::tui
