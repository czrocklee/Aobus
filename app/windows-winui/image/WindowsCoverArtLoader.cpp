// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "image/WindowsCoverArtLoader.h"

#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::winui
{
  struct WindowsCoverArtLoader::Interest final
  {
    std::atomic_bool active{true};
  };

  struct WindowsCoverArtLoader::Waiter final
  {
    std::shared_ptr<Interest> interestPtr;
    OnReady callback;
  };

  struct WindowsCoverArtLoader::Flight final
  {
    std::vector<Waiter> waiters;
  };

  WindowsCoverArtLoader::WindowsCoverArtLoader()
    : _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  WindowsCoverArtLoader::~WindowsCoverArtLoader()
  {
    unbind();
  }

  void WindowsCoverArtLoader::bind(std::shared_ptr<rt::AppRuntime> runtimePtr)
  {
    unbind();
    _scopePtr = std::make_unique<async::LifetimeScope>();
    _runtimePtr = std::move(runtimePtr);
  }

  void WindowsCoverArtLoader::unbind()
  {
    if (_scopePtr)
    {
      _scopePtr->cancelAll();
    }
    _flights.clear();
    _cache.reset();
    _runtimePtr.reset();
  }

  WindowsCoverArtLoader::Request WindowsCoverArtLoader::request(ResourceId const resourceId, OnReady onReady)
  {
    if (!_runtimePtr || resourceId == kInvalidResourceId || !onReady)
    {
      return {};
    }

    if (auto const cached = _cache.cached(resourceId); !cached.empty())
    {
      onReady(cached);
      return {};
    }

    auto interestPtr = std::make_shared<Interest>();
    if (auto const found = _flights.find(resourceId); found != _flights.end())
    {
      found->second->waiters.push_back({.interestPtr = interestPtr, .callback = std::move(onReady)});
    }
    else
    {
      auto flightPtr = std::make_shared<Flight>();
      flightPtr->waiters.push_back({.interestPtr = interestPtr, .callback = std::move(onReady)});
      _flights.emplace(resourceId, flightPtr);
      spawn(resourceId);
    }

    return Request{[interestPtr] { interestPtr->active.store(false, std::memory_order_relaxed); }};
  }

  void WindowsCoverArtLoader::spawn(ResourceId const resourceId)
  {
    auto runtimePtr = _runtimePtr;
    runtimePtr->async().spawnWithLifetime(
      _scopePtr.get(),
      [loader = this, runtimePtr = std::move(runtimePtr), resourceId](std::stop_token const stopToken)
      { return load(loader, runtimePtr, resourceId, stopToken); });
  }

  void WindowsCoverArtLoader::complete(ResourceId const resourceId, std::vector<std::byte> bytes)
  {
    auto const found = _flights.find(resourceId);
    if (found == _flights.end())
    {
      return;
    }

    auto flightPtr = std::move(found->second);
    _flights.erase(found);
    if (!bytes.empty())
    {
      _cache.store(resourceId, std::move(bytes));
    }
    auto const cached = _cache.cached(resourceId);
    for (auto& waiter : flightPtr->waiters)
    {
      if (waiter.interestPtr->active.load(std::memory_order_relaxed))
      {
        waiter.callback(cached);
      }
    }
  }

  async::Task<void> WindowsCoverArtLoader::load(WindowsCoverArtLoader* const loader,
                                                std::shared_ptr<rt::AppRuntime> runtimePtr,
                                                ResourceId const resourceId,
                                                std::stop_token const stopToken)
  {
    try
    {
      auto bytesResult = co_await runtimePtr->library().taskService().loadResourceAsync(resourceId, stopToken);
      auto bytes = std::vector<std::byte>{};
      if (bytesResult && *bytesResult)
      {
        bytes = std::move(**bytesResult);
      }
      co_await runtimePtr->async().resumeOnCallbackExecutor(stopToken);
      loader->complete(resourceId, std::move(bytes));
    }
    catch (async::OperationCancelled const&)
    {
    }
    catch (...)
    {
      runtimePtr->async().reportUnhandledException(std::current_exception(), "Windows cover-art load workflow");
    }
  }
} // namespace ao::winui
