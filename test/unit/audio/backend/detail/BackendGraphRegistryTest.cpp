// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/BackendGraphRegistry.h"

#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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

  TEST_CASE("BackendGraphRegistry - cancellation removes callback already copied for route publication",
            "[audio][regression][backend-graph][concurrency]")
  {
    auto registry = BackendGraphRegistry{};
    bool cancelSecond = false;
    std::int32_t firstCalls = 0;
    std::int32_t secondCalls = 0;
    auto secondSub = Subscription{};
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
            "[audio][regression][backend-graph]")
  {
    auto registry = BackendGraphRegistry{};
    auto secondSubscriberGraphs = std::vector<flow::Graph>{};
    auto firstSub = registry.subscribe("route-a",
                                       [&](flow::Graph const& graph)
                                       {
                                         if (!graph.nodes.empty())
                                         {
                                           registry.clear("route-a");
                                         }
                                       });
    auto secondSub =
      registry.subscribe("route-a", [&](flow::Graph const& graph) { secondSubscriberGraphs.push_back(graph); });

    registry.publish("route-a", graphWithNode("obsolete"));

    REQUIRE(secondSubscriberGraphs.size() == 2U);
    CHECK(secondSubscriberGraphs[0].nodes.empty());
    CHECK(secondSubscriberGraphs[1].nodes.empty());
  }

  TEST_CASE("BackendGraphRegistry - shutdown waits for an active callback and closes admission",
            "[audio][regression][backend-graph][concurrency]")
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

  TEST_CASE("BackendGraphRegistry - subscription may outlive registry", "[audio][regression][backend-graph]")
  {
    auto sub = Subscription{};

    {
      auto registryPtr = std::make_unique<BackendGraphRegistry>();
      sub = registryPtr->subscribe("route-a", [](flow::Graph const&) {});
      REQUIRE(sub);
    }

    sub.reset();
    CHECK_FALSE(sub);
  }
} // namespace ao::audio::backend::detail::test
