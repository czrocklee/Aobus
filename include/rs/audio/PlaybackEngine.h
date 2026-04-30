// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/audio/AudioFormat.h>
#include <rs/audio/IAudioBackend.h>
#include <rs/audio/PlaybackTypes.h>
#include <rs/utility/IMainThreadDispatcher.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace rs::audio
{
  class IPcmSource;
}

namespace rs::audio
{

  class PlaybackEngine final
  {
  public:
    PlaybackEngine(std::unique_ptr<rs::audio::IAudioBackend> backend,
                   rs::audio::AudioDevice const& device,
                   std::shared_ptr<rs::IMainThreadDispatcher> dispatcher = nullptr);
    ~PlaybackEngine();

    void setBackend(std::unique_ptr<rs::audio::IAudioBackend> backend, rs::audio::AudioDevice const& device);
    void setOnTrackEnded(std::function<void()> callback);

    using OnRouteChanged = std::function<void(EngineRouteSnapshot const&)>;
    void setOnRouteChanged(OnRouteChanged callback);
    EngineRouteSnapshot routeSnapshot() const;

    void play(TrackPlaybackDescriptor const& descriptor);
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

  private:
    void handleBackendError(std::string_view message);
    void handleSourceError(rs::Error const& error);
    void handleFormatChanged(AudioFormat const& format);
    void handleDrainComplete();
    void handleRouteReady(std::string_view routeAnchor);
    void resetToIdle();
    bool openTrack(TrackPlaybackDescriptor const& descriptor,
                   std::shared_ptr<rs::audio::IPcmSource>& source,
                   AudioFormat& backendFormat);

    std::unique_ptr<rs::audio::IAudioBackend> _backend;
    std::shared_ptr<rs::IMainThreadDispatcher> _dispatcher;
    rs::audio::AudioDevice _currentDevice;

    std::atomic<std::shared_ptr<rs::audio::IPcmSource>> _source;
    std::atomic<bool> _backendStarted{false};
    std::atomic<bool> _playbackDrainPending{false};
    std::atomic<std::uint32_t> _underrunCount{0};

    mutable std::mutex _stateMutex;
    std::optional<TrackPlaybackDescriptor> _currentTrack;
    PlaybackSnapshot _snapshot;
    std::function<void()> _onTrackEnded;
    OnRouteChanged _onRouteChanged;
    EngineRouteSnapshot _routeSnapshot;
  };

} // namespace rs::audio
