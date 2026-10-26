// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/backend/detail/WasapiGraphRegistry.h"

#include "lib/audio/backend/detail/WasapiFormat.h"
#include "lib/audio/backend/detail/WasapiFormatObservation.h"
#include "test/audio/backend/WasapiFormatTestSupport.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/NodeFormat.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <semaphore>
#include <thread>
#include <vector>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    flow::Graph withDeclaredSource(flow::Graph graph, SignalFormat const& source)
    {
      graph.nodes.insert(graph.nodes.begin(),
                         flow::Node{.id = "ao-source",
                                    .type = flow::NodeType::Source,
                                    .optFormat = source,
                                    .sourceChannelLayout = {.origin = ChannelLayoutOrigin::Identified}});
      graph.connections.push_back({.sourceId = "ao-source", .destinationId = "wasapi-stream", .isActive = true});
      return graph;
    }
  } // namespace

  TEST_CASE("WasapiGraphRegistry - initial subscription receives a neutral route graph")
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

  TEST_CASE("WasapiGraphRegistry - publish projects input and mix formats independently")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = flow::Graph{};
    std::int32_t callCount = 0;
    auto const inputFormat =
      PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe};
    auto const mixFormat = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Float32Le};
    auto sub = registry.subscribe("endpoint-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    received = graph;
                                    ++callCount;
                                  });

    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = inputFormat,
                      .mixObservation = {.sampleRate = 48000, .channels = 2, .optFormat = mixFormat},
                      .volume = 0.25F,
                      .muted = true});

    CHECK(callCount == 2);
    REQUIRE(received.nodes.size() == 2);
    REQUIRE(received.nodes[0].optFormat);
    REQUIRE(received.nodes[1].optFormat);
    CHECK(std::get<PcmFormat>(*received.nodes[0].optFormat) == inputFormat);
    CHECK(std::get<PcmFormat>(*received.nodes[1].optFormat) == mixFormat);
    CHECK(received.nodes[1].softwareVolumeNotUnity);
    CHECK(received.nodes[1].minSoftwareGain == 0.25F);
    CHECK(received.nodes[1].maxSoftwareGain == 0.25F);
    CHECK(received.nodes[1].isMuted);
  }

  TEST_CASE("WasapiGraphRegistry - subscriptions receive only their route")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t callsA = 0;
    std::int32_t callsB = 0;
    auto subA = registry.subscribe("endpoint-a", [&](flow::Graph const&) { ++callsA; });
    auto subB = registry.subscribe("endpoint-b", [&](flow::Graph const&) { ++callsB; });

    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});

    CHECK(callsA == 2);
    CHECK(callsB == 1);

    registry.publish({.routeAnchor = "endpoint-b", .volume = 0.5F});
    CHECK(callsA == 2);
    CHECK(callsB == 2);

    subA.reset();
    registry.publish({.routeAnchor = "endpoint-a", .volume = 0.75F});
    CHECK(callsA == 2);
    CHECK(callsB == 2);
  }

  TEST_CASE("WasapiGraphRegistry - cancellation removes a callback already copied for publication", "[concurrency]")
  {
    auto registry = WasapiGraphRegistry{};
    bool cancelSecond = false;
    std::int32_t firstCalls = 0;
    std::int32_t secondCalls = 0;
    auto secondSub = utility::ScopedRegistration{};
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

  TEST_CASE("WasapiGraphRegistry - late subscriber receives current state")
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

  TEST_CASE("WasapiGraphRegistry - initial callback may publish reentrantly")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t callCount = 0;
    std::int32_t callbackDepth = 0;
    std::int32_t maximumDepth = 0;
    auto received = std::vector<flow::Graph>{};

    auto sub = registry.subscribe("endpoint-a",
                                  [&](flow::Graph const& graph)
                                  {
                                    ++callbackDepth;
                                    maximumDepth = std::max(maximumDepth, callbackDepth);
                                    ++callCount;
                                    received.push_back(graph);

                                    if (callCount == 1)
                                    {
                                      registry.publish({.routeAnchor = "endpoint-a", .volume = 0.5F});
                                    }

                                    --callbackDepth;
                                  });

    CHECK(callCount == 2);
    CHECK(maximumDepth == 1);
    REQUIRE(received.size() == 2);

    for (auto const& graph : received)
    {
      REQUIRE(graph.nodes.size() == 2);
      CHECK(graph.nodes[0].id == "wasapi-stream");
      CHECK(graph.nodes[1].id == "wasapi-sink");
      CHECK(graph.nodes[1].name == "endpoint-a");
      CHECK(graph.nodes[1].objectPath == "endpoint-a");
      REQUIRE(graph.connections.size() == 1);
      CHECK(graph.connections[0].sourceId == "wasapi-stream");
      CHECK(graph.connections[0].destinationId == "wasapi-sink");
      CHECK(graph.connections[0].isActive);
    }

    CHECK_FALSE(received[0].nodes[1].softwareVolumeNotUnity);
    CHECK(received[1].nodes[1].softwareVolumeNotUnity);
    CHECK(received[1].nodes[1].minSoftwareGain == 0.5F);
    CHECK(received[1].nodes[1].maxSoftwareGain == 0.5F);
  }

  TEST_CASE("WasapiGraphRegistry - clear removes state and emits an empty graph")
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

  TEST_CASE("WasapiGraphRegistry - shutdown clears routes and rejects later publications")
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

  TEST_CASE("WasapiGraphRegistry - shutdown from initial callback emits one empty graph", "[concurrency]")
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

  TEST_CASE("WasapiGraphRegistry - shutdown callback may cancel a later shutdown delivery", "[concurrency]")
  {
    auto registry = WasapiGraphRegistry{};
    std::int32_t secondShutdownCalls = 0;
    auto secondSub = utility::ScopedRegistration{};
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

  TEST_CASE("WasapiGraphRegistry - subscription blocked behind callback delivery loses to shutdown", "[concurrency]")
  {
    auto registry = WasapiGraphRegistry{};
    auto startSubscription = std::binary_semaphore{0};
    auto subscriptionAttempted = std::binary_semaphore{0};
    auto emptyCalls = std::atomic<std::int32_t>{0};
    auto nonEmptyCalls = std::atomic<std::int32_t>{0};
    auto racedSub = utility::ScopedRegistration{};
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

  TEST_CASE("WasapiGraphRegistry - client to mix conversion belongs only to its sink")
  {
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 3;
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3f};
    auto const optClientWave = waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = observeWaveFormat(optClientWave->Format);
    auto const mixRes = observeWaveFormat(mix.Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    auto const evidence = waveChannelDeliveryFromObservation(application, *optClientWave, *mixRes);
    CHECK(evidence.hasSharedConversion);
    auto source = signalFormat(application);
    bool hasUpstreamConversion = false;

    SECTION("a six to two channel conversion produces only one sink finding")
    {
    }

    SECTION("a distinct four to six channel transition still produces its own stream finding")
    {
      source.channels = 4;
      source.speakerMask = 0x33;
      hasUpstreamConversion = true;
    }

    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .mixObservation = *mixRes,
                      .channelDelivery = evidence});
    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    REQUIRE(stream.optFormat);
    REQUIRE(sink.optFormat);
    CHECK(signalFormat(*stream.optFormat).speakerMask ==
          (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
           SPEAKER_BACK_RIGHT));
    CHECK(signalFormat(*sink.optFormat).speakerMask == (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
    CHECK(stream.channelDelivery.isMapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == 0x3fU);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.channelDelivery.isMapped);
    CHECK(sink.channelDelivery.nativeSpeakerMask == 3U);
    CHECK(sink.channelDelivery.hasSharedConversion);

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), source));
    CHECK(analysis.channelConfidence == ChannelConfidence::Verified);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    CHECK(analysis.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(analysis.assessments.size() == 3U);
    auto const& streamAssessment = analysis.assessments[1];
    auto const& sinkAssessment = analysis.assessments[2];
    CHECK(streamAssessment.nodeId == "wasapi-stream");
    CHECK(sinkAssessment.nodeId == "wasapi-sink");
    CHECK(std::ranges::count(streamAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) ==
          (hasUpstreamConversion ? 1 : 0));
    CHECK(std::ranges::count(sinkAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 1);
    auto const conversion =
      std::ranges::find(sinkAssessment.findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
    REQUIRE(conversion != sinkAssessment.findings.end());
    REQUIRE(conversion->optFromFormat);
    REQUIRE(conversion->optToFormat);
    CHECK(signalFormat(*conversion->optFromFormat).channels == 6U);
    CHECK(signalFormat(*conversion->optFromFormat).speakerMask == 0x3fU);
    CHECK(signalFormat(*conversion->optToFormat).channels == 2U);
    CHECK(signalFormat(*conversion->optToFormat).speakerMask == 3U);
  }

  TEST_CASE("WasapiGraphRegistry - a five-point-one side client into a seven-point-one mix converts only at the sink")
  {
    // Independent engine descriptor and accepted-client descriptor models;
    // this test executes neither GetMixFormat nor Initialize.
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 8;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 32;
    mix.Format.nAvgBytesPerSec = 1536000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 0x63f;
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 6, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x60f};
    auto const optClientWave = waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    CHECK(optClientWave->dwChannelMask == (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                                           SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT));
    auto const clientRes = observeWaveFormat(optClientWave->Format);
    auto const mixRes = observeWaveFormat(mix.Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    REQUIRE(mixRes);
    REQUIRE(mixRes->optFormat);
    auto const evidence = waveChannelDeliveryFromObservation(application, *optClientWave, *mixRes);
    CHECK(evidence.isMapped);
    CHECK(evidence.hasSharedConversion);

    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .mixObservation = *mixRes,
                      .channelDelivery = evidence});
    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    REQUIRE(stream.optFormat);
    REQUIRE(sink.optFormat);
    CHECK(signalFormat(*stream.optFormat).speakerMask ==
          (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT |
           SPEAKER_SIDE_RIGHT));
    CHECK(stream.channelDelivery.isMapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == 0x60fU);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    // The engine keeps its own literal seven-point-one role set; the client
    // boundary never invents a zero or shared canonical native mask.
    CHECK(signalFormat(*sink.optFormat).speakerMask ==
          (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
           SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT));
    CHECK(sink.channelDelivery.isMapped);
    CHECK(sink.channelDelivery.nativeSpeakerMask == 0x63fU);
    CHECK(sink.channelDelivery.hasSharedConversion);

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), signalFormat(application)));
    CHECK(analysis.channelConfidence == ChannelConfidence::Verified);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    CHECK(analysis.pipelineQuality == Quality::LinearIntervention);
    REQUIRE(analysis.assessments.size() == 3U);
    CHECK(std::ranges::count(
            analysis.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    auto const& sinkFindings = analysis.assessments[2].findings;
    CHECK(std::ranges::count(sinkFindings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 1);
    auto const conversion = std::ranges::find(sinkFindings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
    REQUIRE(conversion != sinkFindings.end());
    REQUIRE(conversion->optFromFormat);
    REQUIRE(conversion->optToFormat);
    CHECK(signalFormat(*conversion->optFromFormat).channels == 6U);
    CHECK(signalFormat(*conversion->optFromFormat).speakerMask == 0x60fU);
    CHECK(signalFormat(*conversion->optToFormat).channels == 8U);
    CHECK(signalFormat(*conversion->optToFormat).speakerMask == 0x63fU);
  }

  TEST_CASE("WasapiGraphRegistry - parsed engine positions establish only the software sink boundary")
  {
    // Independent engine descriptor and accepted-client descriptor models;
    // this test executes neither GetMixFormat nor Initialize.
    auto mix = WAVEFORMATEXTENSIBLE{};
    mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix.Format.nChannels = 2;
    mix.Format.nSamplesPerSec = 48000;
    mix.Format.wBitsPerSample = 32;
    mix.Format.nBlockAlign = 8;
    mix.Format.nAvgBytesPerSec = 384000;
    mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
    mix.Samples.wValidBitsPerSample = 32; // NOLINT(cppcoreguidelines-pro-type-union-access)
    mix.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    mix.dwChannelMask = 3;
    auto const application =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};
    auto const optClientWave = waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = observeWaveFormat(optClientWave->Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    bool expectedMapping = true;
    std::uint32_t expectedMixMask = 3;
    std::uint32_t expectedFormatMask = 3;
    bool hasMixFormat = true;
    bool hasSharedConversion = false;
    auto expectedConfidence = ChannelConfidence::Verified;

    SECTION("matching complete engine positions establish mapping without a conversion")
    {
      mix.dwChannelMask = 3;
    }

    SECTION("excess high assigned positions normalize two channel mask seven to FL FR")
    {
      mix.dwChannelMask = 0x7;
    }

    SECTION("partial FL is compatible uncertainty without a canonical complete format mask")
    {
      mix.dwChannelMask = 0x1;
      expectedMixMask = 0x1;
      expectedFormatMask = 0;
      expectedMapping = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("partial FC contradicts the FL FR client without being promoted to full mapping")
    {
      mix.dwChannelMask = 0x4;
      expectedMixMask = 0x4;
      expectedFormatMask = 0;
      expectedMapping = false;
      hasSharedConversion = true;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("an independently observed alternate set is mapped and converted")
    {
      mix.dwChannelMask = 0x30;
      expectedMixMask = 0x30;
      expectedFormatMask = 0x30;
      hasSharedConversion = true;
    }

    SECTION("an observed zero mask leaves sink mapping unverified")
    {
      mix.dwChannelMask = 0;
      expectedMixMask = 0;
      expectedFormatMask = 0;
      expectedMapping = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("a known count conversion coexists with unknown sink positions")
    {
      mix.dwChannelMask = 0;
      mix.Format.nChannels = 4;
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      expectedMixMask = 0;
      expectedFormatMask = 0;
      expectedMapping = false;
      hasSharedConversion = true;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("unsupported float width retains actual count and role observations")
    {
      mix.Format.wBitsPerSample = 64;
      // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
      mix.Samples.wValidBitsPerSample = 64; // NOLINT(cppcoreguidelines-pro-type-union-access)
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      mix.dwChannelMask = 0x30;
      expectedMixMask = 0x30;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("unsupported subtype retains independent multichannel mix positions")
    {
      mix.SubFormat = {};
      mix.Format.nChannels = 6;
      mix.Format.nBlockAlign = 24;
      mix.Format.nAvgBytesPerSec = 1152000;
      mix.dwChannelMask = 0x3f;
      expectedMixMask = 0x3f;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("unsupported float width still retains compatible partial roles")
    {
      mix.Format.wBitsPerSample = 64;
      // WAVEFORMATEXTENSIBLE exposes valid precision through the Windows SDK union.
      mix.Samples.wValidBitsPerSample = 64; // NOLINT(cppcoreguidelines-pro-type-union-access)
      mix.Format.nBlockAlign = 16;
      mix.Format.nAvgBytesPerSec = 768000;
      mix.dwChannelMask = 0x1;
      expectedMixMask = 0x1;
      expectedMapping = false;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    SECTION("unsupported subtype still retains a conflicting partial FC constraint")
    {
      mix.SubFormat = {};
      mix.dwChannelMask = 0x4;
      expectedMixMask = 0x4;
      expectedMapping = false;
      hasSharedConversion = true;
      hasMixFormat = false;
      expectedConfidence = ChannelConfidence::Unverified;
    }

    auto const observationRes = observeWaveFormat(mix.Format);
    REQUIRE(observationRes);
    REQUIRE(observationRes->optFormat.has_value() == hasMixFormat);
    CHECK(observationRes->channels == mix.Format.nChannels);
    CHECK(observationRes->speakerMask == expectedMixMask);
    auto const evidence = waveChannelDeliveryFromObservation(application, *optClientWave, *observationRes);
    CHECK(evidence.hasSharedConversion == hasSharedConversion);
    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .mixObservation = *observationRes,
                      .channelDelivery = evidence});

    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    CHECK(stream.channelDelivery.isMapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == 3U);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.channelDelivery.isMapped == expectedMapping);
    CHECK(sink.channelDelivery.nativeSpeakerMask == expectedMixMask);
    CHECK(sink.channelDelivery.hasSharedConversion == hasSharedConversion);
    REQUIRE(stream.optFormat);
    CHECK(signalFormat(*stream.optFormat).speakerMask == 3U);
    REQUIRE(sink.optFormat.has_value() == hasMixFormat);

    if (sink.optFormat)
    {
      CHECK(signalFormat(*sink.optFormat).speakerMask == expectedFormatMask);
    }

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), signalFormat(application)));
    CHECK(analysis.channelConfidence == expectedConfidence);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    CHECK(analysis.fullyVerified == hasMixFormat);
    CHECK(analysis.pipelineQuality == (hasSharedConversion ? Quality::LinearIntervention
                                       : hasMixFormat      ? Quality::LosslessFloat
                                                           : Quality::BitwisePerfect));
    REQUIRE(analysis.assessments.size() == 3U);
    CHECK(std::ranges::count(
            analysis.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    CHECK(std::ranges::count(analysis.assessments[2].findings,
                             QualityFindingKind::ChannelMapping,
                             &QualityFinding::kind) == (hasSharedConversion ? 1 : 0));

    if (hasSharedConversion && hasMixFormat)
    {
      auto const& findings = analysis.assessments[2].findings;
      auto const conversion = std::ranges::find(findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind);
      REQUIRE(conversion != findings.end());
      REQUIRE(conversion->optFromFormat);
      REQUIRE(conversion->optToFormat);
      CHECK(signalFormat(*conversion->optFromFormat).speakerMask == 0x3U);
      CHECK(conversion->optFromSpeakerMask.value_or(signalFormat(*conversion->optFromFormat).speakerMask) == 0x3U);
      // Complete sets may already be carried by the actual format mask.
      CHECK(conversion->optToSpeakerMask.value_or(signalFormat(*conversion->optToFormat).speakerMask) ==
            expectedMixMask);

      if (expectedMixMask == 0x4U)
      {
        REQUIRE(conversion->optToSpeakerMask);
        CHECK(*conversion->optToSpeakerMask == 0x4U);
        CHECK(signalFormat(*conversion->optToFormat).speakerMask == 0U);
      }
    }
  }

  TEST_CASE("WasapiGraphRegistry - incomplete mix sets never promote sink mapping or erase conversion")
  {
    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });

    for (auto const mask : {std::uint32_t{0}, std::uint32_t{1}})
    {
      CAPTURE(mask);
      registry.publish(
        {.routeAnchor = "endpoint-a",
         .mixObservation = {.sampleRate = 48000,
                            .channels = 2,
                            .speakerMask = mask,
                            .optFormat =
                              PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Float32Le}},
         .channelDelivery = {.hasSharedConversion = true}});
      REQUIRE(received.back().nodes.size() == 2U);
      auto const& sink = received.back().nodes[1];
      CHECK_FALSE(sink.channelDelivery.isMapped);
      CHECK(sink.channelDelivery.nativeSpeakerMask == mask);
      CHECK(sink.channelDelivery.hasSharedConversion);
    }
  }

  TEST_CASE("WasapiGraphRegistry - unusable mix evidence never fabricates a sink format, layout or conflict")
  {
    // Parser -> graph -> analyzer regression. The engine's optional mix
    // observation contributes independently usable facts only: contradictory
    // or absent numeric evidence must not become a fabricated sink PCM format
    // or Verified delivery, and reserved positions must not become a layout
    // or a false channel conflict.
    auto mix = ::ao::audio::backend::test::extensibleFloatStereoWave();
    auto application =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 0x3};
    std::uint32_t expectedMixRate = 48000;
    std::uint16_t expectedMixChannels = 2;
    std::uint32_t expectedMixMask = 0;
    bool mixHasFormat = false;
    bool sinkHasFormat = false;
    bool expectedSinkMapping = false;

    SECTION("contradictory mix stride withholds the concrete sink format without erasing roles")
    {
      mix.Format.nBlockAlign = 4;
      mix.Format.nAvgBytesPerSec = 192000;
      expectedMixMask = 3;
      expectedSinkMapping = true;
    }

    SECTION("an unavailable mix observation publishes no fabricated sink format")
    {
      mix.Format.nSamplesPerSec = 0;
      expectedMixRate = 0;
      expectedMixChannels = 0;
    }

    SECTION("reserved engine positions never become a sink layout")
    {
      mix.dwChannelMask = 0x40003;
      mixHasFormat = true;
      sinkHasFormat = true;
    }

    SECTION("wildcard engine positions never fabricate a conflicting role")
    {
      // A partial-keep regression would retain FL here and report a false
      // conflict with the client's BL/BR set; full erasure stays unknown.
      application.speakerMask = 0x30;
      mix.dwChannelMask = 0x80000001;
      mixHasFormat = true;
      sinkHasFormat = true;
    }

    auto const optClientWave = waveFormatFromPcmFormat(application);
    REQUIRE(optClientWave);
    auto const clientRes = observeWaveFormat(optClientWave->Format);
    REQUIRE(clientRes);
    REQUIRE(clientRes->optFormat);
    auto const mixRes = observeWaveFormat(mix.Format);
    REQUIRE(mixRes);
    CHECK(mixRes->sampleRate == expectedMixRate);
    CHECK(mixRes->channels == expectedMixChannels);
    CHECK(mixRes->speakerMask == expectedMixMask);
    CHECK(mixRes->optFormat.has_value() == mixHasFormat);

    if (mixRes->optFormat)
    {
      CHECK(mixRes->optFormat->speakerMask == 0U);
    }

    auto const evidence = waveChannelDeliveryFromObservation(application, *optClientWave, *mixRes);
    CHECK(evidence.isMapped);
    CHECK_FALSE(evidence.hasSharedConversion);

    auto registry = WasapiGraphRegistry{};
    auto received = std::vector<flow::Graph>{};
    auto subscription = registry.subscribe("endpoint-a", [&](flow::Graph const& graph) { received.push_back(graph); });
    registry.publish({.routeAnchor = "endpoint-a",
                      .optInputFormat = clientRes->optFormat,
                      .mixObservation = *mixRes,
                      .channelDelivery = evidence});

    REQUIRE(received.size() == 2U);
    REQUIRE(received.back().nodes.size() == 2U);
    auto const& stream = received.back().nodes[0];
    auto const& sink = received.back().nodes[1];
    CHECK(stream.channelDelivery.isMapped);
    CHECK(stream.channelDelivery.nativeSpeakerMask == application.speakerMask);
    CHECK_FALSE(stream.channelDelivery.hasSharedConversion);
    CHECK(sink.optFormat.has_value() == sinkHasFormat);
    CHECK(sink.channelDelivery.isMapped == expectedSinkMapping);
    CHECK(sink.channelDelivery.nativeSpeakerMask == expectedMixMask);
    CHECK_FALSE(sink.channelDelivery.hasSharedConversion);

    if (sink.optFormat)
    {
      // A retained precision fact without roles is not a layout.
      CHECK(signalFormat(*sink.optFormat).speakerMask == 0U);
    }

    auto const analysis = analyzeAudioQuality(withDeclaredSource(received.back(), signalFormat(application)));
    CHECK(analysis.channelConfidence == ChannelConfidence::Unverified);
    CHECK(analysis.fullyVerified == sinkHasFormat);
    CHECK(analysis.sourceQuality == Quality::BitwisePerfect);
    REQUIRE(analysis.assessments.size() == 3U);
    CHECK(std::ranges::count(
            analysis.assessments[1].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
    CHECK(std::ranges::count(
            analysis.assessments[2].findings, QualityFindingKind::ChannelMapping, &QualityFinding::kind) == 0);
  }
} // namespace ao::audio::backend::detail::test
