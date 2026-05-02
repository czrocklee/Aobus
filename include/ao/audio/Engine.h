// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/DecoderTypes.h>
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
}

namespace ao::audio
{
  class Engine final
  {
  public:
    using OnRouteChanged = std::function<void(EngineRouteSnapshot const&)>;

    Engine(std::unique_ptr<IBackend> backend,
                   Device const& device,
                   std::shared_ptr<ao::IMainThreadDispatcher> dispatcher = nullptr);
    ~Engine();

    void setBackend(std::unique_ptr<IBackend> backend, Device const& device);
    void setOnTrackEnded(std::function<void()> callback);

    void setOnRouteChanged(OnRouteChanged callback);
    EngineRouteSnapshot routeSnapshot() const;

    void play(TrackPlaybackDescriptor const& descriptor);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    Snapshot snapshot() const;

    // Backend callbacks
    static std::size_t onReadPcm(void* userData, std::span<std::byte> output) noexcept;
    static bool isSourceDrained(void* userData) noexcept;
    static void onUnderrun(void* userData) noexcept;
    static void onPositionAdvanced(void* userData, std::uint32_t framesRead) noexcept;
    static void onDrainComplete(void* userData) noexcept;
    static void onRouteReady(void* userData, std::string_view routeAnchor) noexcept;
    static void onFormatChanged(void* userData, Format const& format) noexcept;
    static void onBackendError(void* userData, std::string_view message) noexcept;

  private:
    void handleBackendError(std::string_view message);
    void handleSourceError(ao::Error const& error);
    void handleFormatChanged(Format const& format);
    void handleDrainComplete();
    void handleRouteReady(std::string_view routeAnchor);
    void resetToIdle();
    bool openTrack(TrackPlaybackDescriptor const& descriptor,
                   std::shared_ptr<ISource>& source,
                   Format& backendFormat);

    bool negotiateFormat(std::filesystem::path const& path,
                         DecodedStreamInfo const& info,
                         std::unique_ptr<IDecoderSession>& decoder,
                         Format& backendFormat);

    std::shared_ptr<ISource> createPcmSource(std::unique_ptr<IDecoderSession> decoder,
                                                DecodedStreamInfo const& info);

    std::unique_ptr<IBackend> _backend;
    std::shared_ptr<ao::IMainThreadDispatcher> _dispatcher;
    Device _currentDevice;

    std::atomic<std::shared_ptr<ISource>> _source;
    std::atomic<bool> _backendStarted{false};
    std::atomic<bool> _playbackDrainPending{false};
    std::atomic<std::uint32_t> _underrunCount{0};

    mutable std::mutex _stateMutex;
    std::optional<TrackPlaybackDescriptor> _currentTrack;
    Snapshot _snapshot;
    std::function<void()> _onTrackEnded;
    OnRouteChanged _onRouteChanged;
    EngineRouteSnapshot _routeSnapshot;
  };
} // namespace ao::audio
