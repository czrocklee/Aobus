// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace ao::rt
{
  class PlaybackService;
  class ViewService;
  class WorkspaceService;
}

namespace ao::uimodel
{
  enum class PlaybackActionOrigin : std::uint8_t
  {
    Primary,
    SystemMedia,
  };

  struct SystemMediaPlayHandler final
  {
    std::function<bool()> canPlay{};
    std::function<bool()> play{};
  };

  class PlaybackActions final
  {
  public:
    // With no subject to resume, both origins use playSelection only when a selection is available.
    // A host may instead supply systemMediaPlay to evaluate and start its own remote target.
    PlaybackActions(rt::PlaybackService& playback,
                    std::function<void()> playSelection,
                    SystemMediaPlayHandler systemMediaPlay = {});
    ~PlaybackActions();

    PlaybackActions(PlaybackActions const&) = delete;
    PlaybackActions& operator=(PlaybackActions const&) = delete;
    PlaybackActions(PlaybackActions&&) = delete;
    PlaybackActions& operator=(PlaybackActions&&) = delete;

    // Desktop hosts follow selected rows in the focused projection; a host with its own
    // selection cursor can publish its availability directly instead.
    void bindFocusedSelection(rt::WorkspaceService& workspace, rt::ViewService& views);
    void setSelectionAvailable(bool available);
    bool tryExecute(PlaybackCommand command, PlaybackActionOrigin origin = PlaybackActionOrigin::Primary);
    bool isEnabled(PlaybackCommand command, PlaybackActionOrigin origin = PlaybackActionOrigin::Primary) const;
    bool isCapable(PlaybackCommand command) const;

    async::Subscription onAvailabilityChanged(compat::MoveOnlyFunction<void()> handler);
    async::Subscription onAvailabilityChanged(PlaybackCommand command, compat::MoveOnlyFunction<void()> handler);

  private:
    static constexpr std::size_t kCommandCount = 8;

    bool hasSystemMediaSelection() const;
    void setSelectionAvailability(bool primaryAvailable, bool systemMediaAvailable);

    // Re-derives command availability from a newly published snapshot and emits
    // only the per-command signals whose backing state actually changed.
    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _commands;
    std::function<void()> _playSelection;
    SystemMediaPlayHandler _systemMediaPlay;
    bool _selectionAvailable = false;
    bool _systemMediaSelectionAvailable = false;
    rt::PlaybackSnapshot _lastSnapshot{};
    async::Signal<> _availabilityChangedSignal;
    std::array<async::Signal<>, kCommandCount> _commandAvailabilityChangedSignals;
    async::Subscription _snapshotSub;
    struct FocusedSelection;
    std::unique_ptr<FocusedSelection> _focusedSelectionPtr;
  };
} // namespace ao::uimodel
