// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MprisBridge.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <giomm/dbusconnection.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>

namespace ao::gtk::platform::test
{
  namespace
  {
    constexpr char const* kBusName = "org.mpris.MediaPlayer2.aobus";

    Glib::VariantContainerBase callBus(Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr,
                                       Glib::ustring const& method,
                                       Glib::VariantContainerBase const& parameters)
    {
      return connectionPtr->call_sync(
        "/org/freedesktop/DBus", "org.freedesktop.DBus", method, parameters, "org.freedesktop.DBus", 5'000);
    }

    bool hasMprisOwner(Glib::RefPtr<Gio::DBus::Connection> const& connectionPtr)
    {
      auto const reply =
        callBus(connectionPtr, "NameHasOwner", Glib::Variant<std::tuple<Glib::ustring>>::create({kBusName}));
      auto result = Glib::Variant<bool>{};
      reply.get_child(result, 0);
      return result.get();
    }
  } // namespace

  TEST_CASE("MprisBridge - failed acquisition neither revokes nor inherits the current bus owner",
            "[gtk][integration][mpris][async][concurrency]")
  {
    ao::gtk::test::requireOwnedGtkSessionBus();
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto const connectionPtr = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    REQUIRE(connectionPtr);
    REQUIRE_FALSE(hasMprisOwner(connectionPtr));

    // Subscription retirement is a positive observation of the loss handler.
    // The context iteration returns only after that handler and GLib unwind.
    bool contenderSubscribed = false;
    bool contenderDetached = false;
    auto contenderPtr = std::make_unique<MprisBridge>(
      playback,
      actions,
      MprisBridge::Callbacks{},
      MprisBridge::PlaybackSource{
        .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
        .onSnapshot =
          [&](rt::PlaybackSnapshotObserver)
        {
          contenderSubscribed = true;
          return async::Subscription{[&] { contenderDetached = true; }};
        },
        .elapsed = [&playback] { return playback.elapsed(); },
      });

    SECTION("a duplicate on the shared connection cannot release the first bridge's name")
    {
      auto const settleOwner = GENERATE(false, true);
      auto ownerPtr = std::make_unique<MprisBridge>(playback, actions, MprisBridge::Callbacks{});
      ownerPtr->start();

      if (settleOwner)
      {
        REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return ownerPtr->isActive(); }));
        REQUIRE(hasMprisOwner(connectionPtr));
      }
      else
      {
        CHECK_FALSE(ownerPtr->isActive());
      }

      contenderPtr->start();
      // A same-process contender must be rejected before starting subscriptions
      // or another unsupported same-name request on GIO's shared connection.
      CHECK_FALSE(contenderSubscribed);
      CHECK_FALSE(contenderPtr->isActive());

      if (settleOwner)
      {
        CHECK(ownerPtr->isActive());
        CHECK(hasMprisOwner(connectionPtr));
      }

      contenderPtr.reset();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return ownerPtr->isActive(); }));
      CHECK(ownerPtr->isActive());
      CHECK(hasMprisOwner(connectionPtr));

      ownerPtr.reset();
      CHECK_FALSE(hasMprisOwner(connectionPtr));
    }

    SECTION("a failed contender does not queue behind another connection")
    {
      auto const* const address = ::g_getenv("DBUS_SESSION_BUS_ADDRESS");
      REQUIRE(address != nullptr);
      auto const peerPtr = Gio::DBus::Connection::create_for_address_sync(
        address,
        Gio::DBus::ConnectionFlags::AUTHENTICATION_CLIENT | Gio::DBus::ConnectionFlags::MESSAGE_BUS_CONNECTION);
      auto const requested = callBus(
        peerPtr, "RequestName", Glib::Variant<std::tuple<Glib::ustring, std::uint32_t>>::create({kBusName, 0U}));
      auto reply = Glib::Variant<std::uint32_t>{};
      requested.get_child(reply, 0);
      REQUIRE(reply.get() == 1U);
      REQUIRE(hasMprisOwner(connectionPtr));

      contenderPtr->start();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return contenderDetached; }));
      CHECK_FALSE(contenderPtr->isActive());
      CHECK(hasMprisOwner(connectionPtr));

      bool duplicateSubscribed = false;
      auto duplicatePtr = std::make_unique<MprisBridge>(
        playback,
        actions,
        MprisBridge::Callbacks{},
        MprisBridge::PlaybackSource{
          .snapshot = [&playback] -> rt::PlaybackSnapshot const& { return playback.snapshot(); },
          .onSnapshot =
            [&](rt::PlaybackSnapshotObserver)
          {
            duplicateSubscribed = true;
            return async::Subscription{};
          },
          .elapsed = [&playback] { return playback.elapsed(); },
        });
      duplicatePtr->start();
      CHECK_FALSE(duplicateSubscribed);
      CHECK_FALSE(duplicatePtr->isActive());

      auto const released =
        callBus(peerPtr, "ReleaseName", Glib::Variant<std::tuple<Glib::ustring>>::create({kBusName}));
      released.get_child(reply, 0);
      REQUIRE(reply.get() == 1U);
      // The daemon round-trip observes release and any queued promotion before
      // the contender is destroyed; destruction must not hide a queued owner.
      CHECK_FALSE(hasMprisOwner(connectionPtr));
      contenderPtr.reset();
      CHECK_FALSE(hasMprisOwner(connectionPtr));
      // A rejected local bridge can retry only after the failed owner's token
      // has actually been retired, not merely after its name-lost callback.
      duplicatePtr->start();
      REQUIRE(duplicateSubscribed);
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return duplicatePtr->isActive(); }));
      CHECK(hasMprisOwner(connectionPtr));
      duplicatePtr.reset();
      CHECK_FALSE(hasMprisOwner(connectionPtr));
      peerPtr->close_sync();
    }

    ao::gtk::test::drainGtkEvents();
  }

  TEST_CASE("MprisBridge - startup exceptions retire partial work and permit retry or replacement",
            "[gtk][integration][mpris][async]")
  {
    ao::gtk::test::requireOwnedGtkSessionBus();
    enum class FailureStage : std::uint8_t
    {
      Snapshot,
      Artwork,
      Subscription,
    };
    auto const failureStage = GENERATE(FailureStage::Snapshot, FailureStage::Artwork, FailureStage::Subscription);
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto const connectionPtr = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    REQUIRE(connectionPtr);
    REQUIRE_FALSE(hasMprisOwner(connectionPtr));

    auto snapshot = rt::PlaybackSnapshot{};
    snapshot.transport.nowPlaying = rt::NowPlayingInfo{.trackId = TrackId{1}, .coverArtId = ResourceId{11}};
    bool failStartup = false;
    std::int32_t requests = 0;
    std::int32_t cancellations = 0;
    auto bridge = MprisBridge{
      playback,
      actions,
      MprisBridge::Callbacks{.requestArtUrl = [&](ResourceId, MprisBridge::OnArtUrlReady) -> utility::ScopedRegistration
                             {
                               if (failStartup && failureStage == FailureStage::Artwork)
                               {
                                 throw std::runtime_error{"startup failure"};
                               }

                               ++requests;
                               return utility::ScopedRegistration{[&] { ++cancellations; }};
                             }},
      MprisBridge::PlaybackSource{
        .snapshot = [&] -> rt::PlaybackSnapshot const&
        {
          if (failStartup && failureStage == FailureStage::Snapshot)
          {
            throw std::runtime_error{"startup failure"};
          }

          return snapshot;
        },
        .onSnapshot =
          [&](rt::PlaybackSnapshotObserver)
        {
          if (failStartup && failureStage == FailureStage::Subscription)
          {
            throw std::runtime_error{"startup failure"};
          }

          return async::Subscription{};
        },
        .elapsed = [&playback] { return playback.elapsed(); },
      }};
    // Construction reads the initial snapshot too; inject only at startup.
    failStartup = true;
    REQUIRE_THROWS_WITH(bridge.start(), "startup failure");
    CHECK_FALSE(bridge.isActive());
    CHECK_FALSE(hasMprisOwner(connectionPtr));
    CHECK(requests == (failureStage == FailureStage::Subscription ? 1 : 0));
    CHECK(cancellations == requests);
    failStartup = false;

    SECTION("the same bridge retries")
    {
      bridge.start();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return bridge.isActive(); }));
      CHECK(hasMprisOwner(connectionPtr));
    }

    SECTION("another bridge starts while the failed bridge remains alive")
    {
      auto replacement = MprisBridge{playback, actions, {}};
      replacement.start();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return replacement.isActive(); }));
      CHECK(hasMprisOwner(connectionPtr));
    }
  }

  TEST_CASE("MprisBridge - owner retirement permits replacement without stale callbacks",
            "[gtk][integration][mpris][async]")
  {
    ao::gtk::test::requireOwnedGtkSessionBus();
    auto const settleFirst = GENERATE(false, true);
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto actions = uimodel::PlaybackActions{playback, [] {}};
    auto const connectionPtr = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    REQUIRE(connectionPtr);
    REQUIRE_FALSE(hasMprisOwner(connectionPtr));

    {
      auto first = MprisBridge{playback, actions, {}};
      first.start();

      if (settleFirst)
      {
        REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return first.isActive(); }));
        REQUIRE(hasMprisOwner(connectionPtr));
      }
      else
      {
        CHECK_FALSE(first.isActive());
      }
    }

    {
      auto replacement = MprisBridge{playback, actions, {}};
      replacement.start();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil([&] { return replacement.isActive(); }));
      ao::gtk::test::drainGtkEvents();
      CHECK(replacement.isActive());
      CHECK(hasMprisOwner(connectionPtr));
    }
    CHECK_FALSE(hasMprisOwner(connectionPtr));
    ao::gtk::test::drainGtkEvents();
  }
} // namespace ao::gtk::platform::test
