// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/BackendGraphRegistry.h"

#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    flow::Graph graphWithNode(std::string id)
    {
      return {.nodes = {{.id = std::move(id), .type = flow::NodeType::Stream}}};
    }
  } // namespace

  TEST_CASE("BackendGraphRegistry - subscriber receives empty then current route snapshot",
            "[audio][unit][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto firstSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { received.push_back(graph); });

    REQUIRE(received.size() == 1);
    CHECK(received.front().nodes.empty());

    registry.publish("route-a", graphWithNode("stream-a"));

    REQUIRE(received.size() == 2);
    REQUIRE(received.back().nodes.size() == 1);
    CHECK(received.back().nodes.front().id == "stream-a");

    auto lateGraph = flow::Graph{};
    auto lateSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { lateGraph = graph; });
    REQUIRE(lateGraph.nodes.size() == 1);
    CHECK(lateGraph.nodes.front().id == "stream-a");
  }

  TEST_CASE("BackendGraphRegistry - route fallback applies only before publication", "[audio][unit][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto received = flow::Graph{};
    auto firstSub =
      registry.subscribe("route-a", [&](flow::Graph const& graph) { received = graph; }, graphWithNode("fallback"));

    REQUIRE(received.nodes.size() == 1U);
    CHECK(received.nodes.front().id == "fallback");

    registry.publish("route-a", graphWithNode("published"));
    auto lateSub =
      registry.subscribe("route-a", [&](flow::Graph const& graph) { received = graph; }, graphWithNode("ignored"));

    REQUIRE(received.nodes.size() == 1U);
    CHECK(received.nodes.front().id == "published");
  }

  TEST_CASE("BackendGraphRegistry - equal snapshots do not redeliver and remain available to late subscribers",
            "[audio][unit][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto sub = registry.subscribe("route-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    auto const current = graphWithNode("current");

    registry.publish("route-a", current);
    registry.publish("route-a", current);

    CHECK((received == std::vector<flow::Graph>{{}, current}));
    auto lateGraphs = std::vector<flow::Graph>{};
    auto lateSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { lateGraphs.push_back(graph); });
    CHECK(lateGraphs == std::vector<flow::Graph>{current});

    registry.publish("route-b", current);
    CHECK(received.size() == 2U);
  }

  TEST_CASE("BackendGraphRegistry - initial callback publication drains without nesting or losing the current graph",
            "[audio][unit][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto const current = graphWithNode("current");
    auto received = std::vector<flow::Graph>{};
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    auto sub = registry.subscribe("route-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    ++depth;
                                    maxDepth = std::max(maxDepth, depth);
                                    received.push_back(graph);

                                    // This limit only prevents a regression from exhausting the runner's stack.
                                    if (depth < 8U)
                                    {
                                      registry.publish("route-a", current);
                                    }

                                    --depth;
                                  });

    CHECK(maxDepth == 1U);
    CHECK((received == std::vector<flow::Graph>{{}, current}));
    REQUIRE(sub);
    sub.reset();
    registry.publish("route-a", graphWithNode("after-cancellation"));
    CHECK(received.size() == 2U);
  }

  TEST_CASE("BackendGraphRegistry - reentrant subscription defers its initial callback and remains cancellable",
            "[audio][unit][backend-graph][concurrency]")
  {
    bool cancelInitial = false;

    SECTION("initial callback publishes the current graph")
    {
    }

    SECTION("subscription is cancelled before its initial callback")
    {
      cancelInitial = true;
    }

    auto registry = BackendGraphRegistry{};
    auto innerSub = utility::ScopedRegistration{};
    auto innerGraphs = std::vector<flow::Graph>{};
    auto const current = graphWithNode("current");
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    bool returnedFromSubscribe = false;
    bool initialAfterSubscribe = false;
    auto outerSub = registry.subscribe("outer",
                                       [&](flow::Graph const& graph)
                                       {
                                         if (graph.nodes.empty())
                                         {
                                           return;
                                         }

                                         ++depth;
                                         innerSub = registry.subscribe("inner",
                                                                       [&](flow::Graph const& innerGraph)
                                                                       {
                                                                         ++depth;
                                                                         maxDepth = std::max(maxDepth, depth);
                                                                         initialAfterSubscribe = returnedFromSubscribe;
                                                                         innerGraphs.push_back(innerGraph);

                                                                         if (depth < 8U)
                                                                         {
                                                                           registry.publish("inner", current);
                                                                         }

                                                                         --depth;
                                                                       });
                                         returnedFromSubscribe = true;

                                         if (cancelInitial)
                                         {
                                           innerSub.reset();
                                         }

                                         --depth;
                                       });

    registry.publish("outer", graphWithNode("trigger"));

    if (cancelInitial)
    {
      CHECK(innerGraphs.empty());
      CHECK_FALSE(innerSub);
    }
    else
    {
      CHECK(maxDepth == 1U);
      CHECK(initialAfterSubscribe);
      CHECK((innerGraphs == std::vector<flow::Graph>{{}, current}));
      CHECK(innerSub);
    }
  }

  TEST_CASE("BackendGraphRegistry - reentrant real changes coalesce and supersede unfinished delivery",
            "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    auto firstGraphs = std::vector<flow::Graph>{};
    auto secondGraphs = std::vector<flow::Graph>{};
    auto const obsolete = graphWithNode("obsolete");
    auto const current = graphWithNode("current");
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    auto firstSub = registry.subscribe("route-a",
                                       [&](flow::Graph const& graph)
                                       {
                                         ++depth;
                                         maxDepth = std::max(maxDepth, depth);
                                         firstGraphs.push_back(graph);

                                         if (graph == obsolete)
                                         {
                                           registry.publish("route-a", graphWithNode("intermediate"));
                                           registry.publish("route-a", current);
                                         }

                                         --depth;
                                       });
    auto secondSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { secondGraphs.push_back(graph); });

    registry.publish("route-a", obsolete);

    CHECK(maxDepth == 1U);
    CHECK((firstGraphs == std::vector<flow::Graph>{{}, obsolete, current}));
    CHECK((secondGraphs == std::vector<flow::Graph>{{}, current}));
  }

  TEST_CASE("BackendGraphRegistry - cancellation discards a reentrant publication for the cancelled subscriber",
            "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto const obsolete = graphWithNode("obsolete");
    auto sub = utility::ScopedRegistration{};
    sub = registry.subscribe("route-a",
                             [&](flow::Graph const& graph)
                             {
                               received.push_back(graph);

                               if (graph == obsolete)
                               {
                                 registry.publish("route-a", graphWithNode("current"));
                                 sub.reset();
                               }
                             });

    registry.publish("route-a", obsolete);

    CHECK_FALSE(sub);
    CHECK((received == std::vector<flow::Graph>{{}, obsolete}));
    auto lateGraph = flow::Graph{};
    auto lateSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { lateGraph = graph; });
    CHECK(lateGraph == graphWithNode("current"));
  }

  TEST_CASE("BackendGraphRegistry - cancellation removes callback already copied for route publication",
            "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    bool cancelSecond = false;
    std::int32_t firstCalls = 0;
    std::int32_t secondCalls = 0;
    auto secondSub = utility::ScopedRegistration{};
    auto firstSub = registry.subscribe("route-a",
                                       [&](flow::Graph const&)
                                       {
                                         ++firstCalls;

                                         if (cancelSecond)
                                         {
                                           secondSub.reset();
                                         }
                                       });
    secondSub = registry.subscribe("route-a", [&](flow::Graph const&) { ++secondCalls; });
    cancelSecond = true;

    registry.publish("route-a", graphWithNode("stream-a"));

    CHECK(firstCalls == 2);
    CHECK(secondCalls == 1);
  }

  TEST_CASE("BackendGraphRegistry - clear affects only the selected route", "[audio][unit][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto routeAGraph = flow::Graph{};
    std::int32_t routeBCalls = 0;
    auto routeASub = registry.subscribe("route-a", [&](flow::Graph const& graph) { routeAGraph = graph; });
    auto routeBSub = registry.subscribe("route-b", [&](flow::Graph const&) { ++routeBCalls; });
    registry.publish("route-a", graphWithNode("stream-a"));
    registry.publish("route-b", graphWithNode("stream-b"));

    registry.clear("route-a");

    CHECK(routeAGraph.nodes.empty());
    CHECK(routeAGraph.connections.empty());
    CHECK(routeBCalls == 2);
  }

  TEST_CASE("BackendGraphRegistry - reentrant clear supersedes an older publication",
            "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    auto secondSubscriberGraphs = std::vector<flow::Graph>{};
    std::size_t depth = 0U;
    std::size_t maxDepth = 0U;
    auto firstSub = registry.subscribe("route-a",
                                       [&](flow::Graph const& graph)
                                       {
                                         ++depth;
                                         maxDepth = std::max(maxDepth, depth);

                                         if (!graph.nodes.empty())
                                         {
                                           registry.clear("route-a");
                                         }

                                         --depth;
                                       });
    auto secondSub =
      registry.subscribe("route-a", [&](flow::Graph const& graph) { secondSubscriberGraphs.push_back(graph); });

    registry.publish("route-a", graphWithNode("obsolete"));

    CHECK(maxDepth == 1U);
    REQUIRE(secondSubscriberGraphs.size() == 2U);
    CHECK(secondSubscriberGraphs[0].nodes.empty());
    CHECK(secondSubscriberGraphs[1].nodes.empty());
  }

  TEST_CASE("BackendGraphRegistry - a blocked publisher keeps delivery ownership after superseding a route",
            "[audio][unit][backend-graph][concurrency]")
  {
    constexpr auto kWaitTimeout = std::chrono::seconds{5};
    auto registry = BackendGraphRegistry{};
    auto const obsolete = graphWithNode("obsolete");
    auto const latest = graphWithNode("latest");
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto duplicateReturned = std::binary_semaphore{0};
    auto firstGraphs = std::vector<flow::Graph>{};
    auto secondGraphs = std::vector<flow::Graph>{};
    auto latestCallbackThreads = std::vector<std::thread::id>{};
    auto firstSub = registry.subscribe("route-a",
                                       [&](flow::Graph const& graph)
                                       {
                                         firstGraphs.push_back(graph);

                                         if (graph == obsolete)
                                         {
                                           callbackEntered.release();
                                           releaseCallback.acquire();
                                         }

                                         if (graph == latest)
                                         {
                                           latestCallbackThreads.push_back(std::this_thread::get_id());
                                         }
                                       });
    auto secondSub = registry.subscribe("route-a",
                                        [&](flow::Graph const& graph)
                                        {
                                          secondGraphs.push_back(graph);

                                          if (graph == latest)
                                          {
                                            latestCallbackThreads.push_back(std::this_thread::get_id());
                                          }
                                        });
    auto obsoletePublisher = std::jthread{[&] { registry.publish("route-a", obsolete); }};
    auto const entered = callbackEntered.try_acquire_for(kWaitTimeout);
    auto start = std::barrier{3};
    auto firstReturned = std::atomic_bool{false};
    auto secondReturned = std::atomic_bool{false};
    auto firstReturnedIndex = std::atomic{std::int32_t{0}};
    auto publishLatest = [&](std::int32_t const index, std::atomic_bool& returned)
    {
      start.arrive_and_wait();
      registry.publish("route-a", latest);
      returned.store(true);

      if (std::int32_t expected = 0; firstReturnedIndex.compare_exchange_strong(expected, index))
      {
        duplicateReturned.release();
      }
    };
    auto firstPublisher = std::jthread{[&] { publishLatest(1, firstReturned); }};
    auto secondPublisher = std::jthread{[&] { publishLatest(2, secondReturned); }};
    auto const firstThread = firstPublisher.get_id();
    auto const secondThread = secondPublisher.get_id();
    start.arrive_and_wait();

    // One caller commits latest and must wait for the gate. Its equal-graph peer
    // can return only after observing that committed revision under the state lock.
    auto const duplicateFinished = duplicateReturned.try_acquire_for(kWaitTimeout);
    auto const duplicateIndex = firstReturnedIndex.load();
    auto const writerWasWaiting =
      duplicateIndex == 1 ? !secondReturned.load() : duplicateIndex == 2 && !firstReturned.load();
    releaseCallback.release();
    obsoletePublisher.join();
    firstPublisher.join();
    secondPublisher.join();

    // Always release and join before asserting, including timeout failures.
    REQUIRE(entered);
    REQUIRE(duplicateFinished);
    CHECK(writerWasWaiting);
    REQUIRE((duplicateIndex == 1 || duplicateIndex == 2));
    auto const publisherThread = duplicateIndex == 1 ? secondThread : firstThread;
    CHECK((firstGraphs == std::vector<flow::Graph>{{}, obsolete, latest}));
    CHECK((secondGraphs == std::vector<flow::Graph>{{}, latest}));
    CHECK((latestCallbackThreads == std::vector{publisherThread, publisherThread}));
  }

  TEST_CASE("BackendGraphRegistry - shutdown waits for an active callback and closes admission",
            "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    auto callbackEntered = std::binary_semaphore{0};
    auto releaseCallback = std::binary_semaphore{0};
    auto shutdownReturned = std::binary_semaphore{0};
    auto nonEmptyCalls = std::atomic<std::int32_t>{0};
    auto emptyCalls = std::atomic<std::int32_t>{0};
    auto blockPublication = std::atomic_bool{false};
    auto sub = registry.subscribe("route-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    if (graph.nodes.empty())
                                    {
                                      ++emptyCalls;
                                      return;
                                    }

                                    ++nonEmptyCalls;

                                    if (blockPublication.load())
                                    {
                                      callbackEntered.release();
                                      releaseCallback.acquire();
                                    }
                                  });
    blockPublication = true;
    auto publisher = std::jthread{[&] { registry.publish("route-a", graphWithNode("stream-a")); }};
    callbackEntered.acquire();
    auto shutdownThread = std::jthread{[&]
                                       {
                                         registry.shutdown();
                                         shutdownReturned.release();
                                       }};

    CHECK_FALSE(shutdownReturned.try_acquire());
    releaseCallback.release();
    publisher.join();
    shutdownThread.join();

    CHECK(nonEmptyCalls.load() == 1);
    CHECK(emptyCalls.load() == 2);
    registry.publish("route-a", graphWithNode("late"));
    CHECK(nonEmptyCalls.load() == 1);
    auto lateSub = registry.subscribe("route-a", [](flow::Graph const&) {});
    CHECK_FALSE(lateSub);
  }

  TEST_CASE(
    "BackendGraphRegistry - callback shutdown delivers final empties before returning and respects cancellation",
    "[audio][unit][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto cancelledGraphs = std::vector<flow::Graph>{};
    auto cancelledSub = utility::ScopedRegistration{};
    bool shutdownReturned = false;
    bool callbackAfterShutdown = false;
    std::size_t callsAtShutdownReturn = 0U;
    auto sub = registry.subscribe("route-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    callbackAfterShutdown = callbackAfterShutdown || shutdownReturned;
                                    received.push_back(graph);

                                    if (!graph.nodes.empty() && graph.nodes.front().id == "current")
                                    {
                                      registry.publish("route-a", graphWithNode("pending"));
                                      cancelledSub.reset();
                                      registry.shutdown();
                                      callsAtShutdownReturn = received.size();
                                      shutdownReturned = true;
                                    }
                                  });
    cancelledSub = registry.subscribe("route-a", [&](flow::Graph const& graph) { cancelledGraphs.push_back(graph); });
    auto const current = graphWithNode("current");

    registry.publish("route-a", current);
    registry.publish("route-a", graphWithNode("late"));
    registry.clear("route-a");
    registry.shutdown();

    // Shutdown's synchronous final delivery is intentionally a separate retirement contract.
    CHECK(shutdownReturned);
    CHECK(callsAtShutdownReturn == 3U);
    CHECK_FALSE(callbackAfterShutdown);
    CHECK((received == std::vector<flow::Graph>{{}, current, {}}));
    CHECK(cancelledGraphs == std::vector<flow::Graph>{{}});
    CHECK_FALSE(registry.subscribe("route-a", [](flow::Graph const&) {}));
  }

  TEST_CASE("BackendGraphRegistry - subscription may outlive registry", "[audio][unit][backend-graph]")
  {
    auto sub = utility::ScopedRegistration{};

    {
      auto registryPtr = std::make_unique<BackendGraphRegistry>();
      sub = registryPtr->subscribe("route-a", [](flow::Graph const&) {});
      REQUIRE(sub);
    }

    sub.reset();
    CHECK_FALSE(sub);
  }
} // namespace ao::audio::backend::detail::test
