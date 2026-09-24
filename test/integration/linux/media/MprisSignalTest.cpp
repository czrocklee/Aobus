// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisTestSupport.h"
#include "media/linux/MprisBridge.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackUiTestSupport.h"
#include <ao/Contract.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gio/gio.h>

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace ao::media::test
{
  namespace
  {
    struct WireEvent final
    {
      bool reply = false;
      bool valid = false;
      double volume = 0;
    };

    struct WireLog final
    {
      std::mutex mutex;
      std::vector<WireEvent> events;
    };

    std::mutex& wirePublicationMutex()
    {
      // Constant initialization precedes GIO's uninstrumented transport thread.
      static constinit auto mutex = std::mutex{};
      return mutex;
    }

    ::GDBusMessage* observeWire(::GDBusConnection* /*connection*/,
                                ::GDBusMessage* message,
                                ::gboolean incoming,
                                void* data) noexcept
    {
      try
      {
        if (incoming == FALSE)
        {
          return message;
        }

        auto event = WireEvent{};
        auto* const body = ::g_dbus_message_get_body(message);

        if (auto const type = ::g_dbus_message_get_message_type(message); type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
        {
          event.reply = true;
          event.valid = body != nullptr && ::g_variant_is_of_type(body, G_VARIANT_TYPE("(v)")) == TRUE;
        }
        else if (type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
                 ::g_strcmp0(::g_dbus_message_get_member(message), "PropertiesChanged") == 0)
        {
          event.valid = body != nullptr && ::g_variant_is_of_type(body, G_VARIANT_TYPE("(sa{sv}as)")) == TRUE;

          if (event.valid)
          {
            auto fieldsPtr = takeVariant(::g_variant_get_child_value(body, 1));
            auto volumePtr = takeVariant(::g_variant_lookup_value(fieldsPtr.get(), "Volume", G_VARIANT_TYPE_DOUBLE));

            if (!volumePtr)
            {
              return message;
            }

            event.volume = ::g_variant_get_double(volumePtr.get());
          }
        }
        else
        {
          return message;
        }

        auto const publicationLock = std::scoped_lock{wirePublicationMutex()};
        auto const& logPtr = *static_cast<std::shared_ptr<WireLog>*>(data);
        auto const lock = std::scoped_lock{logPtr->mutex};
        logPtr->events.push_back(event);
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "MPRIS wire observer");
      }

      return message;
    }
  } // namespace

  TEST_CASE("MprisBridge private bus - changed properties precede a later live query on the wire",
            "[platform][integration][mpris][concurrency]")
  {
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    fixture.makePlaybackReady();
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto bridge = MprisBridge{
      *fixture.executor,
      playback,
      actions,
      {},
      MprisBridgeOptions{.busName = "org.mpris.MediaPlayer2.aobus.signals", .optBusAddress = bus.address()}};
    bridge.start();
    REQUIRE(fixture.executor->tryDrainUntil([&bridge] { return bridge.isActive(); }));
    auto const destination = std::string{bridge.busName()};
    auto client = BusClient{bus.address()};
    auto const rule = "type='signal',sender='" + destination + "',interface='org.freedesktop.DBus.Properties'";
    ::GError* error = nullptr;
    auto matchReply = CallResult{::g_dbus_connection_call_sync(client.nativeConnection(),
                                                               "org.freedesktop.DBus",
                                                               "/org/freedesktop/DBus",
                                                               "org.freedesktop.DBus",
                                                               "AddMatch",
                                                               ::g_variant_new("(s)", rule.c_str()),
                                                               G_VARIANT_TYPE("()"),
                                                               G_DBUS_CALL_FLAGS_NONE,
                                                               2000,
                                                               nullptr,
                                                               &error),
                                 error};
    REQUIRE(matchReply);

    // Observe on GIO's transport worker, not by pumping any frontend GLib
    // context. The filter owns its state until all in-flight callbacks finish.
    auto logPtr = std::make_shared<WireLog>();
    // Explicitly publish C++ state across the uninstrumented GIO boundary.
    auto filterDataPtr = std::unique_ptr<std::shared_ptr<WireLog>>{};

    {
      auto const lock = std::scoped_lock{wirePublicationMutex()};
      filterDataPtr = std::make_unique<std::shared_ptr<WireLog>>(logPtr);
    }

    auto const filterId = ::g_dbus_connection_add_filter(
      client.nativeConnection(),
      observeWire,
      filterDataPtr.get(),
      [](void* data)
      {
        auto const lock = std::scoped_lock{wirePublicationMutex()};
        auto const ownedPtr = std::unique_ptr<std::shared_ptr<WireLog>>{static_cast<std::shared_ptr<WireLog>*>(data)};
      });
    REQUIRE(filterId != 0);
    std::ignore = filterDataPtr.release();
    auto const registration = utility::ScopedRegistration{
      [&client, filterId] { ::g_dbus_connection_remove_filter(client.nativeConnection(), filterId); }};

    playback.commands().setVolume(0.25F);
    REQUIRE(
      fixture.executor->tryDrainUntil([&playback] { return playback.snapshot().transport.volume.level == 0.25F; }));
    playback.commands().setVolume(0.75F);
    REQUIRE(
      fixture.executor->tryDrainUntil([&playback] { return playback.snapshot().transport.volume.level == 0.75F; }));
    auto query = client.callAsync(destination,
                                  "org.freedesktop.DBus.Properties",
                                  "Get",
                                  ::g_variant_new("(ss)", kPlayerInterface, "Volume"),
                                  G_VARIANT_TYPE("(v)"));
    auto reply = awaitCall(query, *fixture.executor);
    REQUIRE(reply);
    auto boxPtr = takeVariant(::g_variant_get_child_value(reply.value(), 0));
    auto valuePtr = takeVariant(::g_variant_get_variant(boxPtr.get()));
    REQUIRE(::g_variant_is_of_type(valuePtr.get(), G_VARIANT_TYPE_DOUBLE) == TRUE);
    CHECK(::g_variant_get_double(valuePtr.get()) == Catch::Approx{0.75});

    // The filter sees the reply before the client call completes. All earlier
    // signals from this connection must already be present; no settling sleep.
    auto const lock = std::scoped_lock{logPtr->mutex};
    REQUIRE(logPtr->events.size() == 3);
    CHECK(logPtr->events[0].valid);
    CHECK_FALSE(logPtr->events[0].reply);
    CHECK(logPtr->events[0].volume == Catch::Approx{0.25});
    CHECK(logPtr->events[1].valid);
    CHECK_FALSE(logPtr->events[1].reply);
    CHECK(logPtr->events[1].volume == Catch::Approx{0.75});
    CHECK(logPtr->events[2].valid);
    CHECK(logPtr->events[2].reply);
  }
} // namespace ao::media::test
