// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/runtime/ExecutorTestSupport.h"

#include <gio/gio.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <sys/types.h>

namespace ao::media::test
{
  namespace detail
  {
    struct GErrorDeleter final
    {
      void operator()(::GError* error) const noexcept;
    };

    struct GVariantDeleter final
    {
      void operator()(::GVariant* value) const noexcept;
    };

    using ErrorPtr = std::unique_ptr<::GError, GErrorDeleter>;
  } // namespace detail

  /** Disposable private session bus for production MPRIS integration tests.
   *
   * The daemon is a direct child (no shell), no process environment is changed,
   * and cleanup signals and reaps only that child. A stalled child is always
   * resumed before termination.
   */
  class [[nodiscard]] PrivateBus final
  {
  public:
    explicit PrivateBus(std::chrono::milliseconds startupTimeout = std::chrono::seconds{2});
    explicit PrivateBus(std::string const& listenAddress,
                        std::chrono::milliseconds startupTimeout = std::chrono::seconds{2});
    ~PrivateBus();

    PrivateBus(PrivateBus const&) = delete;
    PrivateBus& operator=(PrivateBus const&) = delete;
    PrivateBus(PrivateBus&&) = delete;
    PrivateBus& operator=(PrivateBus&&) = delete;

    std::string const& address() const noexcept;
    void stall(std::chrono::milliseconds timeout = std::chrono::seconds{2});
    void resume(std::chrono::milliseconds timeout = std::chrono::seconds{2});
    void stop();

  private:
    static ::pid_t parsePid(char const* identifier);
    bool isProcessStopped() const;
    void waitForStoppedState(bool expected, std::chrono::milliseconds timeout) const;
    void resumeNoThrow() noexcept;
    void stopNoThrow() noexcept;

    ::GSubprocess* _process = nullptr;
    std::string _address;
    ::pid_t _pid = 0;
    bool _stalled = false;
    bool _stopped = false;
  };

  using VariantPtr = std::unique_ptr<::GVariant, detail::GVariantDeleter>;
  VariantPtr takeVariant(::GVariant* value);

  class [[nodiscard]] CallResult final
  {
  public:
    CallResult(::GVariant* value, ::GError* error);
    ~CallResult() = default;

    CallResult(CallResult&&) noexcept = default;
    CallResult& operator=(CallResult&&) noexcept = default;
    CallResult(CallResult const&) = delete;
    CallResult& operator=(CallResult const&) = delete;

    explicit operator bool() const noexcept;
    ::GVariant* value() const noexcept;
    ::GError* error() const noexcept;
    std::string errorMessage() const;
    std::string remoteErrorName() const;
    bool isTimeout() const noexcept;

  private:
    VariantPtr _valuePtr;
    detail::ErrorPtr _errorPtr;
  };

  class [[nodiscard]] PendingCall final
  {
  public:
    explicit PendingCall(std::future<CallResult> future);
    ~PendingCall() = default;

    PendingCall(PendingCall&&) noexcept = default;
    PendingCall& operator=(PendingCall&&) noexcept = default;
    PendingCall(PendingCall const&) = delete;
    PendingCall& operator=(PendingCall const&) = delete;

    bool isReady();
    bool tryWaitFor(std::chrono::milliseconds timeout);
    CallResult take();

  private:
    std::future<CallResult> _future;
  };

  /** Explicit-address client. Calls run on bounded worker futures, so a test
   * drives only the Aobus owner executor and never a global/default GLib loop.
   */
  class [[nodiscard]] BusClient final
  {
  public:
    explicit BusClient(std::string const& address);
    ~BusClient();

    BusClient(BusClient const&) = delete;
    BusClient& operator=(BusClient const&) = delete;
    BusClient(BusClient&&) = delete;
    BusClient& operator=(BusClient&&) = delete;

    // Borrowed for protocol-level observers; the client outlives registrations.
    ::GDBusConnection* nativeConnection() const noexcept { return _connection; }

    PendingCall callAsync(std::string destination,
                          std::string interfaceName,
                          std::string methodName,
                          ::GVariant* parameters,
                          ::GVariantType const* replyType,
                          std::chrono::milliseconds timeout = std::chrono::seconds{2});

  private:
    ::GDBusConnection* _connection = nullptr;
    ::GMainContext* _context = nullptr;
  };

  CallResult awaitCall(PendingCall& call,
                       rt::test::QueuedExecutor& executor,
                       std::chrono::milliseconds timeout = std::chrono::seconds{3});
  CallResult awaitCallWithoutOwnerProgress(PendingCall& call,
                                           std::chrono::milliseconds timeout = std::chrono::seconds{3});
} // namespace ao::media::test
