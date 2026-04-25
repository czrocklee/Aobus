// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/IAudioBackend.h"
#include "core/playback/IPcmSource.h"
#include "core/playback/PlaybackTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace app::core::playback
{

  class PlaybackEngine final
  {
  public:
    explicit PlaybackEngine(std::unique_ptr<IAudioBackend> backend);
    ~PlaybackEngine();

    void play(TrackPlaybackDescriptor descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    bool openTrack(TrackPlaybackDescriptor descriptor,
                   std::shared_ptr<IPcmSource>& source,
                   StreamFormat& backendFormat);

    void handleGraphChanged(AudioGraph const& backendGraph);
    void analyzeAudioQuality();

    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept;
    static bool isSourceDrained(void* userData) noexcept;
    static void onUnderrun(void* userData) noexcept;
    static void onPositionAdvanced(void* userData, std::uint32_t frames) noexcept;
    static void onDrainComplete(void* userData) noexcept;
    static void onGraphChanged(void* userData, AudioGraph const& graph) noexcept;
    static void onSourceError(void* userData) noexcept;

    std::unique_ptr<IAudioBackend> _backend;
    std::atomic<std::shared_ptr<IPcmSource>> _source;

    mutable std::mutex _stateMutex;
    PlaybackSnapshot _snapshot;
    std::optional<TrackPlaybackDescriptor> _currentTrack;

    std::atomic<TransportState> _state = TransportState::Idle;
    std::atomic<std::uint32_t> _underrunCount = 0;
    std::atomic<bool> _backendStarted = false;
    std::atomic<bool> _playbackDrainPending = false;
  };

} // namespace app::core::playback
