// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlacFrameTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    struct LayoutRow final
    {
      std::uint8_t channels;
      std::uint32_t tag;
      std::uint32_t mask;
      std::vector<std::uint8_t> frame;
    };

    struct DrainRow final
    {
      std::string_view name;
      std::uint8_t channels;
      std::uint32_t knownTag;
      std::uint32_t knownMask;
      std::vector<std::uint8_t> knownFrame;
      std::uint32_t otherTag;
      std::uint32_t otherMask;
      std::vector<std::uint8_t> otherFrame;
      std::uint32_t otherBitmap = 0;
      bool truncateChan = false;
    };

    std::vector<std::uint8_t> repeatFrame(std::span<std::uint8_t const> frame)
    {
      auto pcm = std::vector<std::uint8_t>{};

      for (std::uint32_t frameIndex = 0; frameIndex < 16; ++frameIndex)
      {
        pcm.insert(pcm.end(), frame.begin(), frame.end());
      }

      return pcm;
    }

    std::vector<std::uint8_t> alacLayoutBytes(std::uint8_t channels,
                                              std::uint32_t tag,
                                              std::uint32_t bitmap = 0,
                                              bool truncateChan = false)
    {
      auto suffix = tag == 0 ? std::vector<std::uint8_t>{} : makeAlacChannelLayoutAtom(tag, bitmap);

      if (truncateChan)
      {
        REQUIRE_FALSE(suffix.empty());
        // Only the optional chan extent is broken; the cookie and packets
        // remain valid and must still open with Unknown layout evidence.
        suffix.pop_back();
      }

      return buildAlacUncompressedFileBytes(
        AlacFrameTestOptions{.channels = channels, .sampleEntrySuffix = std::move(suffix)});
    }

    void checkSourceRoute(Engine::Status const& status, std::uint8_t channels, std::uint32_t mask, AudioCodec codec)
    {
      CHECK(status.transport == Transport::Playing);
      CHECK(status.routeState.codec == codec);
      CHECK_FALSE(status.routeState.isLossySource);
      CHECK(status.routeState.sourceFormat.sampleRate == 48000);
      CHECK(status.routeState.sourceFormat.channels == channels);
      CHECK(status.routeState.sourceFormat.precisionBits == 16);
      CHECK(status.routeState.sourceFormat.speakerMask == mask);
      CHECK(
        status.routeState.sourceChannelLayout ==
        ChannelLayoutEvidence{.origin = mask == 0 ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified});
      auto const expected = PcmFormat{
        .sampleRate = 48000, .channels = channels, .encoding = SampleEncoding::Signed16Le, .speakerMask = mask};
      CHECK(status.routeState.decoderOutputFormat == expected);
      CHECK(status.routeState.engineOutputFormat == expected);
    }

    void checkGaplessTransition(Engine::PlaybackItem const& firstItem,
                                Engine::PlaybackItem const& nextItem,
                                std::uint8_t channels,
                                std::uint32_t mask,
                                AudioCodec firstCodec,
                                AudioCodec nextCodec,
                                std::vector<std::uint8_t> const& firstPcm,
                                std::vector<std::uint8_t> const& nextPcm)
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      // The backend retains its default strict policy: no stereo alias opt-in.
      auto* const backend = backendPtr.get();
      auto advanced = CallbackLatch{};
      auto ended = CallbackLatch{};
      auto failed = CallbackLatch{};
      auto settled = CallbackLatch{};
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
      engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failed.notify(); });
      engine.play(firstItem);
      REQUIRE(engine.transport() == Transport::Playing);
      checkSourceRoute(engine.status(), channels, mask, firstCodec);
      CHECK(engine.isCurrentPlaybackItem(firstItem.id));
      auto const firstGeneration = engine.playbackGeneration();
      auto const installedFormat = backend->currentFormat();
      CHECK(installedFormat == engine.status().routeState.engineOutputFormat);
      auto* const target = backend->target();
      REQUIRE(target != nullptr);
      auto const preparedRes = engine.setNext(nextItem);
      REQUIRE(preparedRes);
      CHECK(preparedRes->itemId == nextItem.id);
      CHECK(preparedRes->generation == firstGeneration);
      REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);
      checkSourceRoute(engine.status(), channels, mask, firstCodec);
      CHECK(engine.isCurrentPlaybackItem(firstItem.id));
      CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));

      // One render must concatenate both independently authored canonical
      // streams, not merely report that a successor was adopted.
      auto expected = firstPcm;
      expected.insert(expected.end(), nextPcm.begin(), nextPcm.end());
      auto output = std::vector<std::byte>(expected.size(), std::byte{0xA5});
      auto const rendered = target->renderPcm(output);
      REQUIRE(rendered.bytesWritten == expected.size());
      CHECK_FALSE(rendered.drained);
      CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{expected})));
      CHECK(rendered.positionFrameOffset == 16);
      CHECK(rendered.positionFrames == 16);
      target->handlePositionAdvanced(rendered.positionFrames);
      REQUIRE(advanced.tryWaitForCount(1));
      CHECK(engine.isCurrentPlaybackItem(nextItem.id));
      CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
      CHECK(engine.playbackGeneration() == firstGeneration);
      checkSourceRoute(engine.status(), channels, mask, nextCodec);
      CHECK(backend->target() == target);
      CHECK(backend->currentFormat() == installedFormat);
      auto const history = backend->events();
      CHECK(std::ranges::count(history, std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(history, std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
      CHECK(std::ranges::count(history, std::string{"close"}, &FakeCapturingBackend::Event::name) == 0);
      CHECK(std::ranges::count(history, std::string{"stop"}, &FakeCapturingBackend::Event::name) == 0);
      auto const opened = std::ranges::find(history, std::string{"open"}, &FakeCapturingBackend::Event::name);
      REQUIRE(opened != history.end());
      CHECK(opened->format == installedFormat);

      // FIFO event-worker fence, then join all producers before asserting
      // callback non-emission or destroying their captured storage.
      engine.defer([&] { settled.notify(); });
      REQUIRE(settled.tryWaitForCount(1));
      engine.shutdown();
      CHECK(advanced.count() == 1);
      CHECK(ended.count() == 0);
      CHECK(failed.count() == 0);
      auto const lock = std::scoped_lock{callbackMutex};
      REQUIRE(advancedEvents.size() == 1);
      CHECK(advancedEvents.front().itemId == nextItem.id);
      CHECK(advancedEvents.front().input.filePath == nextItem.input.filePath);
      CHECK(advancedEvents.front().generation == preparedRes->generation);
    }
  } // namespace

  TEST_CASE("Engine - matching real WAV and ALAC layouts splice strictly across one through eight channels",
            "[audio][integration][engine][gapless][concurrency]")
  {
    // Literal S16LE canonical frames, authored from declared roles rather than
    // production mapping tables or decoder output. Mono/stereo omit chan.
    auto const row = GENERATE(
      LayoutRow{1, 0, 0x4, {0x52, 0x51}},
      LayoutRow{2, 0, 0x3, {0x52, 0x51, 0x64, 0x63}},
      LayoutRow{3, 0x00720003, 0x7, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51}},
      LayoutRow{4, 0x00740004, 0x107, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8}},
      LayoutRow{5, 0x00780005, 0x37, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8, 0x2A, 0x29}},
      LayoutRow{6, 0x007C0006, 0x3F, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xC4, 0xC4, 0xE8, 0xE8, 0x2A, 0x29}},
      LayoutRow{
        7, 0x008E0007, 0x13F, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0x4E, 0x4D, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4}},
      LayoutRow{8,
                0x007F0008,
                0xFF,
                {0xE8, 0xE8, 0x2A, 0x29, 0x52, 0x51, 0xA0, 0xA0, 0xC4, 0xC4, 0x4E, 0x4D, 0x64, 0x63, 0x76, 0x75}});
    bool const wavFirst = GENERATE(false, true);
    CAPTURE(row.channels, row.tag, row.mask, wavFirst);
    auto const alac = ao::test::TempFile{alacLayoutBytes(row.channels, row.tag), ".m4a"};
    auto const alacPcm = repeatFrame(row.frame);
    // WAV is already canonical, with intentionally different slot markers so
    // the cross-boundary byte comparison proves which source supplied each half.
    constexpr auto kWavFrame = std::to_array<std::uint8_t>(
      {0x11, 0x12, 0x21, 0x22, 0x31, 0x32, 0x41, 0x42, 0x61, 0x62, 0x71, 0x72, 0x81, 0x82, 0x91, 0x92});
    auto const wavPcm =
      repeatFrame(std::span<std::uint8_t const>{kWavFrame}.first(static_cast<std::size_t>(row.channels) * 2U));
    auto const wav =
      ao::test::TempFile{ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                 .channels = row.channels,
                                                 .sampleRate = 48000,
                                                 .channelMask = row.mask,
                                                 .audioData = wavPcm}),
                         ".wav"};
    auto const firstItem = makePlaybackItem(wavFirst ? wav.path : alac.path);
    auto const nextItem = makePlaybackItem(wavFirst ? alac.path : wav.path);
    checkGaplessTransition(firstItem,
                           nextItem,
                           row.channels,
                           row.mask,
                           wavFirst ? AudioCodec::Wav : AudioCodec::Alac,
                           wavFirst ? AudioCodec::Alac : AudioCodec::Wav,
                           wavFirst ? wavPcm : alacPcm,
                           wavFirst ? alacPcm : wavPcm);
  }

  TEST_CASE("Engine - equal-role ALAC declarations with different arrival orders splice canonical PCM",
            "[audio][integration][engine][gapless][concurrency]")
  {
    bool const centerFirst = GENERATE(false, true);
    CAPTURE(centerFirst);
    auto const centerLeftRight = ao::test::TempFile{alacLayoutBytes(4, 0x00740004), ".m4a"};
    auto const leftRightCenter = ao::test::TempFile{alacLayoutBytes(4, 0x00730004), ".m4a"};
    // Tag 116 is C L R BC; tag 115 is L R C BC. Identical raw arrival
    // markers therefore produce different canonical FL FR FC BC frames.
    auto const centerPcm = repeatFrame(std::to_array<std::uint8_t>({0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8}));
    auto const leftPcm = repeatFrame(std::to_array<std::uint8_t>({0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8}));
    auto const firstItem = makePlaybackItem(centerFirst ? centerLeftRight.path : leftRightCenter.path);
    auto const nextItem = makePlaybackItem(centerFirst ? leftRightCenter.path : centerLeftRight.path);
    checkGaplessTransition(firstItem,
                           nextItem,
                           4,
                           0x107,
                           AudioCodec::Alac,
                           AudioCodec::Alac,
                           centerFirst ? centerPcm : leftPcm,
                           centerFirst ? leftPcm : centerPcm);
  }

  TEST_CASE("Engine - real ALAC unknown identities and different speaker roles drain before explicit reopen",
            "[audio][integration][engine][gapless][concurrency]")
  {
    auto const row =
      GENERATE(DrainRow{"mono unsupported declaration", 1, 0, 0x4, {0x52, 0x51}, 0xDEAD0001, 0, {0x52, 0x51}},
               DrainRow{"three channels without chan",
                        3,
                        0x00720003,
                        0x7,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51},
                        0,
                        0,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75}},
               DrainRow{"ambiguous tag 113",
                        3,
                        0x00720003,
                        0x7,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51},
                        0x00710003,
                        0,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75}},
               DrainRow{"unsupported three-channel tag",
                        3,
                        0x00720003,
                        0x7,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51},
                        0xDEAD0003,
                        0,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75}},
               DrainRow{"truncated chan",
                        3,
                        0x00720003,
                        0x7,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51},
                        0x00720003,
                        0,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75},
                        0,
                        true},
               DrainRow{"center and back-center versus quad",
                        4,
                        0x00740004,
                        0x107,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8},
                        0x006C0004,
                        0x33,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8}},
               DrainRow{"back versus side surrounds",
                        6,
                        0x007C0006,
                        0x3F,
                        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xC4, 0xC4, 0xE8, 0xE8, 0x2A, 0x29},
                        0x00010000,
                        0x60F,
                        {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4},
                        0x60F});
    bool const knownFirst = GENERATE(false, true);
    CAPTURE(row.name, row.channels, knownFirst);
    auto const known = ao::test::TempFile{alacLayoutBytes(row.channels, row.knownTag), ".m4a"};
    auto const other =
      ao::test::TempFile{alacLayoutBytes(row.channels, row.otherTag, row.otherBitmap, row.truncateChan), ".m4a"};
    auto const knownPcm = repeatFrame(row.knownFrame);
    auto const otherPcm = repeatFrame(row.otherFrame);
    auto const& firstPcm = knownFirst ? knownPcm : otherPcm;
    auto const& nextPcm = knownFirst ? otherPcm : knownPcm;
    auto const firstMask = knownFirst ? row.knownMask : row.otherMask;
    auto const nextMask = knownFirst ? row.otherMask : row.knownMask;
    auto const firstItem = makePlaybackItem(knownFirst ? known.path : other.path);
    auto const nextItem = makePlaybackItem(knownFirst ? other.path : known.path);
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    // Even an opted-in backend must not apply its stereo-only alias permission
    // to mono or multichannel, nor equate genuinely different known roles.
    backendPtr->setStereoIdentityReuse(true);
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto failed = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto endedEvents = std::vector<Engine::TrackEnded>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          endedEvents.push_back(event);
        }

        ended.notify();
      });
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failed.notify(); });
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    checkSourceRoute(engine.status(), row.channels, firstMask, AudioCodec::Alac);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    auto const firstRoute = engine.status().routeState;
    auto const firstGeneration = engine.playbackGeneration();
    auto const firstFormat = backend->currentFormat();
    CHECK(firstFormat == firstRoute.engineOutputFormat);
    auto* const firstTarget = backend->target();
    REQUIRE(firstTarget != nullptr);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    CHECK(preparedRes->generation == firstGeneration);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    CHECK_FALSE(engine.clearNext());
    CHECK(engine.status().routeState == firstRoute);

    // An oversized render must retain the old PCM and leave the entire tail
    // untouched; incompatible successor bytes cannot leak across this boundary.
    auto output = std::vector<std::byte>(firstPcm.size() + nextPcm.size(), std::byte{0xA5});
    auto const rendered = firstTarget->renderPcm(output);
    REQUIRE(rendered.bytesWritten == firstPcm.size());
    CHECK(rendered.positionFrameOffset == 0);
    CHECK(rendered.positionFrames == 16);
    CHECK(std::ranges::equal(std::span<std::byte const>{output}.first(firstPcm.size()),
                             utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    CHECK(std::ranges::all_of(std::span<std::byte const>{output}.subspan(firstPcm.size()),
                              [](std::byte value) { return value == std::byte{0xA5}; }));
    firstTarget->handlePositionAdvanced(rendered.positionFrames);
    auto const beforeDrain = output;
    auto const drain = firstTarget->renderPcm(output);
    CHECK(drain.bytesWritten == 0);
    REQUIRE(drain.drained);
    CHECK(output == beforeDrain);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK(engine.playbackGeneration() == firstGeneration);
    checkSourceRoute(engine.status(), row.channels, firstMask, AudioCodec::Alac);
    CHECK(engine.status().routeState == firstRoute);
    CHECK(backend->target() == firstTarget);
    CHECK(backend->currentFormat() == firstFormat);
    auto const drainingHistory = backend->events();
    CHECK(std::ranges::count(drainingHistory, std::string{"open"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(drainingHistory, std::string{"start"}, &FakeCapturingBackend::Event::name) == 1);
    CHECK(std::ranges::count(drainingHistory, std::string{"stop"}, &FakeCapturingBackend::Event::name) == 0);
    CHECK(std::ranges::count(drainingHistory, std::string{"close"}, &FakeCapturingBackend::Event::name) == 0);
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 0);
    CHECK(failed.count() == 0);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    REQUIRE(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));

    // DrainFallback is not promotion: explicitly play the successor only
    // after native-drain completion has retired the old attachment.
    engine.play(nextItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
    checkSourceRoute(engine.status(), row.channels, nextMask, AudioCodec::Alac);
    auto const nextGeneration = engine.playbackGeneration();
    CHECK(nextGeneration > firstGeneration);
    auto const nextFormat = backend->currentFormat();
    CHECK(nextFormat == engine.status().routeState.engineOutputFormat);
    CHECK(nextFormat.speakerMask != firstFormat.speakerMask);
    auto* const nextTarget = backend->target();
    REQUIRE(nextTarget != nullptr);
    auto nextOutput = std::vector<std::byte>(nextPcm.size(), std::byte{0xA5});
    auto const nextRendered = nextTarget->renderPcm(nextOutput);
    REQUIRE(nextRendered.bytesWritten == nextPcm.size());
    CHECK_FALSE(nextRendered.drained);
    CHECK(nextRendered.positionFrameOffset == 0);
    CHECK(nextRendered.positionFrames == 16);
    CHECK(std::ranges::equal(nextOutput, utility::bytes::view(std::span<std::uint8_t const>{nextPcm})));
    nextTarget->handlePositionAdvanced(nextRendered.positionFrames);
    auto const nextDrain = nextTarget->renderPcm(nextOutput);
    CHECK(nextDrain.bytesWritten == 0);
    REQUIRE(nextDrain.drained);
    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(2));
    CHECK(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    auto const history = backend->events();
    auto openedFormats = std::vector<PcmFormat>{};
    auto lifecycle = std::vector<std::string>{};

    for (auto const& event : history)
    {
      if (event.name == "open")
      {
        openedFormats.push_back(event.format);
      }

      if (event.name == "open" || event.name == "start" || event.name == "stop" || event.name == "close")
      {
        lifecycle.push_back(event.name);
      }
    }

    CHECK(openedFormats == std::vector<PcmFormat>{firstFormat, nextFormat});
    CHECK(lifecycle == std::vector<std::string>{"open", "start", "stop", "close", "open", "start", "stop", "close"});
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(2));
    engine.shutdown();
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 2);
    CHECK(failed.count() == 0);
    auto const lock = std::scoped_lock{callbackMutex};
    REQUIRE(endedEvents.size() == 2);
    CHECK(endedEvents.front().generation == firstGeneration);
    CHECK(endedEvents.back().generation == nextGeneration);
  }
} // namespace ao::audio::test
