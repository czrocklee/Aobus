// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "PlaybackCommands.h"
#include "PlaybackEvents.h"
#include "PlaybackSnapshot.h"

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
   * transport and succession collaborators. During RFC 0005 stages 1 and 2 it
   * is an adapter over runtime-internal owners: it brackets commands issued
   * through it so one logical command publishes at most one snapshot, and
   * coalesces spontaneous lower-layer changes into one deferred publication
   * per executor turn. The final "one logical commit, one revision" authority
   * moves into a coordinator in a later stage.
   */
  class PlaybackService final
  {
  public:
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    /** Borrows the last committed snapshot, stable until the next publication. */
    PlaybackSnapshot const& snapshot() const;
    PlaybackCommands& commands() noexcept;
    PlaybackEvents& events() noexcept;

  private:
    friend class AppRuntime;
    friend class PlaybackBootstrap;

    class [[nodiscard]] CommandBracket final
    {
    public:
      explicit CommandBracket(PlaybackService& owner) noexcept;
      ~CommandBracket();

      CommandBracket(CommandBracket const&) = delete;
      CommandBracket& operator=(CommandBracket const&) = delete;
      CommandBracket(CommandBracket&&) = delete;
      CommandBracket& operator=(CommandBracket&&) = delete;

    private:
      PlaybackService& _owner;
    };

    struct Impl;
    explicit PlaybackService(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
