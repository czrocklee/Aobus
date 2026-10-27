// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlacFrameTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "lib/audio/detail/TrackPreparation.h"
#include "test/TestFixtureSupport.h"
#include "test/media/wav/TestWav.h"
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    std::vector<std::uint8_t> stereoAlacBytes()
    {
      // A bounded but unsupported chan tag keeps this source Unknown so the
      // existing 0-vs-3 stereo identity alias contract remains exercised.
      return buildAlacUncompressedFileBytes(
        AlacFrameTestOptions{.sampleEntrySuffix = makeAlacChannelLayoutAtom(0xDEAD0002)});
    }

    std::vector<std::uint8_t> stereoFlacBytes()
    {
      auto bytes = buildFlacStreaminfoBytes(16);
      auto const markers = std::to_array<std::int32_t>({0x3635, 0x4847});
      auto const frame = buildFlacVerbatimFrameBytes(0, 48000, 2, 16, markers);
      bytes.insert(bytes.end(), frame.begin(), frame.end());
      return bytes;
    }

    std::vector<std::uint8_t> stereoFlacPcm()
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        // Literal oracle, independent of encoded markers or a decoder result.
        bytes.insert(bytes.end(), {0x35, 0x36, 0x47, 0x48});
      }

      return bytes;
    }
  } // namespace

  TEST_CASE("Engine - stereo identity opt-in splices real WAV or FLAC with ALAC without rewriting source facts",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const identifiedFirst = GENERATE(false, true);
    bool const identifiedFlac = GENERATE(false, true);
    CAPTURE(identifiedFirst, identifiedFlac);
    auto const compressed = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto const identifiedPcm =
      identifiedFlac ? stereoFlacPcm() : std::vector<std::uint8_t>{0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C};
    auto const identifiedBytes = identifiedFlac
                                   ? stereoFlacBytes()
                                   : ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                             .channels = 2,
                                                             .sampleRate = 48000,
                                                             .channelMask = 3,
                                                             .audioData = identifiedPcm});
    auto const identified = ao::test::TempFile{identifiedBytes, identifiedFlac ? ".flac" : ".wav"};
    auto compressedPcm = std::vector<std::uint8_t>{};

    for (std::uint32_t frame = 0; frame < 16; ++frame)
    {
      // Independent S16LE oracle, never derived from encoded fixture bytes.
      compressedPcm.insert(compressedPcm.end(), {0x52, 0x51, 0x64, 0x63});
    }

    auto const& firstPath = identifiedFirst ? identified.path : compressed.path;
    auto const& nextPath = identifiedFirst ? compressed.path : identified.path;
    auto const& firstPcm = identifiedFirst ? identifiedPcm : compressedPcm;
    auto const& nextPcm = identifiedFirst ? compressedPcm : identifiedPcm;
    std::uint32_t const firstMask = identifiedFirst ? 3U : 0U;
    std::uint32_t const nextMask = identifiedFirst ? 0U : 3U;
    auto const firstOrigin = identifiedFirst ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown;
    auto const nextOrigin = identifiedFirst ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified;
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setStereoIdentityReuse(true);
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto advancedEvents = std::vector<Engine::TrackAdvanced>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced(
      [&](Engine::TrackAdvanced const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          advancedEvents.push_back(event);
        }

        advanced.notify();
      });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    auto const firstItem = makePlaybackItem(firstPath);
    auto const nextItem = makePlaybackItem(nextPath);
    engine.play(firstItem);
    auto const firstStatus = engine.status();
    REQUIRE(firstStatus.transport == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK(firstStatus.routeState.sourceFormat.sampleRate == 48000);
    CHECK(firstStatus.routeState.sourceFormat.channels == 2);
    CHECK(firstStatus.routeState.sourceFormat.precisionBits == 16);
    CHECK(firstStatus.routeState.sourceFormat.speakerMask == firstMask);
    CHECK(firstStatus.routeState.decoderOutputFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(firstStatus.routeState.decoderOutputFormat.speakerMask == firstMask);
    CHECK(firstStatus.routeState.engineOutputFormat.speakerMask == firstMask);
    CHECK(firstStatus.routeState.sourceChannelLayout.origin == firstOrigin);
    auto const installedFormat = backend->currentFormat();
    CHECK(installedFormat.speakerMask == firstMask);
    CHECK(frameBytes(installedFormat) == 4);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);

    // A single render crosses the boundary with both complete literal streams.
    auto expected = firstPcm;
    expected.insert(expected.end(), nextPcm.begin(), nextPcm.end());
    auto output = std::vector<std::byte>(expected.size());
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == expected.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{expected})));
    CHECK(rendered.positionFrameOffset == firstPcm.size() / 4U);
    CHECK(rendered.positionFrames == nextPcm.size() / 4U);
    target->handlePositionAdvanced(rendered.positionFrames);
    REQUIRE(advanced.tryWaitForCount(1));
    CHECK(advanced.count() == 1);
    CHECK(ended.count() == 0);
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
    auto const status = engine.status();
    CHECK(status.transport == Transport::Playing);
    CHECK(status.routeState.sourceFormat.sampleRate == 48000);
    CHECK(status.routeState.sourceFormat.channels == 2);
    CHECK(status.routeState.sourceFormat.precisionBits == 16);
    CHECK(status.routeState.sourceFormat.speakerMask == nextMask);
    CHECK(status.routeState.decoderOutputFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(status.routeState.decoderOutputFormat.speakerMask == nextMask);
    CHECK(status.routeState.engineOutputFormat.speakerMask == nextMask);
    CHECK(status.routeState.sourceChannelLayout.origin == nextOrigin);

    // Reverse the alias after worker promotion: the logical mask changes back,
    // while the installed mode and its open-history evidence remain untouched.
    auto const thirdItem = makePlaybackItem(firstPath);
    auto const thirdRes = engine.setNext(thirdItem);
    REQUIRE(thirdRes);
    CHECK(thirdRes->itemId == thirdItem.id);
    REQUIRE(thirdRes->transition == Engine::PreparedTransitionMode::Gapless);
    auto thirdOutput = std::vector<std::byte>(firstPcm.size());
    auto const thirdRendered = target->renderPcm(thirdOutput);
    REQUIRE(thirdRendered.bytesWritten == thirdOutput.size());
    CHECK_FALSE(thirdRendered.drained);
    CHECK(std::ranges::equal(thirdOutput, utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    CHECK(thirdRendered.positionFrameOffset == 0);
    CHECK(thirdRendered.positionFrames == firstPcm.size() / 4U);
    target->handlePositionAdvanced(thirdRendered.positionFrames);
    REQUIRE(advanced.tryWaitForCount(2));
    CHECK(advanced.count() == 2);
    CHECK(ended.count() == 0);
    CHECK(engine.isCurrentPlaybackItem(thirdItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    auto const thirdStatus = engine.status();
    CHECK(thirdStatus.transport == Transport::Playing);
    CHECK(thirdStatus.routeState.sourceFormat.speakerMask == firstMask);
    CHECK(thirdStatus.routeState.decoderOutputFormat.speakerMask == firstMask);
    CHECK(thirdStatus.routeState.engineOutputFormat.speakerMask == firstMask);
    CHECK(thirdStatus.routeState.sourceChannelLayout.origin == firstOrigin);
    CHECK(backend->currentFormat() == installedFormat);
    CHECK(backend->target() == target);
    auto const events = backend->events();
    CHECK(std::ranges::count(events, std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(events, std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    auto const lock = std::scoped_lock{callbackMutex};
    REQUIRE(advancedEvents.size() == 2);
    CHECK(advancedEvents.front().itemId == nextItem.id);
    CHECK(advancedEvents.front().input.filePath == nextPath);
    CHECK(advancedEvents.front().generation == preparedRes->generation);
    CHECK(advancedEvents.back().itemId == thirdItem.id);
    CHECK(advancedEvents.back().input.filePath == firstPath);
    CHECK(advancedEvents.back().generation == thirdRes->generation);
  }

  TEST_CASE("Engine - strict stereo backend drains real FLAC and ALAC without promoting the successor",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const flacFirst = GENERATE(false, true);
    CAPTURE(flacFirst);
    auto const flac = ao::test::TempFile{stereoFlacBytes(), ".flac"};
    auto const alac = ao::test::TempFile{stereoAlacBytes(), ".m4a"};
    auto const flacPcm = stereoFlacPcm();
    auto alacPcm = std::vector<std::uint8_t>{};

    for (std::uint32_t frame = 0; frame < 16; ++frame)
    {
      alacPcm.insert(alacPcm.end(), {0x52, 0x51, 0x64, 0x63});
    }

    auto const& firstPcm = flacFirst ? flacPcm : alacPcm;
    auto const firstItem = makePlaybackItem(flacFirst ? flac.path : alac.path);
    auto const nextItem = makePlaybackItem(flacFirst ? alac.path : flac.path);
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto failed = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failed.notify(); });
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const info = engine.status().routeState;
    CHECK(info.sourceFormat.speakerMask == (flacFirst ? 3U : 0U));
    CHECK(info.sourceChannelLayout.origin ==
          (flacFirst ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown));
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::vector<std::byte>(firstPcm.size() * 2U, std::byte{0xA5});
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == firstPcm.size());
    CHECK(std::ranges::equal(std::span<std::byte const>{output}.first(firstPcm.size()),
                             utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    CHECK(std::ranges::all_of(std::span<std::byte const>{output}.subspan(firstPcm.size()),
                              [](std::byte value) { return value == std::byte{0xA5}; }));
    target->handlePositionAdvanced(rendered.positionFrames);
    auto const drain = target->renderPcm(output);
    CHECK(drain.bytesWritten == 0);
    REQUIRE(drain.drained);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK(engine.status().routeState.sourceFormat == info.sourceFormat);
    CHECK(engine.status().routeState.sourceChannelLayout == info.sourceChannelLayout);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    engine.shutdown();
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 1);
    CHECK(failed.count() == 0);
  }

  TEST_CASE("Engine - default strict stereo backend drains mask aliases but retains equal-mask reuse",
            "[audio][unit][engine][gapless][concurrency]")
  {
    auto const masks = GENERATE(std::pair<std::uint32_t, std::uint32_t>{0, 3},
                                std::pair<std::uint32_t, std::uint32_t>{3, 0},
                                std::pair<std::uint32_t, std::uint32_t>{3, 3});
    CAPTURE(masks.first, masks.second);
    auto const firstFormat =
      PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = masks.first};
    auto nextFormat = firstFormat;
    nextFormat.speakerMask = masks.second;
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x23}, std::byte{0x24}};
    auto const nextData = std::vector{std::byte{0x35}, std::byte{0x36}, std::byte{0x47}, std::byte{0x48}};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(firstFormat), .data = firstData},
                           {.path = "next.flac", .info = makeScriptedStreamInfo(nextFormat), .data = nextData},
                         })};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    auto const firstItem = makePlaybackItem(PlaybackInput{.filePath = "first.flac"});
    auto const nextItem = makePlaybackItem(PlaybackInput{.filePath = "next.flac"});
    engine.play(firstItem);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    bool const equalMasks = masks.first == masks.second;
    CHECK(preparedRes->transition ==
          (equalMasks ? Engine::PreparedTransitionMode::Gapless : Engine::PreparedTransitionMode::DrainFallback));
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());
    CHECK(std::ranges::equal(output, firstData));

    if (auto const nextRender = target->renderPcm(output); equalMasks)
    {
      REQUIRE(nextRender.bytesWritten == output.size());
      CHECK_FALSE(nextRender.drained);
      CHECK(std::ranges::equal(output, nextData));
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(engine.isCurrentPlaybackItem(nextItem.id));
      CHECK(ended.count() == 0);
    }
    else
    {
      CHECK(nextRender.bytesWritten == 0);
      REQUIRE(nextRender.drained);
      CHECK_FALSE(engine.clearNext());
      backend->emitDrainComplete();
      REQUIRE(ended.tryWaitForCount(1));
      CHECK(advanced.count() == 0);
      CHECK(engine.transport() == Transport::Idle);
    }

    CHECK(backend->currentFormat() == firstFormat);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
  }

  TEST_CASE("FakeCapturingBackend - stereo identity opt-in rejects other PCM geometry and known roles",
            "[audio][unit][backend]")
  {
    auto const installed =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setStereoIdentityReuse(true);
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), makeScriptedEngineDecoderFactory(installed)};
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "installed.flac"}));
    REQUIRE(engine.transport() == Transport::Playing);
    auto alias = installed;
    alias.speakerMask = 0;
    REQUIRE(backend->canReuseFormat(installed, alias));
    CHECK(backend->canReuseFormat(alias, installed));
    CHECK_FALSE(isSamePcmMode(installed, alias));
    auto candidate = alias;

    SECTION("alternate known roles")
    {
      candidate.speakerMask = 0x30;
    }

    SECTION("mono")
    {
      candidate.channels = 1;
    }

    SECTION("multichannel")
    {
      candidate.channels = 4;
    }

    SECTION("sample rate")
    {
      candidate.sampleRate = 44100;
    }

    SECTION("encoding")
    {
      candidate.encoding = SampleEncoding::Signed32Le;
    }

    SECTION("invalid rate pair")
    {
      auto current = installed;
      current.sampleRate = 0;
      candidate.sampleRate = 0;
      CHECK_FALSE(backend->canReuseFormat(current, candidate));
    }

    SECTION("invalid encoding pair")
    {
      auto current = installed;
      current.encoding = SampleEncoding::Unknown;
      candidate.encoding = SampleEncoding::Unknown;
      CHECK_FALSE(backend->canReuseFormat(current, candidate));
    }

    CHECK_FALSE(backend->canReuseFormat(installed, candidate));
  }

  TEST_CASE("Engine - stereo identity reuse retains the endpoint precision veto behind a wider client",
            "[audio][unit][engine][gapless][concurrency]")
  {
    bool const promoteFirst = GENERATE(false, true);
    CAPTURE(promoteFirst);
    auto const firstFormat =
      PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};
    auto aliasFormat = firstFormat;
    aliasFormat.speakerMask = 0;
    auto wideFormat = firstFormat;
    wideFormat.encoding = SampleEncoding::Signed24PackedLe;
    wideFormat.speakerMask = promoteFirst ? 3U : 0U;
    // Scripted sources already supply the requested Signed32Le client bytes.
    auto const firstData = std::vector{std::byte{0},
                                       std::byte{0},
                                       std::byte{0x11},
                                       std::byte{0x12},
                                       std::byte{0},
                                       std::byte{0},
                                       std::byte{0x23},
                                       std::byte{0x24}};
    auto const aliasData = std::vector{std::byte{0},
                                       std::byte{0},
                                       std::byte{0x35},
                                       std::byte{0x36},
                                       std::byte{0},
                                       std::byte{0},
                                       std::byte{0x47},
                                       std::byte{0x48}};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setStereoIdentityReuse(true);
    backendPtr->setSelectedEncoding(SampleEncoding::Signed32Le);
    backendPtr->setConfirmedEndpointPrecision(16);
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(firstFormat), .data = firstData},
                           {.path = "alias.flac", .info = makeScriptedStreamInfo(aliasFormat), .data = aliasData},
                           {.path = "wide.flac", .info = makeScriptedStreamInfo(wideFormat), .data = firstData},
                         })};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.transport() == Transport::Playing);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 8>{};

    if (promoteFirst)
    {
      auto const aliasItem = makePlaybackItem(PlaybackInput{.filePath = "alias.flac"});
      auto const aliasRes = engine.setNext(aliasItem);
      REQUIRE(aliasRes);
      REQUIRE(aliasRes->transition == Engine::PreparedTransitionMode::Gapless);
      REQUIRE(target->renderPcm(output).bytesWritten == output.size());
      CHECK(std::ranges::equal(output, firstData));
      REQUIRE(target->renderPcm(output).bytesWritten == output.size());
      CHECK(std::ranges::equal(output, aliasData));
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(engine.isCurrentPlaybackItem(aliasItem.id));
      CHECK(engine.status().routeState.sourceFormat.speakerMask == 0);
    }

    auto const currentClient = engine.status().routeState.engineOutputFormat;
    auto const wideClient = pcmFormat(signalFormat(wideFormat), SampleEncoding::Signed32Le);
    REQUIRE(backend->canReuseFormat(currentClient, wideClient));
    auto const wideRes = engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "wide.flac"}));
    REQUIRE(wideRes);
    CHECK(wideRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    CHECK_FALSE(engine.clearNext());

    if (!promoteFirst)
    {
      REQUIRE(target->renderPcm(output).bytesWritten == output.size());
      CHECK(std::ranges::equal(output, firstData));
    }

    auto const drain = target->renderPcm(output);
    CHECK(drain.bytesWritten == 0);
    REQUIRE(drain.drained);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(advanced.count() == (promoteFirst ? 1U : 0U));
    CHECK(backend->currentFormat().encoding == SampleEncoding::Signed32Le);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
  }

  TEST_CASE("Engine - lookahead adoption rechecks withdrawn backend stereo identity permission",
            "[audio][unit][engine][gapless][concurrency]")
  {
    auto const firstFormat =
      PcmFormat{.sampleRate = 1000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = 3};
    auto nextFormat = firstFormat;
    nextFormat.speakerMask = 0;
    auto const firstData = std::vector{std::byte{0x11}, std::byte{0x12}, std::byte{0x23}, std::byte{0x24}};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    backendPtr->setStereoIdentityReuse(true);
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory({
                           {.path = "first.flac", .info = makeScriptedStreamInfo(firstFormat), .data = firstData},
                           {.path = "next.flac",
                            .info = makeScriptedStreamInfo(nextFormat),
                            .data = {std::byte{0x35}, std::byte{0x36}, std::byte{0x47}, std::byte{0x48}}},
                         })};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    auto const currentItem = makePlaybackItem(PlaybackInput{.filePath = "first.flac"});
    auto const nextItem = makePlaybackItem(PlaybackInput{.filePath = "next.flac"});
    engine.play(currentItem);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const generation = engine.playbackGeneration();
    auto preparationRes = detail::TrackPreparation::capture(
      engine, nextItem, std::chrono::milliseconds{0}, detail::TrackPreparation::Purpose::GaplessLookahead);
    REQUIRE(preparationRes);
    REQUIRE(preparationRes->inspect());
    REQUIRE(preparationRes->selectPrewarmFormat(engine));
    REQUIRE(preparationRes->needsWorker());
    REQUIRE(preparationRes->prepare());
    REQUIRE(backend->canReuseFormat(firstFormat, nextFormat));

    // Public internal runtime phases isolate selection from adoption without
    // blocking a decoder callback, sleeping, or reaching into a private queue.
    backend->setStereoIdentityReuse(false);
    auto const adoptedRes = std::move(*preparationRes).adoptNext(engine);
    REQUIRE(adoptedRes);
    CHECK(adoptedRes->itemId == nextItem.id);
    CHECK(adoptedRes->generation == generation);
    CHECK(adoptedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    CHECK_FALSE(engine.clearNext());
    CHECK(engine.isCurrentPlaybackItem(currentItem.id));
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 4>{};
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());
    CHECK(std::ranges::equal(output, firstData));
    auto const drain = target->renderPcm(output);
    CHECK(drain.bytesWritten == 0);
    REQUIRE(drain.drained);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(advanced.count() == 0);
    CHECK(engine.transport() == Transport::Idle);
    CHECK(std::ranges::count(backend->events(), std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
  }
} // namespace ao::audio::test
