// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/NullBackend.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Player.h>
#include <ao/audio/RouteAnchor.h>
#include <ao/audio/flow/Graph.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ao::audio::test
{
  namespace
  {
    class GraphObservationExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return _queue.isCurrent(); }

      void dispatch(compat::MoveOnlyFunction<void()> task) override
      {
        if (!isCurrent() && _blockNextForeign.exchange(false))
        {
          _entered.release();
          _release.acquire();
        }

        _queue.dispatch(std::move(task));
      }

      void defer(compat::MoveOnlyFunction<void()> task) override { _queue.defer(std::move(task)); }
      rt::test::ManualExecutor& queue() { return _queue; }
      void blockNextForeignDispatch() { _blockNextForeign.store(true); }
      bool tryWaitForBlockedDispatch() { return _entered.try_acquire_for(std::chrono::seconds{5}); }
      void releaseBlockedDispatch() { _release.release(); }

    private:
      rt::test::ManualExecutor _queue;
      std::atomic<bool> _blockNextForeign{false};
      std::binary_semaphore _entered{0};
      std::binary_semaphore _release{0};
    };

    class GraphObservationProvider final : public BackendProvider
    {
    public:
      explicit GraphObservationProvider(bool returnsRegistration)
        : _returnsRegistration{returnsRegistration}
      {
      }

      void shutdown() noexcept override { _callback = {}; }
      Status status() const override { return {.descriptor = {.id = kBackendNone}}; }
      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback({device()});
        return {};
      }
      utility::ScopedRegistration subscribeGraph(std::string_view /*anchorId*/,
                                                 OnGraphChangedCallback callback) override
      {
        _callback = std::move(callback);

        if (!_returnsRegistration)
        {
          return {};
        }

        return utility::ScopedRegistration{[this] { _callback = {}; }};
      }
      std::unique_ptr<Backend> createBackend(Device const& /*device*/, ProfileId const& /*profile*/) override
      {
        return std::make_unique<NullBackend>();
      }
      static Device device() { return {.id = DeviceId{"graph-observation"}, .backendId = kBackendNone}; }
      OnGraphChangedCallback callback() const { return _callback; }

    private:
      OnGraphChangedCallback _callback;
      bool _returnsRegistration;
    };

    flow::Graph observationGraph(std::string name)
    {
      return {.nodes = {flow::Node{.id = "observed-node", .name = std::move(name)}}};
    }

    struct GraphObservationFixture final
    {
      explicit GraphObservationFixture(bool returnsRegistration = true)
      {
        auto providerPtr = std::make_unique<GraphObservationProvider>(returnsRegistration);
        provider = providerPtr.get();
        playerPtr->addProvider(std::move(providerPtr));
        executor.queue().runUntilIdle();
        REQUIRE(playerPtr->setOutputDevice(kBackendNone, GraphObservationProvider::device().id, kProfileShared));
        subscribe();
        executor.queue().runUntilIdle();
      }

      void subscribe() const
      {
        playerPtr->handleRouteChanged(
          Engine::RouteStatus{.optAnchor = RouteAnchor{.backend = kBackendNone, .id = "observed-route"}},
          playerPtr->playbackGeneration());
      }

      std::string graphName() const
      {
        auto const status = playerPtr->status();
        auto const iter = std::ranges::find(status.flow.nodes, std::string_view{"observed-node"}, &flow::Node::id);
        return iter == status.flow.nodes.end() ? std::string{} : iter->name;
      }

      GraphObservationExecutor executor;
      std::unique_ptr<Player> playerPtr = std::make_unique<Player>(executor);
      GraphObservationProvider* provider = nullptr;
    };
  } // namespace

  TEST_CASE("Player - graph bursts remain coalesced through outward publication and yield to owner work",
            "[audio][regression][player][concurrency]")
  {
    auto fixture = GraphObservationFixture{};
    auto& queue = fixture.executor.queue();
    auto callback = fixture.provider->callback();
    std::size_t delivered = 0;
    bool ownerWorkRan = false;
    fixture.playerPtr->setOnQualityChanged(
      [&](auto const&, bool)
      {
        ++delivered;

        if (delivered == 1)
        {
          callback(observationGraph("reentrant-final"));
          queue.defer([&] { ownerWorkRan = true; });
        }
      });

    for (std::size_t index = 0; index < 1000; ++index)
    {
      callback(observationGraph("before-handoff"));
    }

    REQUIRE(queue.queuedCount() == 1);
    REQUIRE(queue.tryRunOne());
    REQUIRE(queue.tryWaitUntilQueued());

    for (std::size_t index = 0; index < 1000; ++index)
    {
      callback(observationGraph("after-handoff"));
    }

    REQUIRE(queue.queuedCount() == 1);
    REQUIRE(queue.tryRunOne());
    CHECK(fixture.graphName() == "after-handoff");
    CHECK(delivered == 0);

    for (std::size_t index = 0; index < 1000; ++index)
    {
      callback(observationGraph("before-publication"));
    }

    REQUIRE(queue.queuedCount() == 1);
    REQUIRE(queue.tryRunOne());
    CHECK(delivered == 1);
    REQUIRE(queue.tryRunOne());
    CHECK(ownerWorkRan);
    CHECK(delivered == 1);
    REQUIRE(queue.tryDrainUntil([&] { return delivered == 2; }));
    CHECK(fixture.graphName() == "reentrant-final");
    CHECK(queue.queuedCount() == 0);
  }

  TEST_CASE("Player - graph producer remains bounded while the Engine worker is withheld",
            "[audio][regression][player][concurrency]")
  {
    auto fixture = GraphObservationFixture{};
    auto& queue = fixture.executor.queue();
    auto callback = fixture.provider->callback();
    std::size_t delivered = 0;
    fixture.playerPtr->setOnQualityChanged([&](auto const&, bool) { ++delivered; });
    // Release the worker before Player destruction even if an assertion fails.
    auto releaseWorker = utility::ScopedRegistration{[&] { fixture.executor.releaseBlockedDispatch(); }};
    fixture.executor.blockNextForeignDispatch();
    callback(observationGraph("first"));
    REQUIRE(queue.tryRunOne());
    REQUIRE(fixture.executor.tryWaitForBlockedDispatch());

    auto producer = std::jthread{[&]
                                 {
                                   for (std::size_t index = 0; index < 10000; ++index)
                                   {
                                     callback(observationGraph("latest"));
                                   }
                                 }};
    producer.join();
    CHECK(queue.queuedCount() == 0);
    CHECK(delivered == 0);
    bool ownerWorkRan = false;
    queue.defer([&] { ownerWorkRan = true; });
    REQUIRE(queue.tryRunOne());
    CHECK(ownerWorkRan);
    releaseWorker.reset();
    REQUIRE(queue.tryDrainUntil([&] { return delivered == 1; }));
    CHECK(fixture.graphName() == "latest");
    CHECK(queue.queuedCount() == 0);
  }

  TEST_CASE("Player - retiring a graph subscription cancels its queued publication in the same generation",
            "[audio][regression][player][concurrency]")
  {
    auto fixture = GraphObservationFixture{};
    auto& queue = fixture.executor.queue();
    auto staleCallback = fixture.provider->callback();
    staleCallback(observationGraph("retired"));
    REQUIRE(queue.tryRunOne());
    REQUIRE(queue.tryWaitUntilQueued());

    SECTION("Retire before applying the graph")
    {
    }

    SECTION("Retire after applying the graph but before outward publication")
    {
      REQUIRE(queue.tryRunOne());
      CHECK(fixture.graphName() == "retired");
    }

    auto const generation = fixture.playerPtr->playbackGeneration();
    fixture.playerPtr->handleRouteChanged(Engine::RouteStatus{}, generation);
    fixture.subscribe();
    CHECK(fixture.playerPtr->playbackGeneration() == generation);
    std::size_t delivered = 0;
    fixture.playerPtr->setOnQualityChanged([&](auto const&, bool) { ++delivered; });
    // Only the two explicit route notifications remain eligible for publication.
    queue.runUntilIdle();
    CHECK(delivered == 2);
    delivered = 0;
    staleCallback(observationGraph("obsolete"));
    CHECK(queue.queuedCount() == 0);
    fixture.provider->callback()(observationGraph("current"));
    REQUIRE(queue.tryDrainUntil([&] { return delivered == 1; }));
    CHECK(fixture.graphName() == "current");
  }

  TEST_CASE("Player - replacing an unregistered graph callback retires its admission",
            "[audio][regression][player][concurrency]")
  {
    auto fixture = GraphObservationFixture{false};
    auto& queue = fixture.executor.queue();
    auto staleCallback = fixture.provider->callback();
    fixture.subscribe();
    queue.runUntilIdle();
    std::size_t delivered = 0;
    fixture.playerPtr->setOnQualityChanged([&](auto const&, bool) { ++delivered; });
    staleCallback(observationGraph("obsolete"));
    CHECK(queue.queuedCount() == 0);
    fixture.provider->callback()(observationGraph("current"));
    REQUIRE(queue.tryDrainUntil([&] { return delivered == 1; }));
    CHECK(fixture.graphName() == "current");
  }

  TEST_CASE("Player - retained graph callbacks become no-ops after teardown",
            "[audio][regression][player][concurrency]")
  {
    auto fixture = GraphObservationFixture{};
    auto& queue = fixture.executor.queue();
    auto callback = fixture.provider->callback();
    std::size_t delivered = 0;
    fixture.playerPtr->setOnQualityChanged([&](auto const&, bool) { ++delivered; });
    callback(observationGraph("queued"));
    REQUIRE(queue.tryRunOne());
    REQUIRE(queue.tryWaitUntilQueued());
    fixture.playerPtr.reset();
    auto producer = std::jthread{[&] { callback(observationGraph("after-teardown")); }};
    producer.join();
    queue.runUntilIdle();
    CHECK(delivered == 0);
    CHECK(queue.queuedCount() == 0);
  }
} // namespace ao::audio::test
