// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#include <array>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <vector>

namespace ao::rt
{
  class PlaybackSequenceService;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface final
  {
  public:
    PlaybackCommandSurface(rt::PlaybackService& playback,
                           rt::PlaybackSequenceService& sequence,
                           std::function<void()> playSelection);
    ~PlaybackCommandSurface() = default;

    PlaybackCommandSurface(PlaybackCommandSurface const&) = delete;
    PlaybackCommandSurface& operator=(PlaybackCommandSurface const&) = delete;
    PlaybackCommandSurface(PlaybackCommandSurface&&) = delete;
    PlaybackCommandSurface& operator=(PlaybackCommandSurface&&) = delete;

    void execute(PlaybackCommand command);
    bool isEnabled(PlaybackCommand command) const;
    bool isCapable(PlaybackCommand command) const;

    rt::Subscription onAvailabilityChanged(std::move_only_function<void()> handler);
    rt::Subscription onAvailabilityChanged(PlaybackCommand command, std::move_only_function<void()> handler);

  private:
    static constexpr std::size_t kCommandCount = 8;

    void subscribeAvailabilityEvents();
    void emitAvailabilityChanged(std::initializer_list<PlaybackCommand> commands);

    rt::PlaybackService& _playback;
    rt::PlaybackSequenceService& _sequence;
    std::function<void()> _playSelection;
    rt::Signal<> _availabilityChangedSignal;
    std::array<rt::Signal<>, kCommandCount> _commandAvailabilityChangedSignals;
    std::vector<rt::Subscription> _availabilitySubs;
  };
} // namespace ao::uimodel
