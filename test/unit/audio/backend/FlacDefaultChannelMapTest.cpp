// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Decoder-to-native-planner integration at the software boundary. A genuine
// five/six-channel FLAC file is decoded by the real decoder, every sample
// frame is verified against hand-authored literal little-endian bytes, and
// the finalized decoder facts then drive the actual ALSA and PipeWire
// channel-map planners against independently authored native position
// observations. These cases prove the decoder/planner boundary only: no
// audio device is opened, no native command runs, and nothing here asserts
// hardware wiring.

#include "lib/audio/DecoderFactory.h"
#include "lib/audio/backend/detail/AlsaChannelMap.h"
#include "lib/audio/backend/detail/PipeWireFormatParsing.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/FlacFrameTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/ByteView.h>

extern "C"
{
#include <alsa/pcm.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::uint32_t kFramesPerBlock = 16;

    // Constant per-channel marker values for the three verbatim fixture
    // frames. The decode oracle is the separately hand-authored
    // kLiteralFrames below, written directly rather than derived from these
    // values or from any production mapping.
    constexpr auto kFrameMarkers = std::to_array<std::array<std::int32_t, 6>>({
      {0x1001, 0x2002, 0x3003, 0x4004, 0x5005, 0x6006},
      {0x1101, 0x2202, 0x3303, 0x4404, 0x5505, 0x6606},
      {0x2101, 0x3202, 0x4303, 0x5404, 0x6505, 0x7606},
    });

    // Hand-authored literal little-endian decode oracle: the bytes each
    // decoded frame must deliver in source slot order, twelve bytes per
    // frame for up to six channels. They encode the same constant markers as
    // the fixture above but are written directly instead of being extracted
    // from it.
    constexpr auto kLiteralFrames = std::to_array<std::array<std::uint8_t, 12>>({
      {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60},
      {0x01, 0x11, 0x02, 0x22, 0x03, 0x33, 0x04, 0x44, 0x05, 0x55, 0x06, 0x66},
      {0x01, 0x21, 0x02, 0x32, 0x03, 0x43, 0x04, 0x54, 0x05, 0x65, 0x06, 0x76},
    });

    std::span<std::uint8_t const> literalFrameBytes(std::uint8_t const channels, std::size_t const frameIndex)
    {
      return std::span<std::uint8_t const>{kLiteralFrames[frameIndex]}.first(static_cast<std::size_t>(channels) * 2U);
    }

    void appendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t const value)
    {
      for (std::uint32_t shift = 0; shift < 32; shift += 8)
      {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }

    void appendCommentBlock(std::vector<std::uint8_t>& bytes,
                            std::vector<std::string> const& comments,
                            bool const isLast)
    {
      auto body = std::vector<std::uint8_t>{};
      appendLe32(body, 0); // Empty vendor string.
      appendLe32(body, static_cast<std::uint32_t>(comments.size()));

      for (auto const& comment : comments)
      {
        appendLe32(body, static_cast<std::uint32_t>(comment.size()));
        body.insert(body.end(), comment.begin(), comment.end());
      }

      REQUIRE(body.size() <= 0xFFFFFFU);
      bytes.push_back(isLast ? 0x84 : 0x04);
      bytes.push_back(static_cast<std::uint8_t>((body.size() >> 16U) & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>((body.size() >> 8U) & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>(body.size() & 0xFFU));
      bytes.insert(bytes.end(), body.begin(), body.end());
    }

    std::vector<std::uint8_t> buildSurroundStream(std::uint8_t const channels, std::vector<std::string> const& comments)
    {
      REQUIRE((channels == 5 || channels == 6));

      // The shared builder fixes stereo/16-bit STREAMINFO; change only its
      // three channel bits, retaining 48 kHz, 16-bit precision, and the
      // 48-sample total, then append the optional Vorbis comment block and
      // three verbatim frames of constant per-channel markers.
      auto bytes = ao::audio::test::buildFlacStreaminfoBytes(kFrameMarkers.size() * kFramesPerBlock);
      bytes[20] = static_cast<std::uint8_t>((bytes[20] & 0xF0U) | ((channels - 1U) << 1U));
      bytes[21] = static_cast<std::uint8_t>((bytes[21] & 0x0FU) | 0xF0U);

      if (!comments.empty())
      {
        bytes[4] = 0; // STREAMINFO is no longer the last metadata block.
        appendCommentBlock(bytes, comments, true);
      }

      for (std::size_t frameIndex = 0; frameIndex < kFrameMarkers.size(); ++frameIndex)
      {
        auto const markers = std::span<std::int32_t const>{kFrameMarkers[frameIndex]}.first(channels);
        auto const frame = ao::audio::test::buildFlacVerbatimFrameBytes(
          static_cast<std::uint32_t>(frameIndex), kSampleRate, channels, 16, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    std::vector<std::string> maskComments(std::uint32_t const mask)
    {
      return {"TITLE=unrelated", std::format("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{:X}", mask)};
    }

    void checkDecodedInfo(DecodedStreamInfo const& info,
                          std::uint8_t const channels,
                          std::uint32_t const expectedMask,
                          ChannelLayoutOrigin const origin,
                          std::optional<std::uint32_t> const optDeclaration)
    {
      CHECK(info.sourceFormat.sampleRate == kSampleRate);
      CHECK(info.outputFormat.sampleRate == kSampleRate);
      CHECK(info.sourceFormat.channels == channels);
      CHECK(info.outputFormat.channels == channels);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.sourceFormat.speakerMask == expectedMask);
      CHECK(info.outputFormat.speakerMask == expectedMask);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(info.sourceChannelLayout.origin == origin);
      CHECK(info.sourceChannelLayout.optDeclaredMask == optDeclaration);
      CHECK_FALSE(info.isLossy);
      CHECK(info.duration == std::chrono::milliseconds{1});
    }

    // Verifies the hand-authored literal bytes across the entire decoded
    // stream, including end-of-stream.
    void requireLiteralDecodedFrames(DecoderSession& decoder, std::uint8_t const channels)
    {
      for (std::size_t frameIndex = 0; frameIndex < kLiteralFrames.size(); ++frameIndex)
      {
        auto const literal = literalFrameBytes(channels, frameIndex);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(blockRes->firstFrameIndex == frameIndex * kFramesPerBlock);
        CHECK(blockRes->frames == kFramesPerBlock);
        CHECK_FALSE(blockRes->endOfStream);
        REQUIRE(blockRes->bytes.size() == kFramesPerBlock * literal.size());
        auto const literalView = utility::bytes::view(literal.data(), literal.size());

        for (std::size_t frame = 0; frame < kFramesPerBlock; ++frame)
        {
          CHECK(
            std::ranges::equal(blockRes->bytes.subspan(frame * literalView.size(), literalView.size()), literalView));
        }
      }

      for (std::size_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0);
        CHECK(endRes->bytes.empty());
      }
    }

    // Delivery oracle: each endpoint slot receives the source slot named in
    // the hand-authored list, so it carries that source slot's literal bytes.
    std::vector<std::uint8_t> deliveredFrameBytes(std::span<std::uint8_t const> literalFrame,
                                                  std::span<std::uint8_t const> sourceSlots)
    {
      REQUIRE(!sourceSlots.empty());
      auto const sampleBytes = literalFrame.size() / sourceSlots.size();
      auto bytes = std::vector<std::uint8_t>{};

      for (auto const slot : sourceSlots)
      {
        auto const offset = static_cast<std::size_t>(slot) * sampleBytes;
        auto const sample = literalFrame.subspan(offset, sampleBytes);
        bytes.insert(bytes.end(), sample.begin(), sample.end());
      }

      return bytes;
    }

    std::vector<std::byte> byteCopyOf(std::span<std::uint8_t const> bytes)
    {
      auto copy = std::vector<std::byte>{};
      copy.reserve(bytes.size());

      for (auto const byte : bytes)
      {
        copy.push_back(std::byte{byte});
      }

      return copy;
    }

    // Four literal boundary fixtures, one per channel count and back-or-side
    // endpoint. The declared mask is the standard layout matching the
    // endpoint's actual role set; the conflicting mask is the opposite
    // back-or-side layout. Each API's position list uses its own named role
    // identifiers; the lists are authored separately and never shared or
    // converted between APIs.
    struct SurroundRow final
    {
      std::uint8_t channels;
      std::uint32_t declaredMask;
      std::uint32_t conflictingDeclaredMask;
      std::vector<unsigned int> nativeAlsaPositions;
      std::vector<std::uint32_t> nativePipeWirePositions;
      // Hand-authored native slot -> canonical source slot list.
      std::vector<std::uint8_t> expectedSourceSlots;
    };

    std::vector<SurroundRow> surroundRows()
    {
      return {
        SurroundRow{
          .channels = 5,
          // Front pair, center, and side pair; the back-pair mask conflicts.
          .declaredMask = 0x607U,
          .conflictingDeclaredMask = 0x37U,
          .nativeAlsaPositions = {SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC},
          .nativePipeWirePositions = {SPA_AUDIO_CHANNEL_SL,
                                      SPA_AUDIO_CHANNEL_SR,
                                      SPA_AUDIO_CHANNEL_FL,
                                      SPA_AUDIO_CHANNEL_FR,
                                      SPA_AUDIO_CHANNEL_FC},
          .expectedSourceSlots = {3, 4, 0, 1, 2},
        },
        SurroundRow{
          .channels = 5,
          // Front pair, center, and back pair; the side-pair mask conflicts.
          .declaredMask = 0x37U,
          .conflictingDeclaredMask = 0x607U,
          .nativeAlsaPositions = {SND_CHMAP_RL, SND_CHMAP_RR, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC},
          .nativePipeWirePositions = {SPA_AUDIO_CHANNEL_RL,
                                      SPA_AUDIO_CHANNEL_RR,
                                      SPA_AUDIO_CHANNEL_FL,
                                      SPA_AUDIO_CHANNEL_FR,
                                      SPA_AUDIO_CHANNEL_FC},
          .expectedSourceSlots = {3, 4, 0, 1, 2},
        },
        SurroundRow{
          .channels = 6,
          // Front pair, center, LFE, and side pair; the back-pair mask conflicts.
          .declaredMask = 0x60FU,
          .conflictingDeclaredMask = 0x3FU,
          .nativeAlsaPositions = {SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE},
          .nativePipeWirePositions = {SPA_AUDIO_CHANNEL_SL,
                                      SPA_AUDIO_CHANNEL_SR,
                                      SPA_AUDIO_CHANNEL_FL,
                                      SPA_AUDIO_CHANNEL_FR,
                                      SPA_AUDIO_CHANNEL_FC,
                                      SPA_AUDIO_CHANNEL_LFE},
          .expectedSourceSlots = {4, 5, 0, 1, 2, 3},
        },
        SurroundRow{
          .channels = 6,
          // Front pair, center, LFE, and back pair; the side-pair mask conflicts.
          .declaredMask = 0x3FU,
          .conflictingDeclaredMask = 0x60FU,
          .nativeAlsaPositions = {SND_CHMAP_RL, SND_CHMAP_RR, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE},
          .nativePipeWirePositions = {SPA_AUDIO_CHANNEL_RL,
                                      SPA_AUDIO_CHANNEL_RR,
                                      SPA_AUDIO_CHANNEL_FL,
                                      SPA_AUDIO_CHANNEL_FR,
                                      SPA_AUDIO_CHANNEL_FC,
                                      SPA_AUDIO_CHANNEL_LFE},
          .expectedSourceSlots = {4, 5, 0, 1, 2, 3},
        },
      };
    }

    std::expected<detail::AlsaChannelMap, BackendOpenFailure> planAlsaDelivery(
      PcmFormat const& client,
      std::vector<unsigned int> const& positions)
    {
      return detail::prepareAlsaChannelMap(
        client,
        [&positions]
        {
          return std::expected<detail::AlsaChannelMapObservations, BackendOpenFailure>{
            detail::AlsaChannelMapObservations{.supportedMaps = {}, .optCurrentPositions = positions}};
        });
    }

    // Native client readback authored from named SPA role identifiers with
    // the same pure builder the owning parsing tests use. No PipeWire server,
    // stream, or device participates.
    detail::RawStreamFormat requireNativeObservation(std::uint8_t const channels,
                                                     std::span<std::uint32_t const> positions)
    {
      REQUIRE(positions.size() == channels);
      auto buffer = std::array<std::byte, 1024>{};
      auto info = ::spa_audio_info_raw{};
      info.format = SPA_AUDIO_FORMAT_S16_LE;
      info.rate = kSampleRate;
      info.channels = channels;
      info.flags = 0U;
      std::ranges::copy(positions, std::span<std::uint32_t>{info.position}.begin());

      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
      auto const* pod = ::spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info);
      REQUIRE(pod != nullptr);

      auto const optNative = detail::parseRawStreamObservation(pod);
      REQUIRE(optNative);

      return *optNative;
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - decoded five and six channel streams drive the ALSA channel-map planner",
            "[audio][integration][flac][alsa]")
  {
    for (auto const& row : surroundRows())
    {
      auto const channels = row.channels;
      CAPTURE(channels, row.declaredMask);

      // A genuine untagged decode; the planner admits it to the endpoint as
      // legacy identity: no permutation, whatever the endpoint's slot order
      // is, and the endpoint's independently known mask is retained without
      // claiming the source shares it.
      {
        auto const temp = ao::test::TempFile{buildSurroundStream(channels, {}), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(info, channels, 0, ChannelLayoutOrigin::Unknown, std::nullopt);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const mapRes = planAlsaDelivery(info.outputFormat, row.nativeAlsaPositions);
        REQUIRE(mapRes);
        CHECK(mapRes->channels == channels);
        CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
        CHECK(mapRes->delivery.nativeSpeakerMask == row.declaredMask);
        CHECK_FALSE(mapRes->needsPermutation);

        for (std::size_t slot = 0; slot < channels; ++slot)
        {
          CHECK(mapRes->sourceSlots[slot] == slot);
        }

        auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};

        for (std::size_t frameIndex = 0; frameIndex < kLiteralFrames.size(); ++frameIndex)
        {
          auto const literal = literalFrameBytes(channels, frameIndex);
          auto frame = byteCopyOf(literal);
          detail::applyAlsaChannelPermutation(std::span<std::byte>{frame}, *mapRes, scratch);
          CHECK(std::ranges::equal(frame, utility::bytes::view(literal.data(), literal.size())));
        }
      }

      // The standard back-or-side declaration matching the endpoint is
      // admitted with the lossless permutation for its actual slot order.
      {
        auto const temp = ao::test::TempFile{buildSurroundStream(channels, maskComments(row.declaredMask)), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(info, channels, row.declaredMask, ChannelLayoutOrigin::Declared, row.declaredMask);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const mapRes = planAlsaDelivery(info.outputFormat, row.nativeAlsaPositions);
        REQUIRE(mapRes);
        CHECK(mapRes->delivery.mapping == ChannelDeliveryEvidence::Mapping::Mapped);
        CHECK(mapRes->delivery.nativeSpeakerMask == row.declaredMask);
        REQUIRE(mapRes->needsPermutation);

        auto const expectedSlots = std::span<std::uint8_t const>{row.expectedSourceSlots};
        CHECK(std::ranges::equal(std::span<std::uint8_t const>{mapRes->sourceSlots}.first(channels), expectedSlots));

        auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};

        for (std::size_t frameIndex = 0; frameIndex < kLiteralFrames.size(); ++frameIndex)
        {
          auto const literal = literalFrameBytes(channels, frameIndex);
          auto frame = byteCopyOf(literal);
          auto const delivered = deliveredFrameBytes(literal, expectedSlots);
          detail::applyAlsaChannelPermutation(std::span<std::byte>{frame}, *mapRes, scratch);
          CHECK(std::ranges::equal(frame, utility::bytes::view(delivered.data(), delivered.size())));
        }
      }

      // The opposite back-or-side declaration refuses at the same endpoint
      // that the untagged stream above was admitted to, so the refusal is
      // the declaration's, never a fallback to unknown.
      {
        auto const temp =
          ao::test::TempFile{buildSurroundStream(channels, maskComments(row.conflictingDeclaredMask)), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(
          info, channels, row.conflictingDeclaredMask, ChannelLayoutOrigin::Declared, row.conflictingDeclaredMask);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const planRes = planAlsaDelivery(info.outputFormat, row.nativeAlsaPositions);
        REQUIRE_FALSE(planRes);
        CHECK(planRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
        CHECK(planRes.error().error.code == Error::Code::FormatRejected);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - decoded five and six channel streams drive the PipeWire raw stream planner",
            "[audio][integration][flac][pipewire]")
  {
    for (auto const& row : surroundRows())
    {
      auto const channels = row.channels;
      CAPTURE(channels, row.declaredMask);

      // A genuine untagged decode; the planner admits it with legacy
      // identity: no permutation, a zero client mask, and the endpoint's
      // independently known position set retained as the native observation.
      {
        auto const temp = ao::test::TempFile{buildSurroundStream(channels, {}), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(info, channels, 0, ChannelLayoutOrigin::Unknown, std::nullopt);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const native = requireNativeObservation(channels, row.nativePipeWirePositions);
        CHECK(native.nativeSpeakerMask == row.declaredMask);

        auto const deliveryRes = detail::rawStreamDelivery(info.sourceFormat, native);
        REQUIRE(deliveryRes);
        CHECK(deliveryRes->clientFormat.speakerMask == 0U);
        CHECK(deliveryRes->channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
        CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == row.declaredMask);
        CHECK_FALSE(deliveryRes->needsPermutation);

        for (std::size_t frameIndex = 0; frameIndex < kLiteralFrames.size(); ++frameIndex)
        {
          auto const literal = literalFrameBytes(channels, frameIndex);
          auto frame = byteCopyOf(literal);
          detail::applyRawStreamDelivery(std::span<std::byte>{frame}, *deliveryRes);
          CHECK(std::ranges::equal(frame, utility::bytes::view(literal.data(), literal.size())));
        }
      }

      // The standard back-or-side declaration matching the endpoint is
      // admitted with the actual permutation for its slot order. Only the
      // exact two-channel offer can claim Mapped, so multichannel delivery
      // stays legacy-unverified here by design.
      {
        auto const temp = ao::test::TempFile{buildSurroundStream(channels, maskComments(row.declaredMask)), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(info, channels, row.declaredMask, ChannelLayoutOrigin::Declared, row.declaredMask);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const native = requireNativeObservation(channels, row.nativePipeWirePositions);
        auto const deliveryRes = detail::rawStreamDelivery(info.sourceFormat, native);
        REQUIRE(deliveryRes);
        CHECK(deliveryRes->clientFormat.speakerMask == row.declaredMask);
        CHECK(deliveryRes->channelDelivery.mapping == ChannelDeliveryEvidence::Mapping::LegacyUnverified);
        CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == row.declaredMask);
        REQUIRE(deliveryRes->needsPermutation);

        auto const expectedSlots = std::span<std::uint8_t const>{row.expectedSourceSlots};
        CHECK(
          std::ranges::equal(std::span<std::uint8_t const>{deliveryRes->sourceSlots}.first(channels), expectedSlots));

        for (std::size_t frameIndex = 0; frameIndex < kLiteralFrames.size(); ++frameIndex)
        {
          auto const literal = literalFrameBytes(channels, frameIndex);
          auto frame = byteCopyOf(literal);
          auto const delivered = deliveredFrameBytes(literal, expectedSlots);
          detail::applyRawStreamDelivery(std::span<std::byte>{frame}, *deliveryRes);
          CHECK(std::ranges::equal(frame, utility::bytes::view(delivered.data(), delivered.size())));
        }
      }

      // The opposite back-or-side declaration refuses at the same endpoint
      // that the untagged stream above was admitted to, so the refusal is
      // the declaration's, never a fallback to unknown.
      {
        auto const temp =
          ao::test::TempFile{buildSurroundStream(channels, maskComments(row.conflictingDeclaredMask)), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkDecodedInfo(
          info, channels, row.conflictingDeclaredMask, ChannelLayoutOrigin::Declared, row.conflictingDeclaredMask);
        requireLiteralDecodedFrames(*decoderPtr, channels);

        auto const native = requireNativeObservation(channels, row.nativePipeWirePositions);
        auto const deliveryRes = detail::rawStreamDelivery(info.sourceFormat, native);
        REQUIRE_FALSE(deliveryRes);
        CHECK(deliveryRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      }
    }
  }
} // namespace ao::audio::backend::test
