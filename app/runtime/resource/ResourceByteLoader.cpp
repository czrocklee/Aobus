// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/resource/ResourceByteCache.h>
#include <ao/rt/resource/ResourceByteLoader.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::rt
{
  ResourceByteLoader::ResourceByteLoader() = default;

  ResourceByteLoader::ResourceByteLoader(CoreRuntime& runtime)
  {
    bind(runtime);
  }

  ResourceByteLoader::ResourceByteLoader(async::Runtime& runtime, ReadBytes readBytes)
  {
    bind(runtime, std::move(readBytes));
  }

  ResourceByteLoader::~ResourceByteLoader()
  {
    unbind();
  }

  void ResourceByteLoader::bind(std::shared_ptr<CoreRuntime> runtimePtr)
  {
    gsl_Expects(runtimePtr);
    auto& asyncRuntime = runtimePtr->async();
    bind(asyncRuntime,
         [runtimePtr = std::move(runtimePtr)](ResourceId const resourceId, std::stop_token const stopToken)
         { return runtimePtr->library().taskService().loadResourceAsync(resourceId, stopToken); });
  }

  void ResourceByteLoader::bind(CoreRuntime& runtime)
  {
    bind(runtime.async(),
         [&runtime](ResourceId const resourceId, std::stop_token const stopToken)
         { return runtime.library().taskService().loadResourceAsync(resourceId, stopToken); });
  }

  void ResourceByteLoader::bind(async::Runtime& runtime, ReadBytes readBytes)
  {
    gsl_Expects(readBytes);
    auto readBytesPtr = std::make_shared<ReadBytes const>(std::move(readBytes));
    auto scopePtr = std::make_unique<async::LifetimeScope>();

    unbind();
    _readBytesPtr = std::move(readBytesPtr);
    _scopePtr = std::move(scopePtr);
    _asyncRuntime = &runtime;
  }

  void ResourceByteLoader::unbind()
  {
    _scopePtr.reset();
    _requests.clear();
    _cache.reset();
    _readBytesPtr.reset();
    _asyncRuntime = nullptr;
  }

  ResourceByteLoader::Request ResourceByteLoader::request(ResourceId const resourceId, OnReady onReady)
  {
    if (_asyncRuntime == nullptr || resourceId == kInvalidResourceId || !onReady)
    {
      return {};
    }

    if (auto const bytes = _cache.cached(resourceId); !bytes.empty())
    {
      onReady(bytes);
      return {};
    }

    auto callback =
      Requests::Callback{[onReady = std::move(onReady)](ResourceBytes const& bytes) mutable { onReady(bytes); }};

    return _requests.request(resourceId,
                             std::move(callback),
                             [this, resourceId](Requests::FlightToken token) { spawn(resourceId, std::move(token)); });
  }

  void ResourceByteLoader::spawn(ResourceId const resourceId, Requests::FlightToken token)
  {
    auto* const asyncRuntime = _asyncRuntime;
    auto readBytesPtr = _readBytesPtr;
    asyncRuntime->spawnWithLifetime(
      _scopePtr.get(),
      [loader = this, asyncRuntime, readBytesPtr = std::move(readBytesPtr), resourceId, token = std::move(token)](
        std::stop_token const stopToken) mutable
      { return read(loader, asyncRuntime, readBytesPtr, resourceId, std::move(token), stopToken); });
  }

  void ResourceByteLoader::complete(ResourceId const resourceId,
                                    Requests::FlightToken const& token,
                                    std::vector<std::byte> bytes)
  {
    auto resourceBytes = ResourceBytes{};

    if (_cache.store(resourceId, ResourceBytes{std::move(bytes)}))
    {
      resourceBytes = _cache.cached(resourceId);
    }

    _requests.complete(token, resourceBytes);
  }

  async::Task<void> ResourceByteLoader::read(ResourceByteLoader* const loader,
                                             async::Runtime* const asyncRuntime,
                                             std::shared_ptr<ReadBytes const> readBytesPtr,
                                             ResourceId const resourceId,
                                             Requests::FlightToken token,
                                             std::stop_token const stopToken)
  {
    auto bytes = std::vector<std::byte>{};

    try
    {
      auto bytesResult = co_await std::invoke(*readBytesPtr, resourceId, stopToken);

      if (bytesResult && *bytesResult)
      {
        bytes = std::move(**bytesResult);
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      asyncRuntime->reportUnhandledException(std::current_exception(), "resource byte delivery");
    }

    co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);
    loader->complete(resourceId, token, std::move(bytes));
  }
} // namespace ao::rt
