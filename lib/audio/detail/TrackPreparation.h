// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Engine.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace ao::audio::detail
{
  /** Isolated worker value captured from Engine control state. */
  class TrackPreparation final
  {
  public:
    enum class Purpose : std::uint8_t
    {
      ExplicitStart,
      GaplessLookahead,
    };

    ~TrackPreparation();
    TrackPreparation(TrackPreparation const&) = delete;
    TrackPreparation& operator=(TrackPreparation const&) = delete;
    TrackPreparation(TrackPreparation&&) noexcept;
    TrackPreparation& operator=(TrackPreparation&&) noexcept;

    static Result<TrackPreparation> capture(Engine& engine,
                                            Engine::PlaybackItem const& item,
                                            std::chrono::milliseconds initialOffset,
                                            Purpose purpose);

    Result<> prepare();
    bool requiresWorker() const noexcept;
    Result<Engine::PreparedPlaybackStart> adoptStart(Engine& engine) &&;
    Result<Engine::PreparedNextResult> adoptNext(Engine& engine) &&;

  private:
    struct Impl;
    explicit TrackPreparation(std::unique_ptr<Impl> implPtr);

    static Result<TrackPreparation> captureUnlocked(Engine& engine,
                                                    Engine::PlaybackItem const& item,
                                                    std::chrono::milliseconds initialOffset,
                                                    Purpose purpose);
    Result<Engine::PreparedPlaybackStart> adoptStartUnlocked(Engine& engine) &&;
    Result<Engine::PreparedNextResult> adoptNextUnlocked(Engine& engine) &&;
    bool matchesControlContext(Engine const& engine, std::uint64_t currentGeneration) const;

    std::unique_ptr<Impl> _implPtr;

    friend class ::ao::audio::Engine;
  };
} // namespace ao::audio::detail
