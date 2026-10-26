// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "FlacFrameTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/Engine.h>
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
    enum class FlacIdentity : std::uint8_t
    {
      Back,
      Untagged,
      DamagedComment,
      Side,
      ExcessMask,
    };

    struct AlternateIdentity final
    {
      std::string_view name;
      FlacIdentity identity;
      std::uint32_t mask;
    };

    struct EqualIdentities final
    {
      std::string_view name;
      FlacIdentity first;
      FlacIdentity next;
      std::uint32_t mask;
    };

    // Slot-labelled inputs and LE output oracles are authored independently.
    // Explicit back slots are FL FR FC LFE BL BR; side slots are FL FR FC LFE SL SR.
    // Unknown retains the six ordinal samples without claiming speaker roles.
    constexpr auto kFirstMarkers = std::to_array<std::int32_t>({0x1211, 0x2423, 0x3635, 0x4847, 0x5A59, 0x6C6B});
    constexpr auto kNextMarkers = std::to_array<std::int32_t>({0x5251, 0x6463, 0x7675, 0x0807, 0x1A19, 0x2C2B});
    constexpr auto kFirstFrame =
      std::to_array<std::uint8_t>({0x11, 0x12, 0x23, 0x24, 0x35, 0x36, 0x47, 0x48, 0x59, 0x5A, 0x6B, 0x6C});
    constexpr auto kNextFrame =
      std::to_array<std::uint8_t>({0x51, 0x52, 0x63, 0x64, 0x75, 0x76, 0x07, 0x08, 0x19, 0x1A, 0x2B, 0x2C});
    constexpr std::size_t kBytesPerFrame = 12;

    std::vector<std::uint8_t> makeSixChannelFlacBytes(FlacIdentity identity, std::span<std::int32_t const> markers)
    {
      auto bytes = buildFlacStreaminfoBytes(48, 6, 16);

      switch (identity)
      {
        case FlacIdentity::Back:
          appendFlacCommentBlock(bytes, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F"}));
          break;
        case FlacIdentity::Untagged: break;
        case FlacIdentity::DamagedComment:
          // Complete outer block; the seven-byte VC body lacks its entry count.
          appendFlacRawCommentBlock(bytes, std::array<std::uint8_t, 7>{});
          break;
        case FlacIdentity::Side:
          appendFlacCommentBlock(bytes, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"}));
          break;
        case FlacIdentity::ExcessMask:
          // Eight supported roles cannot identify a six-channel stream.
          appendFlacCommentBlock(bytes, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x63F"}));
          break;
      }

      for (std::uint32_t frameIndex = 0; frameIndex < 3; ++frameIndex)
      {
        auto const frame = buildFlacVerbatimFrameBytes(frameIndex, 48000, 6, 16, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    std::vector<std::uint8_t> repeatLiteralFrame(std::span<std::uint8_t const> frame)
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (std::uint32_t frameIndex = 0; frameIndex < 48; ++frameIndex)
      {
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    void checkFlacRouteIdentity(AudioRouteFormatState const& route, std::uint32_t mask)
    {
      CHECK(route.codec == AudioCodec::Flac);
      CHECK_FALSE(route.isLossySource);
      CHECK(route.sourceFormat.sampleRate == 48000);
      CHECK(route.sourceFormat.channels == 6);
      CHECK(route.sourceFormat.precisionBits == 16);
      CHECK(route.sourceFormat.speakerMask == mask);
      CHECK(route.decoderOutputFormat.sampleRate == 48000);
      CHECK(route.decoderOutputFormat.channels == 6);
      CHECK(route.decoderOutputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(route.decoderOutputFormat.speakerMask == mask);
      CHECK(route.engineOutputFormat == route.decoderOutputFormat);
      CHECK(route.sourceChannelLayout == ChannelLayoutEvidence{.origin = mask == 0 ? ChannelLayoutOrigin::Unknown
                                                                                   : ChannelLayoutOrigin::Identified});
    }
  } // namespace

  TEST_CASE("Engine - unequal real six-channel FLAC identities drain before explicit successor reopen",
            "[audio][integration][engine][flac][concurrency]")
  {
    auto const alternate = GENERATE(AlternateIdentity{"untagged", FlacIdentity::Untagged, 0},
                                    AlternateIdentity{"damaged-comment", FlacIdentity::DamagedComment, 0},
                                    AlternateIdentity{"side", FlacIdentity::Side, 0x60F},
                                    AlternateIdentity{"excess-mask", FlacIdentity::ExcessMask, 0});
    bool const alternateFirst = GENERATE(false, true);
    bool const stereoIdentityReuse = GENERATE(false, true);
    CAPTURE(alternate.name, alternateFirst, stereoIdentityReuse);
    auto const firstIdentity = alternateFirst ? alternate.identity : FlacIdentity::Back;
    auto const nextIdentity = alternateFirst ? FlacIdentity::Back : alternate.identity;
    auto const firstMask = alternateFirst ? alternate.mask : 0x3FU;
    auto const nextMask = alternateFirst ? 0x3FU : alternate.mask;
    auto const firstPcm = repeatLiteralFrame(kFirstFrame);
    auto const nextPcm = repeatLiteralFrame(kNextFrame);
    auto const firstFile = ao::test::TempFile{makeSixChannelFlacBytes(firstIdentity, kFirstMarkers), ".flac"};
    auto const nextFile = ao::test::TempFile{makeSixChannelFlacBytes(nextIdentity, kNextMarkers), ".flac"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    // A backend's stereo-only identity proof must not become a six-channel wildcard.
    backend->setStereoIdentityReuse(stereoIdentityReuse);
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto failures = CallbackLatch{};
    auto settled = CallbackLatch{};
    auto callbackMutex = std::mutex{};
    auto endedEvents = std::vector<Engine::TrackEnded>{};
    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice()};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { advanced.notify(); });
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failures.notify(); });
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const& event)
      {
        {
          auto const lock = std::scoped_lock{callbackMutex};
          endedEvents.push_back(event);
        }

        ended.notify();
      });
    auto const firstItem = makePlaybackItem(firstFile.path);
    auto const nextItem = makePlaybackItem(nextFile.path);
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    auto const firstGeneration = engine.playbackGeneration();
    auto const firstRoute = engine.routeStatus();
    checkFlacRouteIdentity(firstRoute.state, firstMask);
    auto const installedFormat = backend->currentFormat();
    CHECK(installedFormat.speakerMask == firstMask);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
    CHECK_FALSE(engine.clearNext());

    auto output = std::vector<std::byte>(firstPcm.size());
    auto const rendered = target->renderPcm(output);
    REQUIRE(rendered.bytesWritten == output.size());
    CHECK_FALSE(rendered.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm})));
    target->handlePositionAdvanced(rendered.positionFrames);
    auto const drain = target->renderPcm(output);
    REQUIRE(drain.drained);
    CHECK(drain.bytesWritten == 0);
    // Drain discovery is not drain completion or successor promotion.
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 0);
    CHECK(failures.count() == 0);
    CHECK(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK(engine.playbackGeneration() == firstGeneration);
    CHECK(engine.routeStatus() == firstRoute);
    CHECK(backend->target() == target);
    CHECK(backend->currentFormat() == installedFormat);
    CHECK(std::ranges::equal(
      backend->events(), std::to_array<std::string_view>({"open", "start"}), {}, &FakeCapturingBackend::Event::name));

    backend->emitDrainComplete();
    REQUIRE(ended.tryWaitForCount(1));
    CHECK(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    CHECK_FALSE(engine.isCurrentPlaybackItem(nextItem.id));
    {
      auto const lock = std::scoped_lock{callbackMutex};
      REQUIRE(endedEvents.size() == 1);
      CHECK(endedEvents.front().generation == firstGeneration);
    }

    engine.play(nextItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
    checkFlacRouteIdentity(engine.status().routeState, nextMask);
    CHECK(backend->currentFormat().speakerMask == nextMask);
    auto* const reopenedTarget = backend->target();
    REQUIRE(reopenedTarget != nullptr);
    auto const reopenedRender = reopenedTarget->renderPcm(output);
    REQUIRE(reopenedRender.bytesWritten == output.size());
    CHECK_FALSE(reopenedRender.drained);
    CHECK(std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{nextPcm})));
    reopenedTarget->handlePositionAdvanced(reopenedRender.positionFrames);
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(2));
    CHECK(advanced.count() == 0);
    CHECK(ended.count() == 1);
    CHECK(failures.count() == 0);
    auto const reopenedEvents = backend->events();
    REQUIRE(reopenedEvents.size() == 6);
    CHECK(reopenedEvents[0].format == installedFormat);
    CHECK(reopenedEvents[4].format == backend->currentFormat());
    CHECK(std::ranges::equal(reopenedEvents,
                             std::to_array<std::string_view>({"open", "start", "stop", "close", "open", "start"}),
                             {},
                             &FakeCapturingBackend::Event::name));
    engine.stop();
    CHECK(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    CHECK(std::ranges::equal(
      backend->events(),
      std::to_array<std::string_view>({"open", "start", "stop", "close", "open", "start", "stop", "close"}),
      {},
      &FakeCapturingBackend::Event::name));
  }

  TEST_CASE("Engine - equal real six-channel FLAC identities splice literal PCM without reopening",
            "[audio][integration][engine][flac][concurrency]")
  {
    auto const identities =
      GENERATE(EqualIdentities{"back-back", FlacIdentity::Back, FlacIdentity::Back, 0x3F},
               EqualIdentities{"untagged-untagged", FlacIdentity::Untagged, FlacIdentity::Untagged, 0},
               EqualIdentities{"untagged-damaged", FlacIdentity::Untagged, FlacIdentity::DamagedComment, 0},
               EqualIdentities{"damaged-untagged", FlacIdentity::DamagedComment, FlacIdentity::Untagged, 0},
               EqualIdentities{"damaged-damaged", FlacIdentity::DamagedComment, FlacIdentity::DamagedComment, 0},
               EqualIdentities{"side-side", FlacIdentity::Side, FlacIdentity::Side, 0x60F},
               EqualIdentities{"excess-damaged", FlacIdentity::ExcessMask, FlacIdentity::DamagedComment, 0});
    CAPTURE(identities.name);
    auto const firstPcm = repeatLiteralFrame(kFirstFrame);
    auto const nextPcm = repeatLiteralFrame(kNextFrame);
    auto const firstFile = ao::test::TempFile{makeSixChannelFlacBytes(identities.first, kFirstMarkers), ".flac"};
    auto const nextFile = ao::test::TempFile{makeSixChannelFlacBytes(identities.next, kNextMarkers), ".flac"};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto advanced = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto failures = CallbackLatch{};
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
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const&) { failures.notify(); });
    auto const firstItem = makePlaybackItem(firstFile.path);
    auto const nextItem = makePlaybackItem(nextFile.path);
    engine.play(firstItem);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(firstItem.id));
    checkFlacRouteIdentity(engine.status().routeState, identities.mask);
    auto const installedFormat = backend->currentFormat();
    CHECK(installedFormat.speakerMask == identities.mask);
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto const preparedRes = engine.setNext(nextItem);
    REQUIRE(preparedRes);
    CHECK(preparedRes->itemId == nextItem.id);
    REQUIRE(preparedRes->transition == Engine::PreparedTransitionMode::Gapless);

    auto captured = std::vector<std::byte>{};
    auto output = std::vector<std::byte>(20U * kBytesPerFrame);
    auto const firstRender = target->renderPcm(output);
    REQUIRE(firstRender.bytesWritten == output.size());
    CHECK_FALSE(firstRender.drained);
    CHECK(
      std::ranges::equal(output, utility::bytes::view(std::span<std::uint8_t const>{firstPcm}).first(output.size())));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(firstRender.positionFrames);

    // This callback must contain 28 old frames and four distinct successor frames.
    output.resize(32U * kBytesPerFrame);
    auto const splice = target->renderPcm(output);
    REQUIRE(splice.bytesWritten == output.size());
    CHECK_FALSE(splice.drained);
    CHECK(splice.positionFrameOffset == 28);
    CHECK(splice.positionFrames == 4);
    CHECK(
      std::ranges::equal(std::span{output}.first(28U * kBytesPerFrame),
                         utility::bytes::view(std::span<std::uint8_t const>{firstPcm}).subspan(20U * kBytesPerFrame)));
    CHECK(std::ranges::equal(std::span{output}.subspan(28U * kBytesPerFrame),
                             utility::bytes::view(std::span<std::uint8_t const>{nextPcm}).first(4U * kBytesPerFrame)));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(splice.positionFrames);
    REQUIRE(advanced.tryWaitForCount(1));
    CHECK(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(nextItem.id));
    CHECK_FALSE(engine.isCurrentPlaybackItem(firstItem.id));
    checkFlacRouteIdentity(engine.status().routeState, identities.mask);
    CHECK(engine.routeStatus().generation == preparedRes->generation);
    CHECK(engine.playbackGeneration() == preparedRes->generation);
    CHECK(backend->target() == target);
    CHECK(backend->currentFormat() == installedFormat);

    output.resize(44U * kBytesPerFrame);
    auto const finalRender = target->renderPcm(output);
    REQUIRE(finalRender.bytesWritten == output.size());
    CHECK_FALSE(finalRender.drained);
    CHECK(std::ranges::equal(
      output, utility::bytes::view(std::span<std::uint8_t const>{nextPcm}).subspan(4U * kBytesPerFrame)));
    captured.insert(captured.end(), output.begin(), output.end());
    target->handlePositionAdvanced(finalRender.positionFrames);
    auto expected = firstPcm;
    expected.insert(expected.end(), nextPcm.begin(), nextPcm.end());
    CHECK(std::ranges::equal(captured, utility::bytes::view(std::span<std::uint8_t const>{expected})));
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(advanced.count() == 1);
    CHECK(ended.count() == 0);
    CHECK(failures.count() == 0);
    {
      auto const lock = std::scoped_lock{callbackMutex};
      REQUIRE(advancedEvents.size() == 1);
      CHECK(advancedEvents.front().itemId == nextItem.id);
      CHECK(advancedEvents.front().input.filePath == nextFile.path);
      CHECK(advancedEvents.front().generation == preparedRes->generation);
    }

    CHECK(std::ranges::equal(
      backend->events(), std::to_array<std::string_view>({"open", "start"}), {}, &FakeCapturingBackend::Event::name));
    engine.stop();
    CHECK(engine.transport() == Transport::Idle);
    CHECK(backend->target() == nullptr);
    CHECK(std::ranges::equal(backend->events(),
                             std::to_array<std::string_view>({"open", "start", "stop", "close"}),
                             {},
                             &FakeCapturingBackend::Event::name));
  }
} // namespace ao::audio::test
