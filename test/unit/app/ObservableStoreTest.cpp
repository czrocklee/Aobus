// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <runtime/CorePrimitives.h>
#include <runtime/ObservableStore.h>
#include <runtime/StateTypes.h>

namespace ao::app::test
{
  TEST_CASE("ObservableStore - snapshot returns current state", "[app][runtime][store]")
  {
    auto store = ObservableStore<FocusState>{};

    SECTION("default constructed store returns default state")
    {
      auto snap = store.snapshot();
      CHECK(snap.focusedView == ViewId{});
      CHECK(snap.revision == 0);
    }

    SECTION("store initialized with value returns that value")
    {
      auto store2 = ObservableStore<FocusState>{
        FocusState{.focusedView = ViewId{5}, .revision = 1},
      };
      auto snap = store2.snapshot();
      CHECK(snap.focusedView == ViewId{5});
    }
  }

  TEST_CASE("ObservableStore - subscribe with ReplayCurrent", "[app][runtime][store]")
  {
    auto store = ObservableStore<PlaybackState>{
      PlaybackState{.volume = 0.5F, .muted = true},
    };
    auto delivered = false;

    auto sub = store.subscribe(
      [&](PlaybackState const& state)
      {
        CHECK(state.volume == Catch::Approx(0.5F));
        CHECK(state.muted);
        delivered = true;
      },
      StoreDeliveryMode::ReplayCurrent);

    CHECK(delivered);
  }

  TEST_CASE("ObservableStore - subscribe with FutureOnly skips replay", "[app][runtime][store]")
  {
    auto store = ObservableStore<PlaybackState>{
      PlaybackState{.volume = 0.5F},
    };
    auto delivered = false;

    auto sub = store.subscribe([&](PlaybackState const&) { delivered = true; }, StoreDeliveryMode::FutureOnly);

    CHECK_FALSE(delivered);

    store.update(PlaybackState{.volume = 1.0F});
    CHECK(delivered);
  }

  TEST_CASE("ObservableStore - update notifies all subscribers", "[app][runtime][store]")
  {
    auto store = ObservableStore<FocusState>{};
    auto count1 = 0;
    auto count2 = 0;

    auto sub1 = store.subscribe([&](FocusState const&) { ++count1; }, StoreDeliveryMode::FutureOnly);
    auto sub2 = store.subscribe([&](FocusState const&) { ++count2; }, StoreDeliveryMode::FutureOnly);

    store.update(FocusState{.focusedView = ViewId{1}});
    CHECK(count1 == 1);
    CHECK(count2 == 1);
  }

  TEST_CASE("ObservableStore - unsubscribed handler not notified", "[app][runtime][store]")
  {
    auto store = ObservableStore<FocusState>{};
    auto count = 0;

    auto sub = store.subscribe([&](FocusState const&) { ++count; }, StoreDeliveryMode::FutureOnly);

    store.update(FocusState{});
    CHECK(count == 1);

    sub.reset();
    store.update(FocusState{});
    CHECK(count == 1);
  }

  TEST_CASE("ObservableStore - snapshot after update returns new state", "[app][runtime][store]")
  {
    auto store = ObservableStore<PlaybackState>{};

    store.update(PlaybackState{.volume = 0.75F, .muted = false});
    auto snap = store.snapshot();
    CHECK(snap.volume == Catch::Approx(0.75F));
    CHECK_FALSE(snap.muted);
  }

  TEST_CASE("ObservableStore - revision increments on update", "[app][runtime][store]")
  {
    auto store = ObservableStore<FocusState>{};
    CHECK(store.revision() == 0);

    store.update(FocusState{});
    CHECK(store.revision() == 1);

    store.update(FocusState{});
    CHECK(store.revision() == 2);
  }

  TEST_CASE("ObservableStore - multiple subscribers with mixed delivery modes", "[app][runtime][store]")
  {
    auto store = ObservableStore<FocusState>{
      FocusState{.focusedView = ViewId{10}},
    };
    auto replayReceived = false;
    auto futureReceived = false;

    auto sub1 = store.subscribe([&](FocusState const&) { replayReceived = true; }, StoreDeliveryMode::ReplayCurrent);

    auto sub2 = store.subscribe([&](FocusState const&) { futureReceived = true; }, StoreDeliveryMode::FutureOnly);

    CHECK(replayReceived);
    CHECK_FALSE(futureReceived);

    store.update(FocusState{.focusedView = ViewId{99}});
    CHECK(futureReceived);
  }

  TEST_CASE("ObservableStore - empty string state round-trip", "[app][runtime][store]")
  {
    auto store = ObservableStore<PlaybackState>{};

    store.update(PlaybackState{
      .transport = ao::audio::Transport::Playing,
      .positionMs = 12345,
      .durationMs = 67890,
    });

    auto snap = store.snapshot();
    CHECK(snap.transport == ao::audio::Transport::Playing);
    CHECK(snap.positionMs == 12345);
    CHECK(snap.durationMs == 67890);
  }

  TEST_CASE("ObservableStore - subscriber received correct reference", "[app][runtime][store]")
  {
    auto store = ObservableStore<PlaybackState>{
      PlaybackState{.ready = false},
    };

    store.update(PlaybackState{.ready = true});

    auto ready = false;
    auto sub =
      store.subscribe([&](PlaybackState const& state) { ready = state.ready; }, StoreDeliveryMode::ReplayCurrent);

    CHECK(ready);
  }
}
