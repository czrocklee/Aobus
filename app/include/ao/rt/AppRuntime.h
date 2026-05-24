// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "CoreRuntime.h"
#include "ao/Type.h"

#include <filesystem>
#include <memory>

namespace ao::audio
{
  class IBackendProvider;
}

namespace ao::rt
{
  namespace async
  {
    class Runtime;
  }

  class ConfigStore;
  class PlaybackService;
  class WorkspaceService;
  class ViewService;

  struct AppRuntimeDependencies
  {
    std::unique_ptr<IControlExecutor> executor{};
    std::filesystem::path musicRoot{};
    std::filesystem::path databasePath{};
    std::shared_ptr<ConfigStore> workspaceConfigStore{};
  };

  class AppRuntime final : public CoreRuntime
  {
  public:
    explicit AppRuntime(AppRuntimeDependencies dependencies);
    ~AppRuntime() override;

    AppRuntime(AppRuntime const&) = delete;
    AppRuntime& operator=(AppRuntime const&) = delete;
    AppRuntime(AppRuntime&&) = delete;
    AppRuntime& operator=(AppRuntime&&) = delete;

    PlaybackService& playback() noexcept;
    WorkspaceService& workspace() noexcept;
    ViewService& views() noexcept;
    ConfigStore& configStore() noexcept;
    async::Runtime& async() noexcept;

    void reloadAllTracks();

    TrackId playSelectionInFocusedView();

    void addAudioProvider(std::unique_ptr<audio::IBackendProvider> provider);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
