// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "ProjectionTypes.h"
#include "StateTypes.h"
#include <ao/audio/IBackendProvider.h>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace ao::app
{
  class CommandBus;
  class EventBus;
  class IControlExecutor;
  class ViewRegistry;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::app
{
  class PlaybackService final
  {
  public:
    PlaybackService(CommandBus& commands,
                    EventBus& events,
                    IControlExecutor& executor,
                    ViewRegistry& views,
                    ao::library::MusicLibrary& library);
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    IReadOnlyStore<PlaybackState>& state();

    void addProvider(std::unique_ptr<ao::audio::IBackendProvider> provider);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  class LibraryMutationService final
  {
  public:
    LibraryMutationService(CommandBus& commands,
                           EventBus& events,
                           IControlExecutor& executor,
                           ao::library::MusicLibrary& library);
    ~LibraryMutationService();

    LibraryMutationService(LibraryMutationService const&) = delete;
    LibraryMutationService& operator=(LibraryMutationService const&) = delete;
    LibraryMutationService(LibraryMutationService&&) = delete;
    LibraryMutationService& operator=(LibraryMutationService&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  class LibraryQueryService final
  {
  public:
    LibraryQueryService(ViewRegistry& views, EventBus& events, ao::library::MusicLibrary& library);
    ~LibraryQueryService();

    LibraryQueryService(LibraryQueryService const&) = delete;
    LibraryQueryService& operator=(LibraryQueryService const&) = delete;
    LibraryQueryService(LibraryQueryService&&) = delete;
    LibraryQueryService& operator=(LibraryQueryService&&) = delete;

    std::shared_ptr<ITrackListProjection> trackListProjection(ViewId viewId);
    std::shared_ptr<ITrackDetailProjection> detailProjection(DetailTarget const& target);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

  class NotificationService final
  {
  public:
    NotificationService(CommandBus& commands, EventBus& events);
    ~NotificationService();

    NotificationService(NotificationService const&) = delete;
    NotificationService& operator=(NotificationService const&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    IReadOnlyStore<NotificationFeedState>& feed();

    NotificationId post(NotificationSeverity severity,
                        std::string message,
                        bool sticky = false,
                        std::optional<std::chrono::milliseconds> optTimeout = std::nullopt);

    void dismiss(NotificationId id);
    void dismissAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
