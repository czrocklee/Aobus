// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/RequestCoalescer.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ScopedRegistration.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  /**
   * Interactive-runtime-scoped read-through cache of immutable resource bytes.
   *
   * Public methods and cache-owned state are confined to the bound runtime's
   * callback executor. A miss invokes the supplied reader; equal ids share one
   * in-flight read, while each callback interest remains independently
   * cancellable. Successful payloads are retained under entry and byte budgets.
   *
   * A cached id invokes its ready callback synchronously inside request() and
   * returns an empty Request. Callers must establish replacement or generation
   * state before calling request(). Ready callbacks may reenter request(). The
   * composition owner must not destroy the cache synchronously from a callback.
   */
  class ResourceByteMemoryCache final
  {
  public:
    using OnReady = compat::MoveOnlyFunction<void(ResourceBytes)>;
    using ReadBytes =
      std::function<async::Task<Result<std::optional<std::vector<std::byte>>>>(ResourceId, std::stop_token)>;
    using Request = utility::ScopedRegistration;

    static constexpr std::size_t kDefaultMaximumEntries = 128;
    static constexpr std::size_t kDefaultMaximumBytes = std::size_t{128U} * 1024U * 1024U;

    ResourceByteMemoryCache(async::Runtime& runtime,
                            ReadBytes readBytes,
                            std::size_t maximumEntries = kDefaultMaximumEntries,
                            std::size_t maximumBytes = kDefaultMaximumBytes);
    ~ResourceByteMemoryCache();

    ResourceByteMemoryCache(ResourceByteMemoryCache const&) = delete;
    ResourceByteMemoryCache& operator=(ResourceByteMemoryCache const&) = delete;
    ResourceByteMemoryCache(ResourceByteMemoryCache&&) = delete;
    ResourceByteMemoryCache& operator=(ResourceByteMemoryCache&&) = delete;

    Request request(ResourceId resourceId, OnReady onReady);

  private:
    using Requests = async::RequestCoalescer<ResourceId, ResourceBytes>;

    struct Entry final
    {
      ResourceBytes bytes;
      std::uint64_t lastUse = 0;
    };

    void startRead(ResourceId resourceId, Requests::FlightToken token);
    void complete(ResourceId resourceId, Requests::FlightToken const& token, std::vector<std::byte> bytes);
    static async::Task<void> runRead(ResourceByteMemoryCache* cache,
                                     async::Runtime* asyncRuntime,
                                     std::shared_ptr<ReadBytes const> readBytesPtr,
                                     ResourceId resourceId,
                                     Requests::FlightToken token,
                                     std::stop_token stopToken);

    ResourceBytes cached(ResourceId resourceId);
    void store(ResourceId resourceId, ResourceBytes bytes);
    void clearRetainedBytes();
    void evictLeastRecentlyUsed();

    std::shared_ptr<ReadBytes const> _readBytesPtr;
    async::Runtime* _asyncRuntime = nullptr;
    async::LifetimeScope _scope;
    std::size_t _maximumEntries;
    std::size_t _maximumBytes;
    std::size_t _cachedBytes = 0;
    std::uint64_t _useSequence = 0;
    std::unordered_map<ResourceId, Entry> _entries{};
    Requests _requests;
  };
} // namespace ao::rt
