// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/CorePrimitives.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>

namespace ao::app::test
{
  TEST_CASE("EventBus - subscribe and publish", "[app][runtime][event]")
  {
    auto bus = EventBus{};

    SECTION("single subscriber receives event")
    {
      auto received = false;
      auto sub = bus.subscribe<PlaybackTransportChanged>(
        [&](PlaybackTransportChanged const& e)
        {
          received = true;
          CHECK(e.transport == ao::audio::Transport::Playing);
        });

      bus.publish(PlaybackTransportChanged{.transport = ao::audio::Transport::Playing});
      CHECK(received);
    }

    SECTION("multiple subscribers all receive event")
    {
      auto count = 0;
      auto sub1 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });
      auto sub2 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });
      auto sub3 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });

      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 3);
    }

    SECTION("unsubscribed handler is not called")
    {
      auto count = 0;
      auto sub = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });

      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 1);

      sub.reset();
      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 1);
    }
  }

  TEST_CASE("EventBus - subscriptions are independent per event type", "[app][runtime][event]")
  {
    auto bus = EventBus{};
    auto transportCount = 0;
    auto trackCount = 0;

    auto sub1 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++transportCount; });
    auto sub2 = bus.subscribe<NowPlayingTrackChanged>([&](NowPlayingTrackChanged const&) { ++trackCount; });

    SECTION("publishing transport only notifies transport subscriber")
    {
      bus.publish(PlaybackTransportChanged{});
      CHECK(transportCount == 1);
      CHECK(trackCount == 0);
    }

    SECTION("publishing track only notifies track subscriber")
    {
      bus.publish(NowPlayingTrackChanged{});
      CHECK(transportCount == 0);
      CHECK(trackCount == 1);
    }
  }

  TEST_CASE("EventBus - publishing with no subscribers is safe", "[app][runtime][event]")
  {
    auto bus = EventBus{};

    SECTION("publish to type with no subscribers")
    {
      REQUIRE_NOTHROW(bus.publish(PlaybackTransportChanged{}));
    }

    SECTION("publish after all unsubscribed")
    {
      auto sub = bus.subscribe<PlaybackTransportChanged>([](PlaybackTransportChanged const&) {});
      sub.reset();
      REQUIRE_NOTHROW(bus.publish(PlaybackTransportChanged{}));
    }
  }

  TEST_CASE("EventBus - event data is delivered correctly", "[app][runtime][event]")
  {
    auto bus = EventBus{};

    SECTION("NowPlayingTrackChanged carries track data")
    {
      auto trackId = ao::TrackId{};
      auto listId = ao::ListId{};
      auto sub = bus.subscribe<NowPlayingTrackChanged>(
        [&](NowPlayingTrackChanged const& e)
        {
          trackId = e.trackId;
          listId = e.sourceListId;
        });

      bus.publish(NowPlayingTrackChanged{
        .trackId = ao::TrackId{42},
        .sourceListId = ao::ListId{7},
      });

      CHECK(trackId == ao::TrackId{42});
      CHECK(listId == ao::ListId{7});
    }

    SECTION("PlaybackOutputChanged carries selection")
    {
      auto backendId = ao::audio::BackendId{};
      auto sub = bus.subscribe<PlaybackOutputChanged>([&](PlaybackOutputChanged const& e)
                                                      { backendId = e.selection.backendId; });

      bus.publish(PlaybackOutputChanged{
        .selection = {.backendId = ao::audio::kBackendAlsa},
      });

      CHECK(backendId == ao::audio::kBackendAlsa);
    }

    SECTION("FocusedViewChanged carries view id")
    {
      auto vid = ViewId{};
      auto sub = bus.subscribe<FocusedViewChanged>([&](FocusedViewChanged const& e) { vid = e.viewId; });

      bus.publish(FocusedViewChanged{.viewId = ViewId{99}});
      CHECK(vid == ViewId{99});
    }
  }

  TEST_CASE("EventBus - all event types are publishable", "[app][runtime][event]")
  {
    auto bus = EventBus{};

    SECTION("playback events")
    {
      REQUIRE_NOTHROW(bus.publish(PlaybackTransportChanged{}));
      REQUIRE_NOTHROW(bus.publish(NowPlayingTrackChanged{}));
      REQUIRE_NOTHROW(bus.publish(PlaybackOutputChanged{}));
      REQUIRE_NOTHROW(bus.publish(PlaybackFaultTransition{}));
    }

    SECTION("library events")
    {
      REQUIRE_NOTHROW(bus.publish(TracksMutated{}));
      REQUIRE_NOTHROW(bus.publish(ListsMutated{}));
      REQUIRE_NOTHROW(bus.publish(LibraryImportCompleted{}));
      REQUIRE_NOTHROW(bus.publish(ImportProgressUpdated{}));
    }

    SECTION("view events")
    {
      REQUIRE_NOTHROW(bus.publish(FocusedViewChanged{}));
      REQUIRE_NOTHROW(bus.publish(ViewDestroyed{}));
      REQUIRE_NOTHROW(bus.publish(RevealTrackRequested{}));
    }

    SECTION("notification events")
    {
      REQUIRE_NOTHROW(bus.publish(NotificationPosted{}));
      REQUIRE_NOTHROW(bus.publish(NotificationDismissed{}));
    }
  }

  TEST_CASE("EventBus - subscriber RAII lifecycle", "[app][runtime][event]")
  {
    auto bus = EventBus{};
    auto count = 0;

    SECTION("destructor unsubscribes")
    {
      {
        auto sub = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });
        bus.publish(PlaybackTransportChanged{});
        CHECK(count == 1);
      }
      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 1);
    }

    SECTION("move construction transfers subscription")
    {
      auto sub1 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });

      auto sub2 = Subscription{std::move(sub1)};

      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 1);
    }

    SECTION("move assignment transfers subscription")
    {
      auto sub1 = bus.subscribe<PlaybackTransportChanged>([&](PlaybackTransportChanged const&) { ++count; });

      auto sub2 = Subscription{};
      sub2 = std::move(sub1);

      bus.publish(PlaybackTransportChanged{});
      CHECK(count == 1);
    }
  }
}
