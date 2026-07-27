// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Task.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>
#include <ao/utility/ScopedRegistration.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace ao::async
{
  class LifetimeScope;
  class Runtime;
}

namespace ao::rt
{
  class AppRuntime;
  class LibraryTaskService;
}

namespace ao::winui
{
  class WindowsCoverArtLoader final
  {
  public:
    using OnReady = std::move_only_function<void(std::span<std::byte const>)>;
    using Request = utility::ScopedRegistration;

    WindowsCoverArtLoader();
    ~WindowsCoverArtLoader();

    WindowsCoverArtLoader(WindowsCoverArtLoader const&) = delete;
    WindowsCoverArtLoader& operator=(WindowsCoverArtLoader const&) = delete;
    WindowsCoverArtLoader(WindowsCoverArtLoader&&) = delete;
    WindowsCoverArtLoader& operator=(WindowsCoverArtLoader&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr);
    void unbind();
    Request request(ResourceId resourceId, OnReady onReady);

  private:
    struct Interest;
    struct Waiter;
    struct Flight;

    void spawn(ResourceId resourceId);
    void complete(ResourceId resourceId, std::vector<std::byte> bytes);
    static async::Task<void> load(WindowsCoverArtLoader* loader,
                                  std::shared_ptr<rt::AppRuntime> runtimePtr,
                                  ResourceId resourceId,
                                  std::stop_token stopToken);

    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    std::unique_ptr<async::LifetimeScope> _scopePtr;
    uimodel::CoverArtByteCache _cache;
    std::unordered_map<ResourceId, std::shared_ptr<Flight>> _flights;
  };
} // namespace ao::winui
