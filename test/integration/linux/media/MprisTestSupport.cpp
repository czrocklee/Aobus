// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisTestSupport.h"

#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/utility/Raii.h>
#include <ao/utility/ScopedRegistration.h>

#include <gio/gio.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) -- POSIX SIGSTOP/SIGCONT require this header.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace ao::media::test
{
  namespace
  {
    struct GObjectDeleter final
    {
      void operator()(void* object) const noexcept
      {
        if (object != nullptr)
        {
          ::g_object_unref(object);
        }
      }
    };

    struct GFreeDeleter final
    {
      void operator()(void* value) const noexcept { ::g_free(value); }
    };

    using ObjectPtr = std::unique_ptr<void, GObjectDeleter>;
    using FreePtr = std::unique_ptr<char, GFreeDeleter>;

    std::string errorMessage(::GError* const error, std::string_view const fallback)
    {
      return error != nullptr ? std::string{error->message} : std::string{fallback};
    }

    std::string readDaemonAddress(::GInputStream* const stream)
    {
      auto dataStreamPtr = ObjectPtr{::g_data_input_stream_new(stream)};
      ::GError* error = nullptr;
      ::gsize length = 0;
      auto linePtr =
        FreePtr{::g_data_input_stream_read_line(G_DATA_INPUT_STREAM(dataStreamPtr.get()), &length, nullptr, &error)};
      auto const errorPtr = detail::ErrorPtr{error};

      if (!linePtr || length == 0)
      {
        throw std::runtime_error{errorMessage(error, "dbus-daemon did not publish an address")};
      }

      return std::string{linePtr.get(), length};
    }
  } // namespace

  namespace detail
  {
    void GErrorDeleter::operator()(::GError* const error) const noexcept
    {
      ::g_error_free(error);
    }

    void GVariantDeleter::operator()(::GVariant* const value) const noexcept
    {
      ::g_variant_unref(value);
    }
  } // namespace detail

  PrivateBus::PrivateBus(std::chrono::milliseconds const startupTimeout)
    : PrivateBus{std::string{}, startupTimeout}
  {
  }

  PrivateBus::PrivateBus(std::string const& listenAddress, std::chrono::milliseconds const startupTimeout)
  {
    auto const addressOption = "--address=" + listenAddress;
    ::GError* error = nullptr;
    _process = ::g_subprocess_new(
      static_cast<::GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
      &error,
      "dbus-daemon",
      "--session",
      "--nofork",
      "--print-address",
      listenAddress.empty() ? nullptr : addressOption.c_str(),
      nullptr);
    auto const errorPtr = detail::ErrorPtr{error};

    if (_process == nullptr)
    {
      throw std::runtime_error{errorMessage(error, "Could not launch dbus-daemon")};
    }

    try
    {
      auto* const output = ::g_subprocess_get_stdout_pipe(_process);
      auto outputPtr = ObjectPtr{g_object_ref(output)};
      auto addressFuture =
        std::async(std::launch::async,
                   [outputPtr = std::move(outputPtr)] { return readDaemonAddress(G_INPUT_STREAM(outputPtr.get())); });

      if (addressFuture.wait_for(startupTimeout) != std::future_status::ready)
      {
        stopNoThrow();

        try
        {
          std::ignore = addressFuture.get();
        }
        catch (...)
        {
          // Forced EOF can fail the reader; the timeout remains the startup failure.
          std::ignore = std::current_exception();
        }

        throw std::runtime_error{"Timed out waiting for dbus-daemon address"};
      }

      _address = addressFuture.get();
      _pid = parsePid(::g_subprocess_get_identifier(_process));
    }
    catch (...)
    {
      stopNoThrow();
      ::g_object_unref(_process);
      _process = nullptr;
      throw;
    }
  }

  PrivateBus::~PrivateBus()
  {
    stopNoThrow();

    if (_process != nullptr)
    {
      ::g_object_unref(_process);
    }
  }

  std::string const& PrivateBus::address() const noexcept
  {
    return _address;
  }

  void PrivateBus::stall(std::chrono::milliseconds const timeout)
  {
    if (_stopped)
    {
      throw std::logic_error{"Cannot stall a stopped private bus"};
    }

    if (!_stalled)
    {
      ::g_subprocess_send_signal(_process, SIGSTOP);
      // Cleanup must issue SIGCONT even if confirmation itself fails.
      _stalled = true;
      waitForStoppedState(true, timeout);
    }
  }

  void PrivateBus::resume(std::chrono::milliseconds const timeout)
  {
    if (_stopped || !_stalled)
    {
      return;
    }

    ::g_subprocess_send_signal(_process, SIGCONT);
    waitForStoppedState(false, timeout);
    _stalled = false;
  }

  void PrivateBus::stop()
  {
    if (_stopped)
    {
      return;
    }

    resume();
    ::g_subprocess_force_exit(_process);
    ::GError* error = nullptr;
    auto const waited = ::g_subprocess_wait(_process, nullptr, &error);
    auto const errorPtr = detail::ErrorPtr{error};

    if (waited == FALSE)
    {
      throw std::runtime_error{errorMessage(error, "Could not reap private dbus-daemon")};
    }

    _stopped = true;
  }

  ::pid_t PrivateBus::parsePid(char const* const identifier)
  {
    if (identifier == nullptr)
    {
      throw std::runtime_error{"dbus-daemon did not expose its child identifier"};
    }

    ::pid_t pid = 0;
    auto const text = std::string_view{identifier};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), pid);

    if (error != std::errc{} || end != text.data() + text.size() || pid <= 0)
    {
      throw std::runtime_error{"dbus-daemon child identifier is not a PID"};
    }

    return pid;
  }

  bool PrivateBus::isProcessStopped() const
  {
    auto input = std::ifstream{std::filesystem::path{"/proc"} / std::to_string(_pid) / "stat"};
    auto line = std::string{};
    std::getline(input, line);
    auto const commandEnd = line.rfind(')');
    return commandEnd != std::string::npos && commandEnd + 2 < line.size() &&
           (line[commandEnd + 2] == 'T' || line[commandEnd + 2] == 't');
  }

  void PrivateBus::waitForStoppedState(bool const expected, std::chrono::milliseconds const timeout) const
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (isProcessStopped() != expected)
    {
      if (std::chrono::steady_clock::now() >= deadline)
      {
        throw std::runtime_error{expected ? "Timed out stopping private dbus-daemon"
                                          : "Timed out resuming private dbus-daemon"};
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  void PrivateBus::resumeNoThrow() noexcept
  {
    if (_process != nullptr && !_stopped && _stalled)
    {
      ::g_subprocess_send_signal(_process, SIGCONT);
      _stalled = false;
    }
  }

  void PrivateBus::stopNoThrow() noexcept
  {
    if (_process == nullptr || _stopped)
    {
      return;
    }

    resumeNoThrow();
    ::g_subprocess_force_exit(_process);
    std::ignore = ::g_subprocess_wait(_process, nullptr, nullptr);
    _stopped = true;
  }

  VariantPtr takeVariant(::GVariant* const value)
  {
    return VariantPtr{value};
  }

  CallResult::CallResult(::GVariant* const value, ::GError* const error)
    : _valuePtr{value}, _errorPtr{error}
  {
  }

  CallResult::operator bool() const noexcept
  {
    return _valuePtr != nullptr && _errorPtr == nullptr;
  }

  ::GVariant* CallResult::value() const noexcept
  {
    return _valuePtr.get();
  }

  ::GError* CallResult::error() const noexcept
  {
    return _errorPtr.get();
  }

  std::string CallResult::errorMessage() const
  {
    return _errorPtr ? std::string{_errorPtr->message} : std::string{};
  }

  std::string CallResult::remoteErrorName() const
  {
    if (!_errorPtr)
    {
      return {};
    }

    auto namePtr = FreePtr{::g_dbus_error_get_remote_error(_errorPtr.get())};
    return namePtr ? std::string{namePtr.get()} : std::string{};
  }

  bool CallResult::isTimeout() const noexcept
  {
    return _errorPtr && ::g_error_matches(_errorPtr.get(), G_IO_ERROR, G_IO_ERROR_TIMED_OUT) == TRUE;
  }

  PendingCall::PendingCall(std::future<CallResult> future)
    : _future{std::move(future)}
  {
  }

  bool PendingCall::isReady()
  {
    return _future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
  }

  bool PendingCall::tryWaitFor(std::chrono::milliseconds const timeout)
  {
    return _future.wait_for(timeout) == std::future_status::ready;
  }

  CallResult PendingCall::take()
  {
    return _future.get();
  }

  BusClient::BusClient(std::string const& address)
  {
    auto contextPtr = utility::makeUniquePtr<::g_main_context_unref>(::g_main_context_new());
    ::g_main_context_push_thread_default(contextPtr.get());
    auto const popContext =
      utility::ScopedRegistration{[context = contextPtr.get()] { ::g_main_context_pop_thread_default(context); }};
    ::GError* error = nullptr;
    _connection = ::g_dbus_connection_new_for_address_sync(
      address.c_str(),
      static_cast<::GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr,
      nullptr,
      &error);
    auto const errorPtr = detail::ErrorPtr{error};

    if (_connection == nullptr)
    {
      throw std::runtime_error{errorMessage(error, "Could not connect private-bus client")};
    }

    ::g_dbus_connection_set_exit_on_close(_connection, FALSE);
    _context = contextPtr.release();
  }

  BusClient::~BusClient()
  {
    // Only client cleanup dispatches this private context. Active protocol
    // tests never pump the frontend/default GLib context. Drain closed signals
    // so a stopped context cannot retain its connection through queued sources.
    ::g_main_context_push_thread_default(_context);
    bool closeFinished = false;
    bool isClosed = false;
    auto const closedHandler = ::g_signal_connect_data(
      _connection,
      "closed",
      G_CALLBACK(+[](::GDBusConnection*, ::gboolean, ::GError*, ::gpointer data) { *static_cast<bool*>(data) = true; }),
      &isClosed,
      nullptr,
      static_cast<::GConnectFlags>(0));
    isClosed = ::g_dbus_connection_is_closed(_connection) != 0;
    ::g_dbus_connection_close(
      _connection,
      nullptr,
      [](::GObject* object, ::GAsyncResult* result, void* data)
      {
        std::ignore = ::g_dbus_connection_close_finish(G_DBUS_CONNECTION(object), result, nullptr);
        *static_cast<bool*>(data) = true;
      },
      &closeFinished);

    while (!closeFinished || !isClosed)
    {
      std::ignore = ::g_main_context_iteration(_context, TRUE);
    }

    while (::g_main_context_iteration(_context, FALSE) == TRUE)
    {
    }

    ::g_signal_handler_disconnect(_connection, closedHandler);
    ::g_main_context_pop_thread_default(_context);
    ::g_object_unref(_connection);
    ::g_main_context_unref(_context);
  }

  PendingCall BusClient::callAsync(std::string destination,
                                   std::string interfaceName,
                                   std::string methodName,
                                   ::GVariant* const parameters,
                                   ::GVariantType const* const replyType,
                                   std::chrono::milliseconds const timeout)
  {
    auto connectionPtr = ObjectPtr{g_object_ref(_connection)};
    auto parametersPtr = VariantPtr{parameters != nullptr ? ::g_variant_ref_sink(parameters) : nullptr};
    return PendingCall{std::async(std::launch::async,
                                  [connectionPtr = std::move(connectionPtr),
                                   destination = std::move(destination),
                                   interfaceName = std::move(interfaceName),
                                   methodName = std::move(methodName),
                                   parametersPtr = std::move(parametersPtr),
                                   replyType,
                                   timeout]
                                  {
                                    ::GError* error = nullptr;
                                    auto* const value =
                                      ::g_dbus_connection_call_sync(G_DBUS_CONNECTION(connectionPtr.get()),
                                                                    destination.c_str(),
                                                                    "/org/mpris/MediaPlayer2",
                                                                    interfaceName.c_str(),
                                                                    methodName.c_str(),
                                                                    parametersPtr.get(),
                                                                    replyType,
                                                                    G_DBUS_CALL_FLAGS_NONE,
                                                                    static_cast<std::int32_t>(timeout.count()),
                                                                    nullptr,
                                                                    &error);

                                    return CallResult{value, error};
                                  })};
  }

  CallResult awaitCall(PendingCall& call, rt::test::QueuedExecutor& executor, std::chrono::milliseconds const timeout)
  {
    if (!executor.tryDrainUntil([&call] { return call.isReady(); }, timeout))
    {
      throw std::runtime_error{"Timed out awaiting private-bus call while driving owner executor"};
    }

    return call.take();
  }

  CallResult awaitCallWithoutOwnerProgress(PendingCall& call, std::chrono::milliseconds const timeout)
  {
    if (!call.tryWaitFor(timeout))
    {
      throw std::runtime_error{"Timed out awaiting private-bus call without owner progress"};
    }

    return call.take();
  }
} // namespace ao::media::test
