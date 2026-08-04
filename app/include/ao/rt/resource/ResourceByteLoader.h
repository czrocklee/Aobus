// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/RequestCoalescer.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteCache.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ScopedRegistration.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace ao::async
{
  class LifetimeScope;
  class Runtime;
}

namespace ao::rt
{
  class CoreRuntime;

  /**
   * Frontend-scoped delivery of immutable resource bytes.
   *
   * Public methods and loader-owned state are confined to the bound runtime's
   * callback executor. Equal ids share one read, while each callback interest
   * remains independently cancellable. Unbinding cancels external work before
   * clearing flights, cached bytes, and the bound byte source.
   *
   * A cached id invokes its ready callback synchronously inside request() and
   * returns an empty Request. Callers must establish replacement or generation
   * state before calling request(). Ready callbacks may reenter request() or
   * unbind(). The composition owner must not destroy the loader synchronously
   * from a ready callback.
   */
  class ResourceByteLoader final
  {
  public:
    using OnReady = std::move_only_function<void(ResourceBytes)>;
    using ReadBytes =
      std::function<async::Task<Result<std::optional<std::vector<std::byte>>>>(ResourceId, std::stop_token)>;
    using Request = utility::ScopedRegistration;

    ResourceByteLoader();
    explicit ResourceByteLoader(CoreRuntime& runtime);
    ResourceByteLoader(async::Runtime& runtime, ReadBytes readBytes);
    ~ResourceByteLoader();

    ResourceByteLoader(ResourceByteLoader const&) = delete;
    ResourceByteLoader& operator=(ResourceByteLoader const&) = delete;
    ResourceByteLoader(ResourceByteLoader&&) = delete;
    ResourceByteLoader& operator=(ResourceByteLoader&&) = delete;

    void bind(std::shared_ptr<CoreRuntime> runtimePtr);
    // A borrowed runtime must outlive this loader and all work cancelled by it.
    void bind(CoreRuntime& runtime);
    void bind(async::Runtime& runtime, ReadBytes readBytes);
    void unbind() noexcept;
    Request request(ResourceId resourceId, OnReady onReady);

  private:
    using Requests = async::RequestCoalescer<ResourceId, ResourceBytes>;

    void spawn(ResourceId resourceId, Requests::FlightToken token);
    void complete(ResourceId resourceId, Requests::FlightToken const& token, std::vector<std::byte> bytes);
    static async::Task<void> read(ResourceByteLoader* loader,
                                  async::Runtime* asyncRuntime,
                                  std::shared_ptr<ReadBytes const> readBytesPtr,
                                  ResourceId resourceId,
                                  Requests::FlightToken token,
                                  std::stop_token stopToken);

    std::shared_ptr<ReadBytes const> _readBytesPtr;
    async::Runtime* _asyncRuntime = nullptr;
    std::unique_ptr<async::LifetimeScope> _scopePtr;
    ResourceByteCache _cache;
    Requests _requests;
  };
} // namespace ao::rt
