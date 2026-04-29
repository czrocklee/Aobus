// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/AudioFormat.h"
#include "core/IMainThreadDispatcher.h"
#include "core/backend/IAudioBackend.h"
#include "core/playback/PlaybackTypes.h"

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

    using OnRouteChanged = std::function<void(EngineRouteSnapshot const&)>;
    void setOnRouteChanged(OnRouteChanged callback);
    EngineRouteSnapshot routeSnapshot() const;

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
    static void onRouteReady(void* userData, std::string_view routeAnchor) noexcept;
    static void onFormatChanged(void* userData, AudioFormat const& format) noexcept;
    static void onBackendError(void* userData, std::string_view message) noexcept;

    // Source callbacks
    static void onSourceError(void* userData) noexcept;

  private:
    void handleBackendError(std::string_view message);
    void handleSourceError(std::string const& message);
    void handleFormatChanged(AudioFormat const& format);
    void handleDrainComplete();
    void handleRouteReady(std::string_view routeAnchor);
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
    std::string _lastError;
    PlaybackSnapshot _snapshot;
    std::function<void()> _onTrackEnded;
    OnRouteChanged _onRouteChanged;
    EngineRouteSnapshot _routeSnapshot;
  };

} // namespace app::core::playback
