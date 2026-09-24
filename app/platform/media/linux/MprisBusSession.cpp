// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisBusSession.h"

#include <ao/Contract.h>
#include <ao/async/Executor.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <gio/gunixconnection.h>
#include <giomm/cancellable.h>
#include <giomm/dbusconnection.h>
#include <giomm/dbusintrospection.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::media
{
  namespace
  {
    constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
    constexpr auto kBusService = "org.freedesktop.DBus";
    constexpr auto kBusObjectPath = "/org/freedesktop/DBus";
    constexpr auto kBusInterface = "org.freedesktop.DBus";
    constexpr auto kAbandonedRequestError = "MPRIS request was abandoned before completion";
    constexpr unsigned int kFlushTimeoutMs = 250;
    constexpr std::size_t kMaximumAuthLineLength = 1024;
    constexpr ::guint32 kDoNotQueue = 4;
    constexpr ::guint32 kPrimaryOwner = 1;

    struct ContextDeleter final
    {
      void operator()(::GMainContext* context) const noexcept { ::g_main_context_unref(context); }
    };

    struct SourceDeleter final
    {
      void operator()(::GSource* source) const noexcept
      {
        ::g_source_destroy(source);
        ::g_source_unref(source);
      }
    };

    template<typename Callback>
    void runNativeCallback(Callback&& callback) noexcept
    {
      try
      {
        std::forward<Callback>(callback)();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "MPRIS native callback");
      }
    }

    bool isSupportedUnixAddressEntry(std::string_view const entry)
    {
      if (!entry.starts_with("unix:"))
      {
        return false;
      }

      auto fields = entry.substr(std::string_view{"unix:"}.size());
      bool hasTransport = false;
      bool hasGuid = false;

      while (!fields.empty())
      {
        auto const fieldSeparator = fields.find(',');
        auto const field = fields.substr(0, fieldSeparator);
        auto const assignment = field.find('=');

        if (assignment == std::string_view::npos || assignment == 0 || assignment + 1 == field.size())
        {
          return false;
        }

        if (auto const key = field.substr(0, assignment); key == "path" || key == "abstract")
        {
          if (std::exchange(hasTransport, true))
          {
            return false;
          }
        }
        else if (key == "guid")
        {
          if (auto const value = std::string{field.substr(assignment + 1)};
              std::exchange(hasGuid, true) || ::g_dbus_is_guid(value.c_str()) == 0)
          {
            return false;
          }
        }
        else
        {
          return false;
        }

        if (fieldSeparator == std::string_view::npos)
        {
          fields = {};
        }
        else if (fieldSeparator + 1 == fields.size())
        {
          return false;
        }
        else
        {
          fields.remove_prefix(fieldSeparator + 1);
        }
      }

      return hasTransport;
    }

    bool isSupportedBusAddress(std::string const& address)
    {
      if (::g_dbus_is_address(address.c_str()) == 0)
      {
        return false;
      }

      auto remaining = std::string_view{address};

      while (!remaining.empty())
      {
        auto const separator = remaining.find(';');

        if (!isSupportedUnixAddressEntry(remaining.substr(0, separator)))
        {
          return false;
        }

        if (separator == std::string_view::npos)
        {
          return true;
        }

        remaining.remove_prefix(separator + 1);
      }

      return false;
    }

    bool tryWriteAuth(::GIOStream* stream, std::string_view const text, ::GCancellable* cancellable, ::GError** error)
    {
      ::gsize written = 0;
      auto const succeeded = ::g_output_stream_write_all(
        ::g_io_stream_get_output_stream(stream), text.data(), text.size(), &written, cancellable, error);
      return succeeded != 0 && written == text.size();
    }

    bool tryReadAuthLine(::GIOStream* stream, ::GCancellable* cancellable, std::string& line, ::GError** error)
    {
      line.clear();
      auto* const input = ::g_io_stream_get_input_stream(stream);

      while (line.size() != kMaximumAuthLineLength)
      {
        char character = 0;
        auto const count = ::g_input_stream_read(input, &character, 1, cancellable, error);

        if (count < 0)
        {
          return false;
        }

        if (count == 0)
        {
          ::g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Session bus closed during authentication");
          return false;
        }

        if (character == '\n')
        {
          if (line.empty() || line.back() != '\r')
          {
            ::g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid D-Bus authentication line");
            return false;
          }

          line.pop_back();
          return true;
        }

        if (unsigned char const byte = static_cast<unsigned char>(character);
            (!line.empty() && line.back() == '\r') || (byte < static_cast<unsigned char>(' ') && character != '\r') ||
            byte > static_cast<unsigned char>('~'))
        {
          ::g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid D-Bus authentication byte");
          return false;
        }

        line.push_back(character);
      }

      ::g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "D-Bus authentication line is too long");
      return false;
    }

    bool tryAuthenticateExternal(::GIOStream* stream,
                                 ::GCancellable* cancellable,
                                 std::string& serverGuid,
                                 ::GError** error)
    {
      if (!G_IS_UNIX_CONNECTION(stream))
      {
        ::g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only Unix session-bus transports are supported");
        return false;
      }

      if (::g_unix_connection_send_credentials(G_UNIX_CONNECTION(stream), cancellable, error) == 0 ||
          !tryWriteAuth(stream, "AUTH EXTERNAL\r\n", cancellable, error))
      {
        return false;
      }

      auto response = std::string{};

      if (!tryReadAuthLine(stream, cancellable, response, error))
      {
        return false;
      }

      if (response == "DATA" || response == "DATA ")
      {
        if (!tryWriteAuth(stream, "DATA\r\n", cancellable, error) ||
            !tryReadAuthLine(stream, cancellable, response, error))
        {
          return false;
        }
      }

      auto const* const guid = response.starts_with("OK ") ? response.c_str() + 3 : "";

      if (*guid == '\0' || ::g_dbus_is_guid(guid) == 0)
      {
        ::g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Session bus rejected EXTERNAL authentication");
        return false;
      }

      serverGuid = guid;
      return tryWriteAuth(stream, "BEGIN\r\n", cancellable, error);
    }

    std::string nativeErrorMessage(::GError const* error, std::string_view const fallback)
    {
      return error != nullptr ? std::string{error->message} : std::string{fallback};
    }
  } // namespace

  struct MprisBusSession::Impl final
  {
    enum class PendingBusCall : std::uint8_t
    {
      None,
      Hello,
      RequestName,
    };

    async::Executor& executor;
    std::string address;
    std::string busName;
    bool uniqueInstance;
    Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoPtr;
    Outputs outputs;
    std::unique_ptr<::GMainContext, ContextDeleter> contextPtr{::g_main_context_new()};
    Glib::RefPtr<Gio::DBus::Connection> connectionPtr;
    Glib::RefPtr<Gio::Cancellable> acquisitionPtr{Gio::Cancellable::create()};
    Glib::RefPtr<Gio::Cancellable> flushCancellationPtr{Gio::Cancellable::create()};
    std::unique_ptr<::GSource, SourceDeleter> flushDeadlinePtr;
    std::vector<::guint> registrationIds;
    std::string uniqueConnectionName;
    PendingBusCall pendingBusCall = PendingBusCall::None;
    ::guint nameLostSubscriptionId = 0;
    ::gulong closedHandlerId = 0;
    std::size_t registeredObjectCount = 0;
    bool nameLostSubscriptionAlive = false;
    bool connectPending = false;
    bool flushPending = false;
    bool closePending = false;
    bool closeStarted = false;
    bool isConnectionClosed = false;
    bool retiring = false;
    bool finished = false;
    std::stop_token stopToken;
    // Declared last: all native callback state exists before publication and
    // remains alive until this producer has joined.
    std::jthread thread;

    Impl(async::Executor& executorRef,
         std::string addressIn,
         std::string busNameIn,
         bool uniqueInstanceIn,
         Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoInPtr,
         Outputs outputsIn)
      : executor{executorRef}
      , address{std::move(addressIn)}
      , busName{std::move(busNameIn)}
      , uniqueInstance{uniqueInstanceIn}
      , nodeInfoPtr{std::move(nodeInfoInPtr)}
      , outputs{std::move(outputsIn)}
    {
      thread = std::jthread{[this](std::stop_token token) { runNativeCallback([&] { run(token); }); }};
    }

    ~Impl()
    {
      thread.request_stop();
      thread.join();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void run(std::stop_token token)
    {
      setCurrentThreadName("ao-mpris");
      stopToken = token;
      ::g_main_context_push_thread_default(contextPtr.get());
      auto const popContext =
        utility::ScopedRegistration{[this] { ::g_main_context_pop_thread_default(contextPtr.get()); }};
      auto stopCallback = std::stop_callback{token,
                                             [this]
                                             {
                                               acquisitionPtr->cancel();
                                               ::g_main_context_wakeup(contextPtr.get());
                                             }};

      if (token.stop_requested())
      {
        beginRetirement();
      }
      else
      {
        connectPending = true;
        ::GError* error = nullptr;
        auto* const connection = acquireConnection(&error);
        auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);
        connectPending = false;

        if (connection != nullptr)
        {
          handleConnection(connection);
        }
        else if (token.stop_requested())
        {
          beginRetirement();
        }
        else
        {
          fail(nativeErrorMessage(error, "Could not connect to the session bus"));
        }
      }

      while (!finished)
      {
        if (token.stop_requested())
        {
          beginRetirement();
        }

        if (!finished)
        {
          std::ignore = ::g_main_context_iteration(contextPtr.get(), TRUE);
        }
      }

      // Successful connections use an internal transport worker. Drain sources
      // it queued before the close/closed settlement gate completed.
      while (::g_main_context_iteration(contextPtr.get(), FALSE) != 0)
      {
      }
    }

    ::GDBusConnection* acquireConnection(::GError** error)
    {
      if (!isSupportedBusAddress(address))
      {
        ::g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only unix:path and unix:abstract session buses are supported");
        return nullptr;
      }

      ::gchar* expectedGuid = nullptr;
      auto* const stream =
        ::g_dbus_address_get_stream_sync(address.c_str(), &expectedGuid, acquisitionPtr->gobj(), error);
      auto const expectedGuidPtr = utility::makeUniquePtr<::g_free>(expectedGuid);

      if (stream == nullptr)
      {
        return nullptr;
      }

      auto const streamPtr = utility::makeUniquePtr<::g_object_unref>(stream);
      auto serverGuid = std::string{};

      // Keep the stream exclusively owned here until every blocking authentication operation has honored cancellation.
      if (!tryAuthenticateExternal(stream, acquisitionPtr->gobj(), serverGuid, error))
      {
        std::ignore = ::g_io_stream_close(stream, nullptr, nullptr);
        return nullptr;
      }

      if (expectedGuid != nullptr && serverGuid != expectedGuid)
      {
        std::ignore = ::g_io_stream_close(stream, nullptr, nullptr);
        ::g_set_error_literal(
          error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Session-bus GUID does not match its address");
        return nullptr;
      }

      if (::g_cancellable_is_cancelled(acquisitionPtr->gobj()) != 0)
      {
        std::ignore = ::g_io_stream_close(stream, nullptr, nullptr);
        ::g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation was cancelled");
        return nullptr;
      }

      // The stream is already authenticated and positioned after BEGIN. No
      // constructor flag may repeat authentication or issue an implicit Hello.
      return ::g_dbus_connection_new_sync(stream, nullptr, G_DBUS_CONNECTION_FLAGS_NONE, nullptr, nullptr, error);
    }

    void handleConnection(::GDBusConnection* connection)
    {
      ::g_dbus_connection_set_exit_on_close(connection, FALSE);
      connectionPtr = Glib::wrap(connection);

      closedHandlerId = ::g_signal_connect_data(
        connection,
        "closed",
        G_CALLBACK(+[](::GDBusConnection*, ::gboolean, ::GError* error, ::gpointer data)
                   {
                     runNativeCallback(
                       [&]
                       {
                         auto& self = *static_cast<Impl*>(data);
                         self.isConnectionClosed = true;

                         if (!self.retiring)
                         {
                           self.fail(nativeErrorMessage(error, "Session bus connection closed"));
                         }

                         self.tryFinishRetirement();
                       });
                   }),
        this,
        nullptr,
        static_cast<::GConnectFlags>(0));

      isConnectionClosed = ::g_dbus_connection_is_closed(connection) != 0;

      if (stopToken.stop_requested())
      {
        beginRetirement();
      }
      else if (isConnectionClosed)
      {
        fail("Session bus connection already closed");
      }
      else
      {
        startBusCall(PendingBusCall::Hello, "Hello", nullptr);
      }
    }

    void startBusCall(PendingBusCall const call, char const* member, ::GVariant* body)
    {
      AO_INVARIANT(pendingBusCall == PendingBusCall::None, "Only one MPRIS bus setup call may be pending");
      auto const messagePtr = utility::makeUniquePtr<::g_object_unref>(
        ::g_dbus_message_new_method_call(kBusService, kBusObjectPath, kBusInterface, member));

      if (body != nullptr)
      {
        ::g_dbus_message_set_body(messagePtr.get(), body);
      }

      pendingBusCall = call;
      ::g_dbus_connection_send_message_with_reply(
        connectionPtr->gobj(),
        messagePtr.get(),
        G_DBUS_SEND_MESSAGE_FLAGS_NONE,
        -1,
        nullptr,
        acquisitionPtr->gobj(),
        [](::GObject* object, ::GAsyncResult* result, ::gpointer data)
        { runNativeCallback([&] { static_cast<Impl*>(data)->handleBusReply(G_DBUS_CONNECTION(object), result); }); },
        this);
    }

    void handleBusReply(::GDBusConnection* connection, ::GAsyncResult* result)
    {
      ::GError* error = nullptr;
      auto const replyPtr = utility::makeUniquePtr<::g_object_unref>(
        ::g_dbus_connection_send_message_with_reply_finish(connection, result, &error));
      auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);
      auto const completedCall = std::exchange(pendingBusCall, PendingBusCall::None);

      if (retiring || stopToken.stop_requested())
      {
        beginRetirement();
        tryFinishRetirement();
        return;
      }

      if (!replyPtr)
      {
        fail(nativeErrorMessage(error, "Session bus setup call failed"));
        return;
      }

      auto const* const sender = ::g_dbus_message_get_sender(replyPtr.get());

      if (sender == nullptr || std::string_view{sender} != kBusService)
      {
        fail("Invalid session-bus setup reply sender");
        return;
      }

      if (::g_dbus_message_get_message_type(replyPtr.get()) == G_DBUS_MESSAGE_TYPE_ERROR)
      {
        ::GError* remoteError = nullptr;
        std::ignore = ::g_dbus_message_to_gerror(replyPtr.get(), &remoteError);
        auto const remoteErrorPtr = utility::makeUniquePtr<::g_error_free>(remoteError);
        fail(nativeErrorMessage(remoteError, "Session bus rejected a setup call"));
        return;
      }

      if (::g_dbus_message_get_message_type(replyPtr.get()) != G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
      {
        fail("Invalid session-bus setup reply");
        return;
      }

      if (auto* const body = ::g_dbus_message_get_body(replyPtr.get()); completedCall == PendingBusCall::Hello)
      {
        handleHelloReply(body);
      }
      else if (completedCall == PendingBusCall::RequestName)
      {
        handleRequestNameReply(body);
      }
      else
      {
        fail("Unexpected session-bus setup reply");
      }
    }

    void handleHelloReply(::GVariant* body)
    {
      if (body == nullptr || ::g_variant_is_of_type(body, G_VARIANT_TYPE("(s)")) == 0)
      {
        fail("Invalid session-bus Hello reply");
        return;
      }

      auto const uniqueNameValuePtr = utility::makeUniquePtr<::g_variant_unref>(::g_variant_get_child_value(body, 0));
      auto const* const uniqueName = ::g_variant_get_string(uniqueNameValuePtr.get(), nullptr);

      if (uniqueName == nullptr || ::g_dbus_is_unique_name(uniqueName) == 0)
      {
        fail("Session bus did not assign a unique connection name");
        return;
      }

      uniqueConnectionName = uniqueName;

      if (uniqueInstance)
      {
        auto suffix = uniqueConnectionName.substr(1);
        std::ranges::replace(suffix, '.', '_');
        busName += ".instance" + suffix;
      }

      if (::g_dbus_is_name(busName.c_str()) == 0 || ::g_dbus_is_unique_name(busName.c_str()) != 0)
      {
        fail("Invalid MPRIS bus name");
        return;
      }

      if (!tryRegisterObjects())
      {
        return;
      }

      nameLostSubscriptionAlive = true;
      nameLostSubscriptionId = ::g_dbus_connection_signal_subscribe(
        connectionPtr->gobj(),
        nullptr,
        kBusInterface,
        "NameLost",
        kBusObjectPath,
        busName.c_str(),
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](::GDBusConnection*,
           ::gchar const* senderName,
           ::gchar const*,
           ::gchar const*,
           ::gchar const*,
           ::GVariant* parameters,
           ::gpointer data)
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              auto lostName = std::string{};
              auto const validParameters =
                parameters != nullptr && ::g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)")) != 0;

              if (validParameters)
              {
                auto const lostNameValuePtr =
                  utility::makeUniquePtr<::g_variant_unref>(::g_variant_get_child_value(parameters, 0));
                lostName = ::g_variant_get_string(lostNameValuePtr.get(), nullptr);
              }

              auto const genuine =
                senderName != nullptr && std::string_view{senderName} == kBusService && self.busName == lostName;

              if (genuine && !self.retiring && !self.stopToken.stop_requested())
              {
                self.fail("Lost MPRIS bus name " + self.busName);
              }
            });
        },
        this,
        [](::gpointer data)
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              self.nameLostSubscriptionAlive = false;
              self.tryFinishRetirement();
            });
        });

      if (nameLostSubscriptionId == 0)
      {
        nameLostSubscriptionAlive = false;
        fail("Could not observe the MPRIS bus name");
        return;
      }

      auto const requestNameFields =
        std::array{::g_variant_new_string(busName.c_str()), ::g_variant_new_uint32(kDoNotQueue)};
      startBusCall(PendingBusCall::RequestName,
                   "RequestName",
                   ::g_variant_new_tuple(requestNameFields.data(), requestNameFields.size()));
    }

    void handleRequestNameReply(::GVariant* body)
    {
      if (body == nullptr || ::g_variant_is_of_type(body, G_VARIANT_TYPE("(u)")) == 0)
      {
        fail("Invalid session-bus RequestName reply");
        return;
      }

      auto const resultValuePtr = utility::makeUniquePtr<::g_variant_unref>(::g_variant_get_child_value(body, 0));
      ::guint32 const result = ::g_variant_get_uint32(resultValuePtr.get());

      if (result != kPrimaryOwner)
      {
        fail("Could not acquire MPRIS bus name " + busName);
        return;
      }

      executor.defer([callback = outputs.acquired, connectionPtr = connectionPtr, name = busName] mutable
                     { callback(std::move(connectionPtr), std::move(name)); });
    }

    bool tryRegisterObjects()
    {
      // Null property callbacks are intentional: GIO forwards Get/GetAll/Set
      // as invocations. Giomm's omitted property slots install trampolines.
      static constexpr auto kVtable = ::GDBusInterfaceVTable{
        .method_call =
          [](::GDBusConnection*,
             ::gchar const*,
             ::gchar const*,
             ::gchar const*,
             ::gchar const*,
             ::GVariant*,
             ::GDBusMethodInvocation* invocation,
             ::gpointer data)
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              auto invocationPtr = Invocation{invocation};

              if (self.retiring || self.stopToken.stop_requested())
              {
                return;
              }

              self.executor.defer(
                [callback = self.outputs.handleRequest, invocationPtr = std::move(invocationPtr)] mutable
                { callback(std::move(invocationPtr)); });
            });
        },
        .get_property = nullptr,
        .set_property = nullptr,
        .padding = {},
      };

      for (auto* const* interface = nodeInfoPtr->gobj()->interfaces; *interface != nullptr; ++interface)
      {
        ::GError* error = nullptr;
        ++registeredObjectCount;
        auto const registrationId = ::g_dbus_connection_register_object(
          connectionPtr->gobj(),
          kObjectPath,
          *interface,
          &kVtable,
          this,
          [](::gpointer data)
          {
            runNativeCallback(
              [&]
              {
                auto& self = *static_cast<Impl*>(data);
                --self.registeredObjectCount;
                self.tryFinishRetirement();
              });
          },
          &error);
        auto const errorPtr = utility::makeUniquePtr<::g_error_free>(error);

        if (registrationId == 0)
        {
          fail(nativeErrorMessage(error, "Could not register MPRIS interfaces"));
          return false;
        }

        registrationIds.push_back(registrationId);
      }

      return true;
    }

    void fail(std::string message)
    {
      if (retiring)
      {
        return;
      }

      executor.defer([callback = outputs.unavailable, message = std::move(message)] mutable
                     { callback(std::move(message)); });
      beginRetirement();
    }

    void beginRetirement()
    {
      if (std::exchange(retiring, true))
      {
        return;
      }

      acquisitionPtr->cancel();

      if (connectionPtr)
      {
        for (auto const registrationId : registrationIds)
        {
          std::ignore = ::g_dbus_connection_unregister_object(connectionPtr->gobj(), registrationId);
        }

        if (nameLostSubscriptionId != 0)
        {
          ::g_dbus_connection_signal_unsubscribe(connectionPtr->gobj(), std::exchange(nameLostSubscriptionId, 0));
        }
      }

      registrationIds.clear();

      if (!connectionPtr)
      {
        tryFinishRetirement();
        return;
      }

      if (::g_dbus_connection_is_closed(connectionPtr->gobj()) != 0)
      {
        beginClose();
        return;
      }

      flushPending = true;
      flushDeadlinePtr.reset(::g_timeout_source_new(kFlushTimeoutMs));
      ::g_source_set_priority(flushDeadlinePtr.get(), G_PRIORITY_HIGH);
      ::g_source_set_callback(
        flushDeadlinePtr.get(),
        [](::gpointer data) -> ::gboolean
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              self.flushCancellationPtr->cancel();
              self.beginClose();
            });
          return G_SOURCE_REMOVE;
        },
        this,
        nullptr);
      ::g_source_attach(flushDeadlinePtr.get(), contextPtr.get());
      ::g_dbus_connection_flush(
        connectionPtr->gobj(),
        flushCancellationPtr->gobj(),
        [](::GObject* object, ::GAsyncResult* result, ::gpointer data)
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              std::ignore = ::g_dbus_connection_flush_finish(G_DBUS_CONNECTION(object), result, nullptr);
              self.flushPending = false;
              self.flushDeadlinePtr.reset();
              self.beginClose();
              self.tryFinishRetirement();
            });
        },
        this);
    }

    void beginClose()
    {
      if (closeStarted)
      {
        return;
      }

      closeStarted = true;
      closePending = true;
      ::g_dbus_connection_close(
        connectionPtr->gobj(),
        nullptr,
        [](::GObject* object, ::GAsyncResult* result, ::gpointer data)
        {
          runNativeCallback(
            [&]
            {
              auto& self = *static_cast<Impl*>(data);
              std::ignore = ::g_dbus_connection_close_finish(G_DBUS_CONNECTION(object), result, nullptr);
              self.closePending = false;
              self.tryFinishRetirement();
            });
        },
        this);
    }

    void tryFinishRetirement()
    {
      if (!retiring || connectPending || pendingBusCall != PendingBusCall::None || flushPending || closePending)
      {
        return;
      }

      if (connectionPtr && (!closeStarted || !isConnectionClosed))
      {
        return;
      }

      if (nameLostSubscriptionAlive || registeredObjectCount != 0)
      {
        return;
      }

      if (closedHandlerId != 0)
      {
        ::g_signal_handler_disconnect(connectionPtr->gobj(), std::exchange(closedHandlerId, 0));
      }

      finished = true;
    }
  };

  void MprisBusSession::InvocationDeleter::operator()(::GDBusMethodInvocation* invocation) const noexcept
  {
    if (::g_dbus_connection_is_closed(::g_dbus_method_invocation_get_connection(invocation)) != 0)
    {
      ::g_object_unref(invocation);
      return;
    }

    ::g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.Failed", kAbandonedRequestError);
  }

  MprisBusSession::MprisBusSession(async::Executor& executor,
                                   std::string address,
                                   std::string busName,
                                   bool uniqueInstance,
                                   Glib::RefPtr<Gio::DBus::NodeInfo> nodeInfoPtr,
                                   Outputs outputs)
    : _implPtr{std::make_unique<Impl>(executor,
                                      std::move(address),
                                      std::move(busName),
                                      uniqueInstance,
                                      std::move(nodeInfoPtr),
                                      std::move(outputs))}
  {
  }

  MprisBusSession::~MprisBusSession() = default;

  void MprisBusSession::retire() noexcept
  {
    _implPtr->thread.request_stop();
  }
} // namespace ao::media
