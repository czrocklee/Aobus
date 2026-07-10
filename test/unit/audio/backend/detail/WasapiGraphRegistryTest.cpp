// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Format.h>
#include <ao/audio/backend/detail/WasapiGraphRegistry.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("WasapiGraphRegistry - initial subscription receives a neutral route graph", "[audio][unit][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};

    auto sub = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });

    REQUIRE(received.size() == 1);
    auto const& graph = received.front();
    REQUIRE(graph.nodes.size() == 2);
    CHECK(graph.nodes[0].id == "wasapi-stream");
    CHECK(graph.nodes[0].type == flow::NodeType::Stream);
    CHECK_FALSE(graph.nodes[0].optFormat);
    CHECK(graph.nodes[1].id == "wasapi-sink");
    CHECK(graph.nodes[1].type == flow::NodeType::Sink);
    CHECK(graph.nodes[1].name == "endpoint-a");
    CHECK(graph.nodes[1].objectPath == "endpoint-a");
    CHECK_FALSE(graph.nodes[1].softwareVolumeNotUnity);
    CHECK_FALSE(graph.nodes[1].isMuted);
    REQUIRE(graph.connections.size() == 1);
    CHECK(graph.connections[0].sourceId == "wasapi-stream");
    CHECK(graph.connections[0].destinationId == "wasapi-sink");
    CHECK(graph.connections[0].isActive);
  }

  TEST_CASE("WasapiGraphRegistry - publish projects input and mix formats independently",
            "[audio][unit][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = flow::Graph{};
    std::int32_t callCount = 0;
    auto const inputFormat = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 24, .validBits = 24};
    auto const mixFormat = Format{.sampleRate = 48000, .channels = 2, .bitDepth = 32, .validBits = 32, .isFloat = true};
    auto sub = registry.subscribe("endpoint-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    received = graph;
                                    ++callCount;
                                  });

    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = inputFormat,
                      .optMixFormat = mixFormat,
                      .volume = 0.25F,
                      .muted = true});

    CHECK(callCount == 2);
    REQUIRE(received.nodes.size() == 2);
    REQUIRE(received.nodes[0].optFormat);
    REQUIRE(received.nodes[1].optFormat);
    CHECK(*received.nodes[0].optFormat == inputFormat);
    CHECK(*received.nodes[1].optFormat == mixFormat);
    CHECK(received.nodes[1].softwareVolumeNotUnity);
    CHECK(received.nodes[1].minSoftwareGain == 0.25F);
    CHECK(received.nodes[1].maxSoftwareGain == 0.25F);
    CHECK(received.nodes[1].isMuted);
  }

  TEST_CASE("WasapiGraphRegistry - subscriptions receive only their route", "[audio][unit][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t callsA = 0;
    std::int32_t callsB = 0;
    auto subA = registry.subscribe("endpoint-a", [&](flow::Graph const&) { ++callsA; });
    auto subB = registry.subscribe("endpoint-b", [&](flow::Graph const&) { ++callsB; });

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});

    CHECK(callsA == 2);
    CHECK(callsB == 1);

    subA.reset();
    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.75F});
    CHECK(callsA == 2);
    CHECK(callsB == 1);
  }

  TEST_CASE("WasapiGraphRegistry - cancellation removes a callback already copied for publication",
            "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    bool cancelSecond = false;
    std::int32_t firstCalls = 0;
    std::int32_t secondCalls = 0;
    auto secondSub = Subscription{};
    auto firstSub = registry.subscribe("endpoint-a",
                                       [&](flow::Graph const&)
                                       {
                                         ++firstCalls;

                                         if (cancelSecond)
                                         {
                                           secondSub.reset();
                                         }
                                       });
    secondSub = registry.subscribe("endpoint-a", [&](flow::Graph const&) { ++secondCalls; });
    cancelSecond = true;

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});

    CHECK(firstCalls == 2);
    CHECK(secondCalls == 1);

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.25F});
    CHECK(firstCalls == 3);
    CHECK(secondCalls == 1);
  }

  TEST_CASE("WasapiGraphRegistry - late subscriber receives current state", "[audio][unit][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = flow::Graph{};
    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F, .muted = true});

    auto sub = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received = graph; });

    REQUIRE(received.nodes.size() == 2);
    CHECK(received.nodes[1].softwareVolumeNotUnity);
    CHECK(received.nodes[1].maxSoftwareGain == 0.5F);
    CHECK(received.nodes[1].isMuted);
  }

  TEST_CASE("WasapiGraphRegistry - initial callback may publish reentrantly", "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t callCount = 0;

    auto sub = registry.subscribe("endpoint-a",
                                  [&](flow::Graph const&)
                                  {
                                    ++callCount;

                                    if (callCount == 1)
                                    {
                                      registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});
                                    }
                                  });

    CHECK(callCount == 2);
  }

  TEST_CASE("WasapiGraphRegistry - throwing initial callback is rolled back", "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t callCount = 0;

    REQUIRE_THROWS_AS(registry.subscribe("endpoint-a",
                                         [&](flow::Graph const&)
                                         {
                                           ++callCount;
                                           throw std::runtime_error{"callback failed"};
                                         }),
                      std::runtime_error);

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});
    CHECK(callCount == 1);
  }

  TEST_CASE("WasapiGraphRegistry - clear removes state and emits an empty graph", "[audio][unit][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = flow::Graph{};
    auto sub = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received = graph; });
    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});
    REQUIRE_FALSE(received.nodes.empty());

    registry.clear("endpoint-a");

    CHECK(received.nodes.empty());
    CHECK(received.connections.empty());
  }

  TEST_CASE("WasapiGraphRegistry - shutdown clears routes and rejects later publications",
            "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto sub = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});

    registry.shutdown();

    REQUIRE(received.size() == 3);
    CHECK(received.back().nodes.empty());
    CHECK(received.back().connections.empty());

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.25F});
    registry.clear("endpoint-a");
    CHECK(received.size() == 3);

    std::int32_t lateCalls = 0;
    auto lateSub = registry.subscribe("endpoint-a",
                                      [&](flow::Graph const& graph)
                                      {
                                        ++lateCalls;
                                        CHECK(graph.nodes.empty());
                                      });
    CHECK(lateCalls == 0);
    CHECK_FALSE(lateSub);
  }

  TEST_CASE("WasapiGraphRegistry - shutdown from initial callback emits one empty graph",
            "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t initialCalls = 0;
    std::int32_t shutdownCalls = 0;
    bool shutdownRequested = false;

    auto sub = registry.subscribe("endpoint-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    if (graph.nodes.empty())
                                    {
                                      ++shutdownCalls;
                                    }
                                    else
                                    {
                                      ++initialCalls;
                                    }

                                    if (!shutdownRequested)
                                    {
                                      shutdownRequested = true;
                                      registry.shutdown();
                                    }
                                  });

    CHECK(initialCalls == 1);
    CHECK(shutdownCalls == 1);
    CHECK_FALSE(sub);
  }

  TEST_CASE("WasapiGraphRegistry - shutdown callback may cancel a later shutdown delivery",
            "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t secondShutdownCalls = 0;
    auto secondSub = Subscription{};
    auto firstSub = registry.subscribe("endpoint-a",
                                       [&](flow::Graph const& graph)
                                       {
                                         if (graph.nodes.empty())
                                         {
                                           secondSub.reset();
                                         }
                                       });
    secondSub = registry.subscribe("endpoint-a",
                                   [&](flow::Graph const& graph)
                                   {
                                     if (graph.nodes.empty())
                                     {
                                       ++secondShutdownCalls;
                                     }
                                   });

    registry.shutdown();

    CHECK(secondShutdownCalls == 0);
  }

  TEST_CASE("WasapiGraphRegistry - subscription blocked behind callback delivery loses to shutdown",
            "[audio][regression][wasapi][graph]")
  {
    auto registry = WasapiGraphRegistry{};
    auto startSubscription = std::binary_semaphore{0};
    auto subscriptionAttempted = std::binary_semaphore{0};
    auto emptyCalls = std::atomic<std::int32_t>{0};
    auto nonEmptyCalls = std::atomic<std::int32_t>{0};
    auto racedSub = Subscription{};
    auto subscribeThread =
      std::jthread{[&]
                   {
                     startSubscription.acquire();
                     subscriptionAttempted.release();
                     racedSub = registry.subscribe("endpoint-b",
                                                   [&](flow::Graph const& graph)
                                                   {
                                                     auto& calls = graph.nodes.empty() ? emptyCalls : nonEmptyCalls;
                                                     calls.fetch_add(1, std::memory_order_relaxed);
                                                   });
                   }};
    bool coordinateShutdown = false;
    bool shutdownCoordinated = false;
    auto blockerSub = registry.subscribe("endpoint-a",
                                         [&](flow::Graph const&)
                                         {
                                           if (!coordinateShutdown || shutdownCoordinated)
                                           {
                                             return;
                                           }

                                           shutdownCoordinated = true;
                                           startSubscription.release();
                                           subscriptionAttempted.acquire();
                                           registry.shutdown();
                                         });
    coordinateShutdown = true;

    registry.publish({.routeAnchor = "endpoint-a"});
    subscribeThread.join();

    CHECK(emptyCalls.load(std::memory_order_relaxed) == 0);
    CHECK(nonEmptyCalls.load(std::memory_order_relaxed) == 0);
    CHECK_FALSE(racedSub);
  }
} // namespace ao::audio::backend::detail::test
