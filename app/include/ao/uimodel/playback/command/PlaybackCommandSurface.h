// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <cstddef>
#include <functional>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface final
  {
  public:
    PlaybackCommandSurface(rt::PlaybackService& playback, std::function<void()> playSelection);
    ~PlaybackCommandSurface() = default;

    PlaybackCommandSurface(PlaybackCommandSurface const&) = delete;
    PlaybackCommandSurface& operator=(PlaybackCommandSurface const&) = delete;
    PlaybackCommandSurface(PlaybackCommandSurface&&) = delete;
    PlaybackCommandSurface& operator=(PlaybackCommandSurface&&) = delete;

    bool execute(PlaybackCommand command);
    bool isEnabled(PlaybackCommand command) const;
    bool isCapable(PlaybackCommand command) const;

    async::Subscription onAvailabilityChanged(std::move_only_function<void()> handler);
    async::Subscription onAvailabilityChanged(PlaybackCommand command, std::move_only_function<void()> handler);

  private:
    static constexpr std::size_t kCommandCount = 8;

    // Re-derives command availability from a newly published snapshot and emits
    // only the per-command signals whose backing state actually changed.
    void handleSnapshot(rt::PlaybackSnapshot const& snapshot);

    rt::PlaybackService& _playback;
    rt::PlaybackCommands& _commands;
    std::function<void()> _playSelection;
    rt::PlaybackSnapshot _lastSnapshot{};
    async::Signal<> _availabilityChangedSignal;
    std::array<async::Signal<>, kCommandCount> _commandAvailabilityChangedSignals;
    async::Subscription _snapshotSub;
  };
} // namespace ao::uimodel
