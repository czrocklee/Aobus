// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "media/linux/MprisBusSession.h"

#include "MprisTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <giomm/dbusconnection.h>
#include <giomm/dbusintrospection.h>
#include <giomm/init.h>
#include <glibmm/refptr.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace ao::media::test
{
  namespace
  {
    constexpr auto kInterface = "org.mpris.MediaPlayer2.AobusTest";
    constexpr auto kBusName = "org.mpris.MediaPlayer2.aobus.sessiontest";
    constexpr auto kIntrospectionXml = R"xml(
<node>
  <interface name="org.mpris.MediaPlayer2.AobusTest">
    <method name="Ping">
      <arg name="value" type="s" direction="in"/>
      <arg name="value" type="s" direction="out"/>
    </method>
    <method name="Hold"/>
  </interface>
</node>
)xml";

    class DeadlineRescue final
    {
    public:
      DeadlineRescue(std::chrono::milliseconds const timeout, std::function<void()> rescue)
        : _thread{[this, timeout, rescue = std::move(rescue)]
                  {
                    auto lock = std::unique_lock{_mutex};

                    if (_cv.wait_for(lock, timeout, [this] { return _stopped; }))
                    {
                      return;
                    }

                    lock.unlock();
                    _fired.store(true);
                    rescue();
                  }}
      {
      }

      ~DeadlineRescue() { stopAndJoin(); }

      DeadlineRescue(DeadlineRescue const&) = delete;
      DeadlineRescue& operator=(DeadlineRescue const&) = delete;
      DeadlineRescue(DeadlineRescue&&) = delete;
      DeadlineRescue& operator=(DeadlineRescue&&) = delete;

      void stopAndJoin()
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _stopped = true;
        }

        _cv.notify_all();

        if (_thread.joinable())
        {
          _thread.join();
        }
      }

      bool hasFired() const noexcept { return _fired.load(); }

    private:
      std::mutex _mutex;
      std::condition_variable _cv;
      bool _stopped = false;
      std::atomic_bool _fired = false;
      std::thread _thread;
    };

    class StalledAuthenticationEndpoint final
    {
    public:
      StalledAuthenticationEndpoint()
      {
        auto listenerPtr = utility::makeUniquePtr<::g_object_unref>(::g_socket_listener_new());
        auto cancellablePtr = utility::makeUniquePtr<::g_object_unref>(::g_cancellable_new());
        _listener = listenerPtr.get();
        _cancellable = cancellablePtr.get();
        auto const socketPath = (_tempDir.path() / "stalled-auth.sock").string();
        auto* const address = ::g_unix_socket_address_new(socketPath.c_str());
        ::GError* error = nullptr;
        auto const added = ::g_socket_listener_add_address(
          _listener, address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, &error);
        ::g_object_unref(address);

        if (added == FALSE)
        {
          auto const message = error != nullptr ? std::string{error->message} : "Could not listen for D-Bus auth";
          ::g_clear_error(&error);
          throw std::runtime_error{message};
        }

        _address = "unix:path=" + socketPath;
        _acceptThread = std::thread{[this]
                                    {
                                      ::GError* acceptError = nullptr;
                                      auto* const connection =
                                        ::g_socket_listener_accept(_listener, nullptr, _cancellable, &acceptError);

                                      {
                                        auto const lock = std::scoped_lock{_mutex};
                                        _accepted = connection;
                                        _acceptFinished = true;
                                      }

                                      ::g_clear_error(&acceptError);
                                      _cv.notify_all();
                                    }};
        std::ignore = listenerPtr.release();
        std::ignore = cancellablePtr.release();
      }

      ~StalledAuthenticationEndpoint()
      {
        closePeerAndListener();

        if (_accepted != nullptr)
        {
          ::g_object_unref(_accepted);
        }

        ::g_object_unref(_cancellable);
        ::g_object_unref(_listener);
      }

      StalledAuthenticationEndpoint(StalledAuthenticationEndpoint const&) = delete;
      StalledAuthenticationEndpoint& operator=(StalledAuthenticationEndpoint const&) = delete;
      StalledAuthenticationEndpoint(StalledAuthenticationEndpoint&&) = delete;
      StalledAuthenticationEndpoint& operator=(StalledAuthenticationEndpoint&&) = delete;

      std::string const& address() const noexcept { return _address; }

      bool tryWaitForAcceptance(std::chrono::milliseconds const timeout = std::chrono::seconds{2})
      {
        auto lock = std::unique_lock{_mutex};
        std::ignore = _cv.wait_for(lock, timeout, [this] { return _acceptFinished; });
        return _accepted != nullptr;
      }

      void closePeerAndListener() noexcept
      {
        ::g_cancellable_cancel(_cancellable);
        ::g_socket_listener_close(_listener);

        if (_acceptThread.joinable())
        {
          _acceptThread.join();
        }

        auto* accepted = static_cast<::GSocketConnection*>(nullptr);

        {
          auto const lock = std::scoped_lock{_mutex};

          if (_accepted != nullptr)
          {
            accepted = G_SOCKET_CONNECTION(g_object_ref(_accepted));
          }
        }

        if (accepted != nullptr)
        {
          std::ignore = ::g_io_stream_close(G_IO_STREAM(accepted), nullptr, nullptr);
          ::g_object_unref(accepted);
        }
      }

    private:
      ao::test::TempDir _tempDir;
      ::GSocketListener* _listener = nullptr;
      ::GCancellable* _cancellable = nullptr;
      std::string _address;
      std::mutex _mutex;
      std::condition_variable _cv;
      ::GSocketConnection* _accepted = nullptr;
      bool _acceptFinished = false;
      std::thread _acceptThread;
    };

    class ScriptedAuthenticationEndpoint final
    {
    public:
      enum class Mode : std::uint8_t
      {
        CloseAuthentication,
        RejectAuthentication,
        MalformedAuthentication,
        OversizedAuthentication,
        StallExternalResponse,
        MismatchedGuid,
        DropHello,
        StallHello,
      };

      explicit ScriptedAuthenticationEndpoint(Mode const mode)
        : _mode{mode}
      {
        auto listenerPtr = utility::makeUniquePtr<::g_object_unref>(::g_socket_listener_new());
        auto cancellablePtr = utility::makeUniquePtr<::g_object_unref>(::g_cancellable_new());
        _listener = listenerPtr.get();
        _cancellable = cancellablePtr.get();
        auto const socketPath = (_tempDir.path() / "scripted-auth.sock").string();
        auto* const address = ::g_unix_socket_address_new(socketPath.c_str());
        ::GError* error = nullptr;
        auto const added = ::g_socket_listener_add_address(
          _listener, address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, &error);
        ::g_object_unref(address);

        if (added == FALSE)
        {
          auto const message = error != nullptr ? std::string{error->message} : "Could not listen for D-Bus auth";
          ::g_clear_error(&error);
          throw std::runtime_error{message};
        }

        _address = "unix:path=" + socketPath;

        if (_mode == Mode::MismatchedGuid)
        {
          _address += ",guid=ffffffffffffffffffffffffffffffff";
        }

        _thread = std::thread{[this] { run(); }};
        std::ignore = listenerPtr.release();
        std::ignore = cancellablePtr.release();
      }

      ~ScriptedAuthenticationEndpoint()
      {
        closePeerAndListener();

        if (_accepted != nullptr)
        {
          ::g_object_unref(_accepted);
        }

        ::g_object_unref(_cancellable);
        ::g_object_unref(_listener);
      }

      ScriptedAuthenticationEndpoint(ScriptedAuthenticationEndpoint const&) = delete;
      ScriptedAuthenticationEndpoint& operator=(ScriptedAuthenticationEndpoint const&) = delete;
      ScriptedAuthenticationEndpoint(ScriptedAuthenticationEndpoint&&) = delete;
      ScriptedAuthenticationEndpoint& operator=(ScriptedAuthenticationEndpoint&&) = delete;

      std::string const& address() const noexcept { return _address; }

      bool tryWaitForProtocolPoint(std::chrono::milliseconds const timeout = std::chrono::seconds{2})
      {
        auto lock = std::unique_lock{_mutex};
        std::ignore = _cv.wait_for(lock, timeout, [this] { return _protocolPointReached || !_error.empty(); });
        return _protocolPointReached;
      }

      bool tryWaitForPeerClose(std::chrono::milliseconds const timeout = std::chrono::seconds{2})
      {
        auto lock = std::unique_lock{_mutex};
        std::ignore = _cv.wait_for(lock, timeout, [this] { return _peerClosed || !_error.empty(); });
        return _peerClosed;
      }

      std::string error() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _error;
      }

      void closePeerAndListener() noexcept
      {
        ::g_cancellable_cancel(_cancellable);
        ::g_socket_listener_close(_listener);

        {
          auto const lock = std::scoped_lock{_mutex};
          _closing = true;
        }

        if (_thread.joinable())
        {
          _thread.join();
        }

        if (_accepted != nullptr)
        {
          std::ignore = ::g_io_stream_close(G_IO_STREAM(_accepted), nullptr, nullptr);
        }
      }

    private:
      void writeAll(::GOutputStream* output, std::string_view const text)
      {
        for (char const character : text)
        {
          ::gsize written = 0;
          ::GError* error = nullptr;
          auto const succeeded = ::g_output_stream_write_all(output, &character, 1, &written, _cancellable, &error);
          auto const message = error != nullptr ? std::string{error->message} : std::string{};
          ::g_clear_error(&error);

          if (succeeded == FALSE || written != 1)
          {
            throw std::runtime_error{message.empty() ? "Could not write D-Bus authentication response" : message};
          }
        }
      }

      std::string readLine(::GInputStream* input)
      {
        auto line = std::string{};

        while (line.size() != 1024)
        {
          char character = 0;
          ::GError* error = nullptr;
          auto const count = ::g_input_stream_read(input, &character, 1, _cancellable, &error);
          auto const message = error != nullptr ? std::string{error->message} : std::string{};
          ::g_clear_error(&error);

          if (count <= 0)
          {
            throw std::runtime_error{message.empty() ? "D-Bus authentication peer closed" : message};
          }

          if (character == '\n')
          {
            if (line.empty() || line.back() != '\r')
            {
              throw std::runtime_error{"Invalid D-Bus authentication line"};
            }

            line.pop_back();
            return line;
          }

          line.push_back(character);
        }

        throw std::runtime_error{"D-Bus authentication line is too long"};
      }

      void reachProtocolPoint()
      {
        {
          auto const lock = std::scoped_lock{_mutex};
          _protocolPointReached = true;
        }

        _cv.notify_all();
      }

      void awaitPeerClose(::GInputStream* input)
      {
        char value = 0;

        while (true)
        {
          ::GError* error = nullptr;
          ::gssize const count = ::g_input_stream_read(input, &value, 1, _cancellable, &error);
          ::g_clear_error(&error);

          if (count <= 0)
          {
            break;
          }
        }

        {
          auto const lock = std::scoped_lock{_mutex};
          _peerClosed = true;
        }

        _cv.notify_all();
      }

      void run()
      {
        try
        {
          ::GError* error = nullptr;
          auto* const connection = ::g_socket_listener_accept(_listener, nullptr, _cancellable, &error);

          if (connection == nullptr)
          {
            auto const message = error != nullptr ? std::string{error->message} : "Could not accept D-Bus peer";
            ::g_clear_error(&error);
            throw std::runtime_error{message};
          }

          {
            auto const lock = std::scoped_lock{_mutex};
            _accepted = connection;
          }

          auto* const input = ::g_io_stream_get_input_stream(G_IO_STREAM(connection));
          auto* const output = ::g_io_stream_get_output_stream(G_IO_STREAM(connection));
          char initialByte = 0;
          auto const initialCount = ::g_input_stream_read(input, &initialByte, 1, _cancellable, &error);

          if (initialCount != 1 || initialByte != '\0')
          {
            auto const message = error != nullptr ? std::string{error->message} : "Missing D-Bus credentials byte";
            ::g_clear_error(&error);
            throw std::runtime_error{message};
          }

          if (readLine(input) != "AUTH EXTERNAL")
          {
            throw std::runtime_error{"Unexpected D-Bus authentication command"};
          }

          if (_mode == Mode::CloseAuthentication)
          {
            reachProtocolPoint();
            std::ignore = ::g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
            return;
          }

          if (_mode == Mode::RejectAuthentication)
          {
            writeAll(output, "REJECTED NO_SUCH_MECHANISM\r\n");
            reachProtocolPoint();
            awaitPeerClose(input);
            return;
          }

          if (_mode == Mode::MalformedAuthentication)
          {
            writeAll(output, std::string_view{"DATA\0", 5});
            reachProtocolPoint();
            awaitPeerClose(input);
            return;
          }

          if (_mode == Mode::OversizedAuthentication)
          {
            writeAll(output, std::string(1024, 'A'));
            reachProtocolPoint();
            awaitPeerClose(input);
            return;
          }

          writeAll(output, _mode == Mode::StallHello ? "DATA \r\n" : "DATA\r\n");

          if (readLine(input) != "DATA")
          {
            throw std::runtime_error{"Unexpected D-Bus EXTERNAL response"};
          }

          if (_mode == Mode::StallExternalResponse)
          {
            reachProtocolPoint();
            awaitPeerClose(input);
            return;
          }

          writeAll(output, "OK 0123456789abcdef0123456789abcdef\r\n");

          if (readLine(input) != "BEGIN")
          {
            throw std::runtime_error{"Unexpected D-Bus authentication completion"};
          }

          if (_mode == Mode::MismatchedGuid)
          {
            reachProtocolPoint();
            awaitPeerClose(input);
            return;
          }

          char firstMessageByte = 0;
          auto const messageCount = ::g_input_stream_read(input, &firstMessageByte, 1, _cancellable, &error);

          if (messageCount != 1)
          {
            auto const message = error != nullptr ? std::string{error->message} : "Missing D-Bus Hello";
            ::g_clear_error(&error);
            throw std::runtime_error{message};
          }

          reachProtocolPoint();

          if (_mode == Mode::DropHello)
          {
            std::ignore = ::g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
            return;
          }

          awaitPeerClose(input);
        }
        catch (std::exception const& error)
        {
          auto const lock = std::scoped_lock{_mutex};

          if (!_closing)
          {
            _error = error.what();
          }

          _cv.notify_all();
        }
      }

      ao::test::TempDir _tempDir;
      Mode _mode;
      ::GSocketListener* _listener = nullptr;
      ::GCancellable* _cancellable = nullptr;
      std::string _address;
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      ::GSocketConnection* _accepted = nullptr;
      bool _protocolPointReached = false;
      bool _peerClosed = false;
      bool _closing = false;
      std::string _error;
      std::thread _thread;
    };

    class DroppingExecutor final : public async::Executor
    {
    public:
      explicit DroppingExecutor(rt::test::QueuedExecutor& executor)
        : _executor{executor}
      {
      }
      ~DroppingExecutor() override = default;

      DroppingExecutor(DroppingExecutor const&) = delete;
      DroppingExecutor& operator=(DroppingExecutor const&) = delete;
      DroppingExecutor(DroppingExecutor&&) = delete;
      DroppingExecutor& operator=(DroppingExecutor&&) = delete;
      bool isCurrent() const noexcept override { return _executor.isCurrent(); }
      void dispatch(compat::MoveOnlyFunction<void()> task) override { defer(std::move(task)); }
      void defer(compat::MoveOnlyFunction<void()> task) override
      {
        if (!_drop.load())
        {
          _executor.defer(std::move(task));
        }
      }
      void setDropping(bool enabled) noexcept { _drop.store(enabled); }

    private:
      rt::test::QueuedExecutor& _executor;
      std::atomic_bool _drop = false;
    };

    Glib::RefPtr<Gio::DBus::NodeInfo> testNodeInfo()
    {
      Gio::init();
      return Gio::DBus::NodeInfo::create_for_xml(kIntrospectionXml);
    }

    struct SessionState final
    {
      Glib::RefPtr<Gio::DBus::Connection> acquiredConnectionPtr;
      std::string acquiredName;
      std::string unavailableMessage;
      std::int32_t handledCount = 0;
      std::int32_t lateDropCount = 0;
      bool handledOnOwner = false;
      bool admitted = true;
    };

    MprisBusSession::Outputs pingOutputs(SessionState& state, rt::test::QueuedExecutor& executor)
    {
      return {
        .handleRequest =
          [&state, &executor](MprisBusSession::Invocation invocationPtr)
        {
          if (!state.admitted)
          {
            ++state.lateDropCount;
            return;
          }

          ++state.handledCount;
          state.handledOnOwner = executor.isCurrent();
          auto const methodName = std::string_view{::g_dbus_method_invocation_get_method_name(invocationPtr.get())};

          if (methodName == "Ping")
          {
            char const* value = nullptr;
            ::g_variant_get(::g_dbus_method_invocation_get_parameters(invocationPtr.get()), "(&s)", &value);
            ::g_dbus_method_invocation_return_value(invocationPtr.release(), ::g_variant_new("(s)", value));
          }
        },
        .acquired =
          [&state](Glib::RefPtr<Gio::DBus::Connection> connectionPtr, std::string name)
        {
          state.acquiredConnectionPtr = std::move(connectionPtr);
          state.acquiredName = std::move(name);
        },
        .unavailable = [&state](std::string message) { state.unavailableMessage = std::move(message); },
      };
    }

    void awaitAcquisition(SessionState& state, rt::test::QueuedExecutor& executor)
    {
      REQUIRE(executor.tryDrainUntil([&state] { return !state.acquiredName.empty(); }));
      CHECK(state.acquiredName == kBusName);
      CHECK(state.unavailableMessage.empty());
    }

    void checkPingReply(CallResult const& result, std::string_view const expected)
    {
      REQUIRE(result);
      REQUIRE(::g_variant_is_of_type(result.value(), G_VARIANT_TYPE("(s)")) == TRUE);
      char const* value = nullptr;
      ::g_variant_get(result.value(), "(&s)", &value);
      CHECK(std::string_view{value} == expected);
    }
  } // namespace

  TEST_CASE("MprisBusSession private bus - native registration dispatches owned invocations",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    awaitAcquisition(state, executor);
    auto client = BusClient{bus.address()};

    auto pingCall = client.callAsync(
      state.acquiredName, kInterface, "Ping", ::g_variant_new("(s)", "native boundary"), G_VARIANT_TYPE("(s)"));
    executor.checkQueued();
    CHECK_FALSE(pingCall.isReady());
    auto pingReply = awaitCall(pingCall, executor);
    checkPingReply(pingReply, "native boundary");
    CHECK(state.handledCount == 1);
    CHECK(state.handledOnOwner);

    sessionPtr->retire();
    sessionPtr.reset();
  }

  TEST_CASE("MprisBusSession private bus - spoofed NameLost does not retire the session",
            "[platform][integration][mpris]")
  {
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    awaitAcquisition(state, executor);
    auto client = BusClient{bus.address()};
    ::GError* error = nullptr;
    auto const sent = ::g_dbus_connection_emit_signal(client.nativeConnection(),
                                                      state.acquiredName.c_str(),
                                                      "/org/freedesktop/DBus",
                                                      "org.freedesktop.DBus",
                                                      "NameLost",
                                                      ::g_variant_new("(s)", state.acquiredName.c_str()),
                                                      &error);
    auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);
    REQUIRE(sent != 0);
    REQUIRE(error == nullptr);

    auto pingCall = client.callAsync(
      state.acquiredName, kInterface, "Ping", ::g_variant_new("(s)", "after spoof"), G_VARIANT_TYPE("(s)"));
    auto pingReply = awaitCall(pingCall, executor);
    checkPingReply(pingReply, "after spoof");
    CHECK(state.unavailableMessage.empty());
  }

  TEST_CASE("MprisBusSession private bus - no-queue name conflict degrades only the second session",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto firstState = SessionState{};
    auto firstSessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(firstState, executor));
    awaitAcquisition(firstState, executor);
    auto secondState = SessionState{};
    auto secondSessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(secondState, executor));
    REQUIRE(executor.tryDrainUntil([&secondState] { return !secondState.unavailableMessage.empty(); }));
    CHECK(secondState.acquiredName.empty());
    CHECK(secondState.unavailableMessage.contains("Could not acquire MPRIS bus name"));

    secondSessionPtr.reset();
    auto client = BusClient{bus.address()};
    auto pingCall = client.callAsync(
      firstState.acquiredName, kInterface, "Ping", ::g_variant_new("(s)", "first remains"), G_VARIANT_TYPE("(s)"));
    auto pingReply = awaitCall(pingCall, executor);
    checkPingReply(pingReply, "first remains");
  }

  TEST_CASE("MprisBusSession private bus - genuine NameLost retires a retained connection",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    awaitAcquisition(state, executor);
    REQUIRE(state.acquiredConnectionPtr);
    ::GError* error = nullptr;
    auto const replyPtr = utility::makeUniquePtr<::g_variant_unref>(
      ::g_dbus_connection_call_sync(state.acquiredConnectionPtr->gobj(),
                                    "org.freedesktop.DBus",
                                    "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus",
                                    "ReleaseName",
                                    ::g_variant_new("(s)", state.acquiredName.c_str()),
                                    G_VARIANT_TYPE("(u)"),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    2000,
                                    nullptr,
                                    &error));
    auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);
    REQUIRE(replyPtr);
    REQUIRE(error == nullptr);
    ::guint32 result = 0;
    ::g_variant_get(replyPtr.get(), "(u)", &result);
    REQUIRE(result == 1);
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));
    CHECK(state.unavailableMessage == "Lost MPRIS bus name " + state.acquiredName);

    sessionPtr.reset();
    CHECK(::g_dbus_connection_is_closed(state.acquiredConnectionPtr->gobj()) != 0);
  }

  TEST_CASE("MprisBusSession private bus - repeated retirement settles and releases acquired connections",
            "[platform][integration][mpris][concurrency][stress]")
  {
    auto bus = PrivateBus{};

    for (std::int32_t iteration = 0; iteration != 64; ++iteration)
    {
      INFO(iteration);
      auto executor = rt::test::QueuedExecutor{};
      auto state = SessionState{};
      auto sessionPtr = std::make_unique<MprisBusSession>(
        executor, bus.address(), kBusName, true, testNodeInfo(), pingOutputs(state, executor));
      REQUIRE(executor.tryDrainUntil([&state] { return !state.acquiredName.empty(); }));
      REQUIRE(state.acquiredConnectionPtr);
      CHECK(state.unavailableMessage.empty());
      auto finalizedPtr = std::make_unique<std::promise<void>>();
      auto finalized = finalizedPtr->get_future();
      ::g_object_weak_ref(
        G_OBJECT(state.acquiredConnectionPtr->gobj()),
        [](::gpointer data, ::GObject*) noexcept
        {
          auto notificationPtr = std::unique_ptr<std::promise<void>>{static_cast<std::promise<void>*>(data)};
          notificationPtr->set_value();
        },
        finalizedPtr.release());

      // Joining must observe closure without requiring the owner to drain.
      sessionPtr.reset();
      CHECK(::g_dbus_connection_is_closed(state.acquiredConnectionPtr->gobj()) != 0);
      executor.drain();
      state.acquiredConnectionPtr.reset();

      // A stranded closed-signal source would retain the connection and its
      // stopped private context. Only transport-worker cleanup may remain.
      REQUIRE(finalized.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      finalized.get();
    }
  }

  TEST_CASE("MprisBusSession private bus - queued invocation survives join and disconnects caller",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto executorOwnerPtr = std::make_unique<rt::test::QueuedExecutor>();
    auto& executor = *executorOwnerPtr;
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    awaitAcquisition(state, executor);
    executor.drain();
    REQUIRE(executor.queuedCount() == 0);
    auto client = BusClient{bus.address()};

    auto heldCall =
      client.callAsync(state.acquiredName, kInterface, "Hold", ::g_variant_new("()"), G_VARIANT_TYPE("()"));
    executor.checkQueued();
    CHECK_FALSE(heldCall.isReady());
    state.admitted = false;

    auto const retirementStart = std::chrono::steady_clock::now();
    sessionPtr->retire();
    sessionPtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    CHECK(retirementElapsed < std::chrono::seconds{2});

    auto heldReply = awaitCallWithoutOwnerProgress(heldCall);
    REQUIRE_FALSE(heldReply);
    CHECK_FALSE(heldReply.isTimeout());
    CHECK(heldReply.remoteErrorName() == "org.freedesktop.DBus.Error.NoReply");
    CHECK((heldReply.errorMessage().contains("disconnected") || heldReply.errorMessage().contains("closed")));
    CHECK(state.handledCount == 0);

    SECTION("late owner drain is inert")
    {
      executor.drain();
      CHECK(state.handledCount == 0);
      CHECK(state.lateDropCount == 1);
    }

    SECTION("owner queue destruction releases invocations without executing them")
    {
      executorOwnerPtr.reset();
      CHECK(state.handledCount == 0);
      CHECK(state.lateDropCount == 0);
    }
  }

  TEST_CASE("MprisBusSession private bus - rejected owner admission completes an owned invocation",
            "[platform][integration][mpris][concurrency]")
  {
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto admission = DroppingExecutor{executor};
    auto state = SessionState{};
    auto session =
      MprisBusSession{admission, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor)};
    awaitAcquisition(state, executor);
    auto client = BusClient{bus.address()};

    admission.setDropping(true);
    auto rejected = client.callAsync(
      state.acquiredName, kInterface, "Ping", ::g_variant_new("(s)", "rejected"), G_VARIANT_TYPE("(s)"));
    auto rejectedReply = awaitCallWithoutOwnerProgress(rejected);
    REQUIRE_FALSE(rejectedReply);
    CHECK_FALSE(rejectedReply.isTimeout());
    CHECK(rejectedReply.remoteErrorName() == "org.freedesktop.DBus.Error.Failed");
    CHECK(state.handledCount == 0);

    admission.setDropping(false);
    auto accepted = client.callAsync(
      state.acquiredName, kInterface, "Ping", ::g_variant_new("(s)", "still live"), G_VARIANT_TYPE("(s)"));
    auto acceptedReply = awaitCall(accepted, executor);
    checkPingReply(acceptedReply, "still live");
    CHECK(state.handledCount == 1);
  }

  TEST_CASE("MprisBusSession private bus - pending authentication cancellation joins without owner progress",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = StalledAuthenticationEndpoint{};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    auto const accepted = endpoint.tryWaitForAcceptance();
    auto rescue = DeadlineRescue{std::chrono::seconds{3}, [&endpoint] { endpoint.closePeerAndListener(); }};

    auto const retirementStart = std::chrono::steady_clock::now();
    sessionPtr->retire();
    sessionPtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    rescue.stopAndJoin();
    endpoint.closePeerAndListener();

    REQUIRE(accepted);
    CHECK_FALSE(rescue.hasFired());
    CHECK(retirementElapsed < std::chrono::seconds{2});
    executor.drain();
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.empty());
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - authentication EOF degrades without an empty native error",
            "[platform][integration][mpris]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::CloseAuthentication};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

    sessionPtr.reset();
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.contains("closed during authentication"));
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - rejected EXTERNAL authentication closes its owned stream",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::RejectAuthentication};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));
    auto rescue = DeadlineRescue{std::chrono::seconds{3}, [&endpoint] { endpoint.closePeerAndListener(); }};

    sessionPtr.reset();
    auto const peerClosed = endpoint.tryWaitForPeerClose();
    rescue.stopAndJoin();

    CHECK_FALSE(rescue.hasFired());
    REQUIRE(peerClosed);
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.contains("rejected EXTERNAL authentication"));
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - malformed authentication closes its owned stream",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::MalformedAuthentication};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

    sessionPtr.reset();
    REQUIRE(endpoint.tryWaitForPeerClose());
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.contains("Invalid D-Bus authentication byte"));
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - oversized authentication closes its owned stream",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::OversizedAuthentication};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

    sessionPtr.reset();
    REQUIRE(endpoint.tryWaitForPeerClose());
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.contains("authentication line is too long"));
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - cancellation interrupts a stalled EXTERNAL response",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::StallExternalResponse};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    auto rescue = DeadlineRescue{std::chrono::seconds{3}, [&endpoint] { endpoint.closePeerAndListener(); }};

    auto const retirementStart = std::chrono::steady_clock::now();
    sessionPtr->retire();
    sessionPtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    auto const peerClosed = endpoint.tryWaitForPeerClose();
    rescue.stopAndJoin();

    CHECK_FALSE(rescue.hasFired());
    REQUIRE(peerClosed);
    CHECK(endpoint.error().empty());
    CHECK(retirementElapsed < std::chrono::seconds{2});
    executor.drain();
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.empty());
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - authenticated GUID must match the selected address",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::MismatchedGuid};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

    sessionPtr.reset();
    REQUIRE(endpoint.tryWaitForPeerClose());
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.contains("GUID does not match"));
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - immediate post-BEGIN disconnect degrades without acquisition",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::DropHello};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

    sessionPtr.reset();
    CHECK(endpoint.error().empty());
    CHECK(state.acquiredName.empty());
    CHECK_FALSE(state.unavailableMessage.empty());
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession private bus - empty DATA payload and stalled Hello cancel without owner progress",
            "[platform][integration][mpris][concurrency]")
  {
    auto endpoint = ScriptedAuthenticationEndpoint{ScriptedAuthenticationEndpoint::Mode::StallHello};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, endpoint.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    REQUIRE(endpoint.tryWaitForProtocolPoint());
    auto rescue = DeadlineRescue{std::chrono::seconds{3}, [&endpoint] { endpoint.closePeerAndListener(); }};

    auto const retirementStart = std::chrono::steady_clock::now();
    sessionPtr->retire();
    sessionPtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    auto const peerClosed = endpoint.tryWaitForPeerClose();
    rescue.stopAndJoin();

    CHECK_FALSE(rescue.hasFired());
    REQUIRE(peerClosed);
    CHECK(endpoint.error().empty());
    CHECK(retirementElapsed < std::chrono::seconds{2});
    executor.drain();
    CHECK(state.acquiredName.empty());
    CHECK(state.unavailableMessage.empty());
    CHECK(state.handledCount == 0);
  }

  TEST_CASE("MprisBusSession unsupported transport - non-local addresses degrade before connecting",
            "[platform][unit][mpris]")
  {
    constexpr auto kAddresses = std::array{
      "tcp:host=example.invalid,port=1234",
      "unix:path=/tmp/aobus-never,uid=0",
      "unix:path=/tmp/aobus-never,abstract=duplicate",
      "unix:path=/tmp/aobus-never,guid=not-a-guid",
      "unix:path=/tmp/aobus-never,guid=0123456789abcdef0123456789abcdef,guid=fedcba9876543210fedcba9876543210",
      "unix:path=/tmp/aobus-never,",
      "unix:path=/tmp/aobus-never;autolaunch:",
    };

    for (auto const* const address : kAddresses)
    {
      INFO(address);
      auto executor = rt::test::QueuedExecutor{};
      auto state = SessionState{};
      auto sessionPtr = std::make_unique<MprisBusSession>(
        executor, address, kBusName, false, testNodeInfo(), pingOutputs(state, executor));
      REQUIRE(executor.tryDrainUntil([&state] { return !state.unavailableMessage.empty(); }));

      sessionPtr.reset();
      CHECK(state.acquiredName.empty());
      CHECK(state.unavailableMessage.contains("Only unix:path and unix:abstract"));
      CHECK(state.handledCount == 0);
    }
  }

  TEST_CASE("MprisBusSession private bus - stalled queued write uses bounded flush deadline",
            "[platform][integration][mpris][concurrency]")
  {
    constexpr auto kPayloadBytes = std::size_t{16} * 1024 * 1024;
    auto bus = PrivateBus{};
    auto executor = rt::test::QueuedExecutor{};
    auto state = SessionState{};
    auto sessionPtr = std::make_unique<MprisBusSession>(
      executor, bus.address(), kBusName, false, testNodeInfo(), pingOutputs(state, executor));
    awaitAcquisition(state, executor);
    REQUIRE(state.acquiredConnectionPtr);

    bus.stall();
    auto rescueResumeFailed = std::atomic_bool{false};
    auto rescue = DeadlineRescue{std::chrono::seconds{3},
                                 [&bus, &rescueResumeFailed]
                                 {
                                   try
                                   {
                                     bus.resume();
                                   }
                                   catch (...)
                                   {
                                     rescueResumeFailed.store(true);
                                   }
                                 }};
    auto payload = std::string(kPayloadBytes, 'x');
    ::GError* emitError = nullptr;
    auto const emitted = ::g_dbus_connection_emit_signal(state.acquiredConnectionPtr->gobj(),
                                                         nullptr,
                                                         "/org/mpris/MediaPlayer2",
                                                         kInterface,
                                                         "Backpressure",
                                                         ::g_variant_new("(s)", payload.c_str()),
                                                         &emitError);
    auto const emitMessage = emitError != nullptr ? std::string{emitError->message} : std::string{};
    ::g_clear_error(&emitError);

    auto const retirementStart = std::chrono::steady_clock::now();
    sessionPtr->retire();
    sessionPtr.reset();
    auto const retirementElapsed = std::chrono::steady_clock::now() - retirementStart;
    rescue.stopAndJoin();
    CHECK(::g_dbus_connection_is_closed(state.acquiredConnectionPtr->gobj()) != 0);
    state.acquiredConnectionPtr.reset();
    bus.resume();

    INFO(emitMessage);
    REQUIRE(emitted == TRUE);
    CHECK_FALSE(rescue.hasFired());
    CHECK_FALSE(rescueResumeFailed.load());
    CHECK(retirementElapsed >= std::chrono::milliseconds{200});
    CHECK(retirementElapsed < std::chrono::seconds{2});
  }
} // namespace ao::media::test
