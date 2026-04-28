// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/BackendTypes.h"
#include "core/playback/PlaybackTypes.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace app::core
{
  class IMainThreadDispatcher;
}

namespace app::core::backend
{
  class IBackendManager;
}

namespace app::core::playback
{
  class PlaybackEngine;

  class PlaybackController final
  {
  public:
    PlaybackController(std::shared_ptr<IMainThreadDispatcher> dispatcher);
    ~PlaybackController();

    void setTrackEndedCallback(std::function<void()> callback);

    void addManager(std::unique_ptr<backend::IBackendManager> manager);

    void play(TrackPlaybackDescriptor descriptor);
    void setOutput(backend::BackendKind kind, std::string_view deviceId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);

    PlaybackSnapshot snapshot() const;

  private:
    std::unique_ptr<PlaybackEngine> _engine;

    // Backend managers
    std::vector<std::unique_ptr<backend::IBackendManager>> _managers;

    std::shared_ptr<IMainThreadDispatcher> _dispatcher;
    std::function<void()> _onTrackEnded;

    mutable std::atomic<bool> _backendsDirty{true};
    mutable std::vector<BackendSnapshot> _cachedBackends;
    mutable std::vector<backend::AudioDevice> _allDevices;
  };

} // namespace app::core::playback