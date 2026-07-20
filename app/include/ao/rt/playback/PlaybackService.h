// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PlaybackCommands.h"
#include "PlaybackEvents.h"
#include "PlaybackSnapshot.h"

#include <functional>
#include <memory>

namespace ao::rt
{
  class AppRuntime;
  class PlaybackBootstrap;

  /**
   * The single public playback composition boundary.
   *
   * It exposes the current coherent snapshot directly, owns access to the
   * narrow `PlaybackCommands` and `PlaybackEvents` roles, and hides the
   * transport and succession collaborators. Its private implementation is the
   * playback commit authority: one logical mutation produces at most one
   * coherent revision, and observer-issued commands enter a later executor
   * turn. Every method and returned role is callback-executor-affine.
   */
  class PlaybackService final
  {
  public:
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    /**
     * Borrows the last committed snapshot, stable until the next publication
     * or service destruction. Do not retain it across a command or callback
     * that may publish a newer snapshot.
     */
    PlaybackSnapshot const& snapshot() const;
    PlaybackCommands& commands() noexcept;
    PlaybackEvents& events() noexcept;

  private:
    friend class AppRuntime;
    friend class PlaybackBootstrap;

    struct Impl;
    explicit PlaybackService(std::unique_ptr<Impl> implPtr);

    // Reports only whether immediate admission was available. Invariant faults
    // from an admitted operation remain exceptions after commit bookkeeping.
    bool runSynchronousIntent(std::move_only_function<bool()> operation);
    void shutdown() noexcept;

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
