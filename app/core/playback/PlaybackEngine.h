// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/AudioFormat.h"
#include "core/IMainThreadDispatcher.h"
#include "core/playback/PlaybackTypes.h"
#include "core/backend/IAudioBackend.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace app::core::source
{
  class IPcmSource;
}

namespace app::core::playback
{

  class PlaybackEngine final
  {
  public:
    PlaybackEngine(std::unique_ptr<backend::IAudioBackend> backend,
                   backend::AudioDevice const& device,
                   std::shared_ptr<IMainThreadDispatcher> dispatcher = nullptr);
    ~PlaybackEngine();

    void setBackend(std::unique_ptr<backend::IAudioBackend> backend, backend::AudioDevice const& device);
    void setOnTrackEnded(std::function<void()> callback);

    void play(TrackPlaybackDescriptor descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

    // Backend callbacks
    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept;
    static bool isSourceDrained(void* userData) noexcept;
    static void onUnderrun(void* userData) noexcept;
    static void onPositionAdvanced(void* userData, std::uint32_t framesRead) noexcept;
    static void onDrainComplete(void* userData) noexcept;
    static void onGraphChanged(void* userData, backend::AudioGraph const& graph) noexcept;
    static void onBackendError(void* userData, std::string_view message) noexcept;

    // Source callbacks
    static void onSourceError(void* userData) noexcept;

  private:
    void handleBackendError(std::string_view message);
    void handleSourceError(std::string const& message);
    void handleDrainComplete();
    void handleGraphChanged(backend::AudioGraph const& graph);
    void analyzeAudioQuality();
    void resetToIdle();
    bool openTrack(TrackPlaybackDescriptor descriptor,
                   std::shared_ptr<source::IPcmSource>& source,
                   AudioFormat& backendFormat);

    std::unique_ptr<backend::IAudioBackend> _backend;
    std::shared_ptr<IMainThreadDispatcher> _dispatcher;
    backend::AudioDevice _currentDevice;
    
    std::atomic<std::shared_ptr<source::IPcmSource>> _source;
    std::atomic<bool> _backendStarted{false};
    std::atomic<bool> _playbackDrainPending{false};
    std::atomic<std::uint32_t> _underrunCount{0};

    mutable std::mutex _stateMutex;
    std::optional<TrackPlaybackDescriptor> _currentTrack;
    PlaybackSnapshot _snapshot;
    std::function<void()> _onTrackEnded;
  };

} // namespace app::core::playback
