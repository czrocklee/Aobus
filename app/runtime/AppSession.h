// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "ListSourceStore.h"
#include <ao/Type.h>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class IBackendProvider;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::app
{
  class EventBus;
  class IControlExecutor;
  class ConfigStore;
  class ConfigStore;
  class PlaybackService;
  class LibraryMutationService;
  class WorkspaceService;
  class NotificationService;
  class ViewService;

  struct AppSessionDependencies final
  {
    std::shared_ptr<IControlExecutor> executor{};
    std::filesystem::path libraryRoot{};
    std::shared_ptr<ConfigStore> configStore{};
  };

  class AppSession final
  {
  public:
    explicit AppSession(AppSessionDependencies dependencies);
    ~AppSession();

    AppSession(AppSession const&) = delete;
    AppSession& operator=(AppSession const&) = delete;
    AppSession(AppSession&&) = delete;
    AppSession& operator=(AppSession&&) = delete;

    IControlExecutor& executor() noexcept;
    EventBus& events() noexcept;

    PlaybackService& playback() noexcept;
    WorkspaceService& workspace() noexcept;
    LibraryMutationService& mutation() noexcept;
    NotificationService& notifications() noexcept;
    ViewService& views() noexcept;

    ao::library::MusicLibrary& musicLibrary() noexcept;
    ListSourceStore& sources() noexcept;
    void reloadAllTracks();

    ao::TrackId playSelectionInFocusedView();

    void addAudioProvider(std::unique_ptr<ao::audio::IBackendProvider> provider);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
