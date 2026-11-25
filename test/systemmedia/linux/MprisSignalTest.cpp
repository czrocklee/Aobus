// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MprisTestSupport.h"
#include "test/audio/EngineTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackSuccessionTransportTestSupport.h"
#include "test/runtime/PlaybackUiTestSupport.h"
#include <ao/Contract.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/systemmedia/linux/MprisBridge.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gio/gio.h>

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::systemmedia::test
{
  namespace
  {
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

    struct WireEvent final
    {
      bool reply = false;
      bool seeked = false;
      VariantPtr bodyPtr;
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

        auto const type = ::g_dbus_message_get_message_type(message);
        auto const reply = type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN;

        auto const seeked = type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
                            ::g_strcmp0(::g_dbus_message_get_interface(message), kPlayerInterface) == 0 &&
                            ::g_strcmp0(::g_dbus_message_get_member(message), "Seeked") == 0;

        if (!reply && !seeked &&
            (type != G_DBUS_MESSAGE_TYPE_SIGNAL ||
             ::g_strcmp0(::g_dbus_message_get_interface(message), kPropertiesInterface) != 0 ||
             ::g_strcmp0(::g_dbus_message_get_member(message), "PropertiesChanged") != 0))
        {
          return message;
        }

        auto* const body = ::g_dbus_message_get_body(message);
        auto event = WireEvent{
          .reply = reply, .seeked = seeked, .bodyPtr = takeVariant(body != nullptr ? ::g_variant_ref(body) : nullptr)};
        auto const publicationLock = std::scoped_lock{wirePublicationMutex()};
        auto const& logPtr = *static_cast<std::shared_ptr<WireLog>*>(data);
        auto const lock = std::scoped_lock{logPtr->mutex};
        logPtr->events.push_back(std::move(event));
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "MPRIS wire observer");
      }

      return message;
    }

    VariantPtr requireSingleChangedProperty(WireEvent const& event, char const* name, ::GVariantType const* type)
    {
      REQUIRE_FALSE(event.reply);
      REQUIRE(event.bodyPtr);
      REQUIRE(::g_variant_is_of_type(event.bodyPtr.get(), G_VARIANT_TYPE("(sa{sv}as)")) == TRUE);
      auto const interfacePtr = takeVariant(::g_variant_get_child_value(event.bodyPtr.get(), 0));
      CHECK(std::string_view{::g_variant_get_string(interfacePtr.get(), nullptr)} == kPlayerInterface);
      auto const fieldsPtr = takeVariant(::g_variant_get_child_value(event.bodyPtr.get(), 1));
      REQUIRE(::g_variant_n_children(fieldsPtr.get()) == 1);
      auto propertyPtr = takeVariant(::g_variant_lookup_value(fieldsPtr.get(), name, type));
      REQUIRE(propertyPtr);
      return propertyPtr;
    }

    VariantPtr readPlayerProperty(BusClient& client,
                                  std::string const& destination,
                                  rt::test::QueuedExecutor& executor,
                                  char const* name,
                                  ::GVariantType const* type)
    {
      auto query = client.callAsync(destination,
                                    kPropertiesInterface,
                                    "Get",
                                    ::g_variant_new("(ss)", kPlayerInterface, name),
                                    G_VARIANT_TYPE("(v)"));
      auto const reply = awaitCall(query, executor);
      REQUIRE(reply);
      auto const boxPtr = takeVariant(::g_variant_get_child_value(reply.value(), 0));
      auto valuePtr = takeVariant(::g_variant_get_variant(boxPtr.get()));
      REQUIRE(::g_variant_is_of_type(valuePtr.get(), type) == TRUE);
      return valuePtr;
    }

    utility::ScopedRegistration recordPlayerWire(BusClient& client,
                                                 std::string const& destination,
                                                 std::shared_ptr<WireLog> const& logPtr)
    {
      auto const rule = "type='signal',sender='" + destination + "',path='/org/mpris/MediaPlayer2'";
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
      return utility::ScopedRegistration{[&client, filterId]
                                         { ::g_dbus_connection_remove_filter(client.nativeConnection(), filterId); }};
    }
  } // namespace

  TEST_CASE("MprisBridge private bus - changed properties precede a later live query on the wire", "[concurrency]")
  {
    auto bus = PrivateBus{};
    auto fixture = rt::test::PlaybackUiFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto actions =
      uimodel::PlaybackActions{playback, [&runtime] { return runtime.playSelectionInFocusedView().has_value(); }};
    actions.bindFocusedSelection(runtime.workspace(), runtime.views());
    auto bridge = MprisBridge{
      *fixture.executor,
      playback,
      actions,
      {},
      MprisBridgeOptions{.busName = "org.mpris.MediaPlayer2.aobus.signals", .optBusAddress = bus.address()}};
    // Acquisition must seed its signal baseline from live state, not construction state.
    fixture.makePlaybackReady();
    REQUIRE(playback.snapshot().transport.ready);
    bridge.start();
    REQUIRE(fixture.executor->tryDrainUntil([&bridge] { return bridge.isActive(); }));
    auto const destination = std::string{bridge.busName()};
    auto client = BusClient{bus.address()};
    auto logPtr = std::make_shared<WireLog>();
    auto const registration = recordPlayerWire(client, destination, logPtr);

    // The filter sees each reply before the call completes, so Get also
    // establishes that all earlier signals from this connection arrived.
    auto const getProperty = [&](char const* name, ::GVariantType const* type)
    { return readPlayerProperty(client, destination, *fixture.executor, name, type); };

    SECTION("Volume changes arrive in order")
    {
      playback.commands().setVolume(0.25F);
      REQUIRE(
        fixture.executor->tryDrainUntil([&playback] { return playback.snapshot().transport.volume.level == 0.25F; }));
      playback.commands().setVolume(0.75F);
      REQUIRE(
        fixture.executor->tryDrainUntil([&playback] { return playback.snapshot().transport.volume.level == 0.75F; }));
      auto const volumePtr = getProperty("Volume", G_VARIANT_TYPE_DOUBLE);
      CHECK(::g_variant_get_double(volumePtr.get()) == Catch::Approx{0.75});

      auto const lock = std::scoped_lock{logPtr->mutex};
      REQUIRE(logPtr->events.size() == 3);
      auto const firstPtr = requireSingleChangedProperty(logPtr->events[0], "Volume", G_VARIANT_TYPE_DOUBLE);
      auto const secondPtr = requireSingleChangedProperty(logPtr->events[1], "Volume", G_VARIANT_TYPE_DOUBLE);
      CHECK(::g_variant_get_double(firstPtr.get()) == Catch::Approx{0.25});
      CHECK(::g_variant_get_double(secondPtr.get()) == Catch::Approx{0.75});
      CHECK(logPtr->events[2].reply);
    }

    SECTION("CanPlay follows an actionable focused selection")
    {
      auto const trackId = fixture.addPlayableTrack("Selected Track");
      REQUIRE(runtime.views().setSelection(fixture.viewId, {trackId}));
      auto const availablePtr = getProperty("CanPlay", G_VARIANT_TYPE_BOOLEAN);
      CHECK(::g_variant_get_boolean(availablePtr.get()) == TRUE);

      SECTION("Clearing only the selection removes CanPlay")
      {
        REQUIRE(runtime.views().setSelection(fixture.viewId, {}));
        CHECK(playback.snapshot().transport.ready);
      }

      SECTION("Losing output removes CanPlay even with the selection")
      {
        playback.commands().setOutputDevice(audio::OutputDeviceSelection{.backendId = audio::BackendId{"usb"},
                                                                         .deviceId = audio::DeviceId{"missing"},
                                                                         .profileId = audio::kProfileShared});
        REQUIRE(fixture.executor->tryDrainUntil([&playback] { return !playback.snapshot().transport.ready; }));
      }

      auto const unavailablePtr = getProperty("CanPlay", G_VARIANT_TYPE_BOOLEAN);
      CHECK(::g_variant_get_boolean(unavailablePtr.get()) == FALSE);

      auto const lock = std::scoped_lock{logPtr->mutex};
      REQUIRE(logPtr->events.size() == 4);
      auto const firstPtr = requireSingleChangedProperty(logPtr->events[0], "CanPlay", G_VARIANT_TYPE_BOOLEAN);
      auto const secondPtr = requireSingleChangedProperty(logPtr->events[2], "CanPlay", G_VARIANT_TYPE_BOOLEAN);
      CHECK(::g_variant_get_boolean(firstPtr.get()) == TRUE);
      CHECK(logPtr->events[1].reply);
      CHECK(::g_variant_get_boolean(secondPtr.get()) == FALSE);
      CHECK(logPtr->events[3].reply);
    }
  }

  TEST_CASE("MprisBridge private bus - deferred positioning holds the clock but preserves play intent", "[concurrency]")
  {
    constexpr auto kOffset = std::chrono::milliseconds{100};
    auto const gatePtr = std::make_shared<audio::test::SeekTestGate>();
    auto fixture = rt::test::playback_succession::PlaybackSuccessionTransportFixture{
      {.seekGatePtr = gatePtr, .gatedSeekOffset = kOffset}};
    auto const trackId = fixture.addPlayableTrack("Buffered Track");
    fixture.openManualView(std::array{trackId});
    auto playback = fixture.createPlayback();
    REQUIRE(fixture.playAndWait(trackId));
    auto& executor = fixture.transport.executor;
    executor.drain();
    auto actions = uimodel::PlaybackActions{playback, {}};
    auto bus = PrivateBus{};
    auto bridge = MprisBridge{executor, playback, actions, {}, MprisBridgeOptions{.optBusAddress = bus.address()}};
    bridge.start();
    REQUIRE(executor.tryDrainUntil([&bridge] { return bridge.isActive(); }));
    auto const destination = std::string{bridge.busName()};
    auto client = BusClient{bus.address()};
    auto logPtr = std::make_shared<WireLog>();
    auto const registration = recordPlayerWire(client, destination, logPtr);

    REQUIRE(playback.commands().trySeek(playback.snapshot().transport.occurrenceId, kOffset));
    REQUIRE(gatePtr->tryWaitForEntry());
    auto const command = [&](char const* method)
    {
      auto call = client.callAsync(destination, kPlayerInterface, method, nullptr, G_VARIANT_TYPE("()"));
      REQUIRE(awaitCall(call, executor));
    };
    auto const heldPositionPtr = readPlayerProperty(client, destination, executor, "Position", G_VARIANT_TYPE_INT64);
    CHECK(::g_variant_get_int64(heldPositionPtr.get()) == 100'000);
    {
      // Get fences the seek publication while the decoder is still held.
      auto const lock = std::scoped_lock{logPtr->mutex};
      REQUIRE(logPtr->events.size() == 3);
      auto const heldPtr = requireSingleChangedProperty(logPtr->events[0], "PlaybackStatus", G_VARIANT_TYPE_STRING);
      CHECK(std::string_view{::g_variant_get_string(heldPtr.get(), nullptr)} == "Paused");
      REQUIRE(logPtr->events[1].seeked);
      auto const& seekedPtr = logPtr->events[1].bodyPtr;
      REQUIRE(seekedPtr);
      REQUIRE(::g_variant_is_of_type(seekedPtr.get(), G_VARIANT_TYPE("(x)")) == TRUE);
      auto const anchorPtr = takeVariant(::g_variant_get_child_value(seekedPtr.get(), 0));
      CHECK(::g_variant_get_int64(anchorPtr.get()) == 100'000);
      CHECK(logPtr->events[2].reply);
      logPtr->events.clear();
    }
    auto const canPausePtr = readPlayerProperty(client, destination, executor, "CanPause", G_VARIANT_TYPE_BOOLEAN);
    CHECK(::g_variant_get_boolean(canPausePtr.get()) == TRUE);

    SECTION("Toggling the advertised Paused state lets pending playback start")
    {
      command("PlayPause");
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Buffering);
    }

    SECTION("Explicit Pause still pauses, and Play or PlayPause can resume")
    {
      command("Pause");
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Paused);
      command("Play");
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Buffering);
      command("Pause");
      command("PlayPause");
      REQUIRE(playback.snapshot().transport.transport == audio::Transport::Buffering);
    }

    gatePtr->release();
    REQUIRE(
      executor.tryDrainUntil([&] { return playback.snapshot().transport.transport == audio::Transport::Playing; }));
    // This reply fences every signal sent during positioning and restart.
    auto const statusPtr = readPlayerProperty(client, destination, executor, "PlaybackStatus", G_VARIANT_TYPE_STRING);
    CHECK(std::string_view{::g_variant_get_string(statusPtr.get(), nullptr)} == "Playing");
    auto const lock = std::scoped_lock{logPtr->mutex};
    std::erase_if(logPtr->events, [](auto const& event) { return event.reply; });
    REQUIRE(logPtr->events.size() == 1);
    auto const runningPtr =
      requireSingleChangedProperty(logPtr->events.front(), "PlaybackStatus", G_VARIANT_TYPE_STRING);
    CHECK(std::string_view{::g_variant_get_string(runningPtr.get(), nullptr)} == "Playing");
  }
} // namespace ao::systemmedia::test
