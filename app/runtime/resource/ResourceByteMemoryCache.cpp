// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/resource/ResourceByteMemoryCache.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::rt
{
  ResourceByteMemoryCache::ResourceByteMemoryCache(async::Runtime& runtime,
                                                   ReadBytes readBytes,
                                                   std::size_t const maximumEntries,
                                                   std::size_t const maximumBytes)
    : _readBytesPtr{std::make_shared<ReadBytes const>(std::move(readBytes))}
    , _asyncRuntime{&runtime}
    , _maximumEntries{std::max<std::size_t>(1, maximumEntries)}
    , _maximumBytes{std::max<std::size_t>(1, maximumBytes)}
  {
    AO_EXPECTS(_readBytesPtr && *_readBytesPtr);
    _entries.reserve(_maximumEntries);
  }

  ResourceByteMemoryCache::~ResourceByteMemoryCache()
  {
    // Cancel external work before the flights and cached bytes it completes into
    // are released. Member declaration order is the fallback, not the mechanism.
    _scope.cancelAll();
    _requests.clear();
    clearRetainedBytes();
  }

  ResourceByteMemoryCache::Request ResourceByteMemoryCache::request(ResourceId const resourceId, OnReady onReady)
  {
    if (resourceId == kInvalidResourceId || !onReady)
    {
      return {};
    }

    if (auto const bytes = cached(resourceId); !bytes.empty())
    {
      onReady(bytes);
      return {};
    }

    auto callback =
      Requests::Callback{[onReady = std::move(onReady)](ResourceBytes const& bytes) mutable { onReady(bytes); }};

    return _requests.request(resourceId,
                             std::move(callback),
                             [this, resourceId](Requests::FlightToken token)
                             { startRead(resourceId, std::move(token)); });
  }

  void ResourceByteMemoryCache::startRead(ResourceId const resourceId, Requests::FlightToken token)
  {
    auto* const asyncRuntime = _asyncRuntime;
    auto readBytesPtr = _readBytesPtr;
    asyncRuntime->spawnWithLifetime(
      _scope,
      [cache = this, asyncRuntime, readBytesPtr = std::move(readBytesPtr), resourceId, token = std::move(token)](
        std::stop_token const stopToken) mutable
      { return runRead(cache, asyncRuntime, readBytesPtr, resourceId, std::move(token), stopToken); },
      "resource byte cache read");
  }

  void ResourceByteMemoryCache::complete(ResourceId const resourceId,
                                         Requests::FlightToken const& token,
                                         std::vector<std::byte> bytes)
  {
    auto resourceBytes = ResourceBytes{std::move(bytes)};

    if (!resourceBytes.empty())
    {
      store(resourceId, resourceBytes);
    }

    _requests.complete(token, resourceBytes);
  }

  async::Task<void> ResourceByteMemoryCache::runRead(ResourceByteMemoryCache* const cache,
                                                     async::Runtime* const asyncRuntime,
                                                     std::shared_ptr<ReadBytes const> readBytesPtr,
                                                     ResourceId const resourceId,
                                                     Requests::FlightToken token,
                                                     std::stop_token const stopToken)
  {
    auto bytes = std::vector<std::byte>{};
    auto bytesRes = co_await std::invoke(*readBytesPtr, resourceId, stopToken);

    if (bytesRes && *bytesRes)
    {
      bytes = std::move(**bytesRes);
    }

    co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);
    cache->complete(resourceId, token, std::move(bytes));
  }

  ResourceBytes ResourceByteMemoryCache::cached(ResourceId const resourceId)
  {
    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      it->second.lastUse = ++_useSequence;
      return it->second.bytes;
    }

    return {};
  }

  void ResourceByteMemoryCache::store(ResourceId const resourceId, ResourceBytes bytes)
  {
    if (resourceId == kInvalidResourceId || bytes.empty())
    {
      return;
    }

    auto const byteCount = bytes.view().size();

    if (byteCount > _maximumBytes)
    {
      return;
    }

    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      _cachedBytes -= it->second.bytes.view().size();
      it->second.bytes = std::move(bytes);
      it->second.lastUse = ++_useSequence;
      _cachedBytes += byteCount;

      while (_cachedBytes > _maximumBytes)
      {
        evictLeastRecentlyUsed();
      }

      return;
    }

    while (_entries.size() >= _maximumEntries || _cachedBytes > _maximumBytes - byteCount)
    {
      evictLeastRecentlyUsed();
    }

    _entries.emplace(resourceId, Entry{.bytes = std::move(bytes), .lastUse = ++_useSequence});
    _cachedBytes += byteCount;
  }

  void ResourceByteMemoryCache::clearRetainedBytes()
  {
    _entries.clear();
    _cachedBytes = 0;
    _useSequence = 0;
  }

  void ResourceByteMemoryCache::evictLeastRecentlyUsed()
  {
    auto oldest = _entries.end();
    auto oldestUse = std::numeric_limits<std::uint64_t>::max();

    for (auto it = _entries.begin(); it != _entries.end(); ++it)
    {
      if (it->second.lastUse < oldestUse)
      {
        oldest = it;
        oldestUse = it->second.lastUse;
      }
    }

    if (oldest != _entries.end())
    {
      _cachedBytes -= oldest->second.bytes.view().size();
      _entries.erase(oldest);
    }
  }
} // namespace ao::rt
