// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/Types.h>
#include <ao/utility/IMainThreadDispatcher.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace ao::audio
{
  class ISource;
  using DecoderFactoryFn = std::function<std::unique_ptr<IDecoderSession>(std::filesystem::path const&, Format)>;
}

namespace ao::audio
{
  class Engine final
  {
  public:
    struct Status final
    {
      Transport transport = Transport::Idle;
      BackendId backendId;
      ProfileId profileId;
      std::uint32_t positionMs = 0;
      std::uint32_t durationMs = 0;
      std::uint32_t bufferedMs = 0;
      std::uint32_t underrunCount = 0;
      std::string statusText;
      DeviceId currentDeviceId;
      flow::Graph flow;

      float volume = 1.0f;
      bool muted = false;
      bool volumeAvailable = false;

      bool operator==(Status const&) const = default;
    };

    struct RouteStatus final
    {
      flow::Graph flow;
      std::optional<RouteAnchor> optAnchor;

      bool operator==(RouteStatus const&) const = default;
    };

    using OnRouteChanged = std::function<void(RouteStatus const&)>;

    Engine(std::unique_ptr<IBackend> backend,
           Device const& device,
           std::shared_ptr<ao::IMainThreadDispatcher> dispatcher = nullptr,
           DecoderFactoryFn decoderFactory = nullptr);
    ~Engine() noexcept;

    void setBackend(std::unique_ptr<IBackend> backend, Device const& device);
    void updateDevice(Device const& device);
    void setOnTrackEnded(std::function<void()> callback);

    void setOnRouteChanged(OnRouteChanged callback);
    RouteStatus routeStatus() const;

    void play(TrackPlaybackDescriptor const& descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    void setVolume(float volume);
    float getVolume() const;
    void setMuted(bool muted);
    bool isMuted() const;
    bool isVolumeAvailable() const;

    Status status() const;

    // Backend callbacks
    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept;
    static bool isSourceDrained(void* userData) noexcept;
    static void onUnderrun(void* userData) noexcept;
    static void onPositionAdvanced(void* userData, std::uint32_t framesRead) noexcept;
    static void onDrainComplete(void* userData) noexcept;
    static void onRouteReady(void* userData, std::string_view routeAnchor) noexcept;
    static void onFormatChanged(void* userData, Format const& format) noexcept;
    static void onPropertyChanged(void* userData, PropertyId id) noexcept;
    static void onBackendError(void* userData, std::string_view message) noexcept;

  private:
    void handleBackendError(std::string_view message);
    void handleSourceError(ao::Error const& error);
    void handleFormatChanged(Format const& format);
    void handlePropertyChanged(PropertyId id);
    void handleDrainComplete();
    void handleRouteReady(std::string_view routeAnchor);
    void resetToIdle();
    bool openTrack(TrackPlaybackDescriptor const& descriptor, std::shared_ptr<ISource>& source, Format& backendFormat);

    bool negotiateFormat(std::filesystem::path const& path,
                         DecodedStreamInfo const& info,
                         std::unique_ptr<IDecoderSession>& decoder,
                         Format& backendFormat);

    std::shared_ptr<ISource> createPcmSource(std::unique_ptr<IDecoderSession> decoder, DecodedStreamInfo const& info);

    std::unique_ptr<IBackend> _backend;
    std::shared_ptr<ao::IMainThreadDispatcher> _dispatcher;
    Device _currentDevice;

    std::atomic<std::shared_ptr<ISource>> _source;
    std::atomic<bool> _backendStarted{false};
    std::atomic<bool> _playbackDrainPending{false};
    std::atomic<std::uint32_t> _underrunCount{0};
    std::atomic<std::uint64_t> _accumulatedFrames{0};
    std::atomic<std::uint32_t> _engineSampleRate{0};

    mutable std::mutex _stateMutex;
    std::optional<TrackPlaybackDescriptor> _currentTrack;
    Status _status;
    std::function<void()> _onTrackEnded;
    OnRouteChanged _onRouteChanged;
    RouteStatus _routeStatus;
    DecoderFactoryFn _decoderFactory;
  };
} // namespace ao::audio
