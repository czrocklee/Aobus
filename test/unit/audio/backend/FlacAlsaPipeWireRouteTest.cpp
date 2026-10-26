// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/FlacDecoderSession.h"
#include "lib/audio/backend/detail/AlsaChannelMap.h"
#include "lib/audio/backend/detail/PipeWireFormatParsing.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/FlacFrameTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <alsa/pcm.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::backend::test
{
  namespace
  {
    constexpr auto kMarkers = std::to_array<std::int32_t>({16384, -8192, 4096, -2048, 1024, -512});
    // Independently authored S16LE frame bytes, not a decoder/packer oracle.
    constexpr auto kFrame =
      std::to_array<std::uint8_t>({0x00, 0x40, 0x00, 0xE0, 0x00, 0x10, 0x00, 0xF8, 0x00, 0x04, 0x00, 0xFE});
    constexpr auto kFrontSwappedFrame =
      std::to_array<std::uint8_t>({0x00, 0xE0, 0x00, 0x40, 0x00, 0x10, 0x00, 0xF8, 0x00, 0x04, 0x00, 0xFE});

    struct NativeRoute final
    {
      std::uint32_t mask;
      std::vector<unsigned int> alsaPositions;
      std::vector<std::uint32_t> pipeWirePositions;
    };

    std::array<NativeRoute, 2> nativeRoutes(std::uint8_t channels)
    {
      if (channels == 3U)
      {
        return {NativeRoute{0x7,
                            {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC},
                            {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC}},
                NativeRoute{0xB,
                            {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_LFE},
                            {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_LFE}}};
      }

      if (channels == 5U)
      {
        return {NativeRoute{0x37,
                            {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_RL, SND_CHMAP_RR},
                            {SPA_AUDIO_CHANNEL_FL,
                             SPA_AUDIO_CHANNEL_FR,
                             SPA_AUDIO_CHANNEL_FC,
                             SPA_AUDIO_CHANNEL_RL,
                             SPA_AUDIO_CHANNEL_RR}},
                NativeRoute{0x607,
                            {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_SL, SND_CHMAP_SR},
                            {SPA_AUDIO_CHANNEL_FL,
                             SPA_AUDIO_CHANNEL_FR,
                             SPA_AUDIO_CHANNEL_FC,
                             SPA_AUDIO_CHANNEL_SL,
                             SPA_AUDIO_CHANNEL_SR}}};
      }

      REQUIRE(channels == 6U);
      return {NativeRoute{0x3F,
                          {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_RL, SND_CHMAP_RR},
                          {SPA_AUDIO_CHANNEL_FL,
                           SPA_AUDIO_CHANNEL_FR,
                           SPA_AUDIO_CHANNEL_FC,
                           SPA_AUDIO_CHANNEL_LFE,
                           SPA_AUDIO_CHANNEL_RL,
                           SPA_AUDIO_CHANNEL_RR}},
              NativeRoute{0x60F,
                          {SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_SR},
                          {SPA_AUDIO_CHANNEL_FL,
                           SPA_AUDIO_CHANNEL_FR,
                           SPA_AUDIO_CHANNEL_FC,
                           SPA_AUDIO_CHANNEL_LFE,
                           SPA_AUDIO_CHANNEL_SL,
                           SPA_AUDIO_CHANNEL_SR}}};
    }

    detail::RawStreamFormat parseNativeRoute(std::span<std::uint32_t const> positions)
    {
      auto info = ::spa_audio_info_raw{};
      info.format = SPA_AUDIO_FORMAT_S16_LE;
      info.rate = 48000;
      info.channels = static_cast<std::uint32_t>(positions.size());
      std::ranges::copy(positions, std::span<std::uint32_t>{info.position}.begin());
      auto storage = std::array<std::byte, 1024>{};
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, storage.data(), static_cast<std::uint32_t>(storage.size()));
      auto const* pod = ::spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info);
      REQUIRE(pod != nullptr);
      auto const optNative = detail::parseRawStreamObservation(pod);
      REQUIRE(optNative);
      return *optNative;
    }

    void checkPcm(std::span<std::byte const> bytes, std::uint8_t channels, bool frontSwapped = false)
    {
      auto const frame = std::span<std::uint8_t const>{frontSwapped ? kFrontSwappedFrame : kFrame}.first(
        static_cast<std::size_t>(channels) * 2U);
      REQUIRE(bytes.size() == 16U * frame.size());

      for (std::size_t offset = 0; offset < bytes.size(); ++offset)
      {
        CAPTURE(offset);
        CHECK(std::to_integer<std::uint8_t>(bytes[offset]) == frame[offset % frame.size()]);
      }
    }

    void checkRouteRefusal(BackendOpenFailure const& failure, std::string_view backendName)
    {
      CHECK(failure.cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      CHECK(failure.error.code == Error::Code::FormatRejected);
      CHECK(failure.error.message.contains(backendName));
      CHECK(failure.error.message.contains("channel positions conflict"));
    }

    void checkDecodedRoutes(std::uint8_t channels, std::uint32_t sourceMask, std::string_view declaration = {})
    {
      auto bytes = audio::test::buildFlacStreaminfoBytes(16, channels, 16);

      if (!declaration.empty())
      {
        auto const comments = std::array{std::string{declaration}};
        audio::test::appendFlacCommentBlock(bytes, comments);
      }

      auto const frame = audio::test::buildFlacVerbatimFrameBytes(0, 48000, channels, 16, kMarkers);
      bytes.insert(bytes.end(), frame.begin(), frame.end());
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto const decoded = decoderPtr->streamInfo();
      CHECK(decoded.sourceFormat.speakerMask == sourceMask);
      CHECK(decoded.outputFormat == PcmFormat{.sampleRate = 48000,
                                              .channels = channels,
                                              .encoding = SampleEncoding::Signed16Le,
                                              .speakerMask = sourceMask});
      CHECK(decoded.sourceChannelLayout.origin ==
            (sourceMask == 0U ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified));
      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == 0U);
      REQUIRE(blockRes->frames == 16U);
      checkPcm(blockRes->bytes, channels);

      // Parsed-value boundaries only: no native device open or hardware claim.
      // Swap FL/FR to make accidental source-zero permutations observable.
      for (bool const frontSwapped : {false, true})
      {
        for (bool const sideFirst : {false, true})
        {
          auto routes = nativeRoutes(channels);

          if (sideFirst)
          {
            std::swap(routes[0], routes[1]);
          }

          auto optPrevious = std::optional<detail::RawStreamFormat>{};

          for (auto& route : routes)
          {
            CAPTURE(channels, declaration, sourceMask, route.mask, frontSwapped, sideFirst);

            if (frontSwapped)
            {
              std::swap(route.alsaPositions[0], route.alsaPositions[1]);
              std::swap(route.pipeWirePositions[0], route.pipeWirePositions[1]);
            }

            auto const observations =
              detail::AlsaChannelMapObservations{.query = detail::AlsaChannelMapObservations::Query::ApplicableMap,
                                                 .supportedMaps = {route.alsaPositions},
                                                 .optCurrentPositions = route.alsaPositions};
            auto const mapRes = detail::prepareAlsaChannelMap(
              decoded.outputFormat,
              [&] { return std::expected<detail::AlsaChannelMapObservations, BackendOpenFailure>{observations}; });
            auto const native = parseNativeRoute(route.pipeWirePositions);
            CHECK(native.nativeSpeakerMask == route.mask);
            CHECK(native.format.speakerMask == (frontSwapped ? 0U : route.mask));
            auto const deliveryRes = detail::rawStreamDelivery(decoded.sourceFormat, native, optPrevious);
            bool const shouldAdmit = sourceMask == 0U || sourceMask == route.mask;

            if (!shouldAdmit)
            {
              REQUIRE_FALSE(mapRes);
              checkRouteRefusal(mapRes.error(), "ALSA");
              REQUIRE_FALSE(deliveryRes);
              checkRouteRefusal(deliveryRes.error(), "PipeWire");
              continue;
            }

            REQUIRE(mapRes);
            CHECK(mapRes->delivery.nativeSpeakerMask == route.mask);
            CHECK(mapRes->delivery.isMapped == (sourceMask != 0U));
            CHECK_FALSE(mapRes->delivery.hasSharedConversion);
            CHECK(mapRes->needsPermutation == (sourceMask != 0U && frontSwapped));
            CHECK(mapRes->observations.optCurrentPositions == observations.optCurrentPositions);
            auto alsaPcm = std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()};
            auto scratch = std::array<std::byte, detail::kAlsaPermutationScratchBytes>{};
            detail::applyAlsaChannelPermutation(alsaPcm, *mapRes, scratch);
            checkPcm(alsaPcm, channels, sourceMask != 0U && frontSwapped);

            REQUIRE(deliveryRes);
            CHECK(deliveryRes->clientFormat == decoded.outputFormat);
            CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == route.mask);
            CHECK(deliveryRes->channelDelivery.isMapped);
            CHECK_FALSE(deliveryRes->channelDelivery.hasSharedConversion);
            CHECK(deliveryRes->needsPermutation == (sourceMask != 0U && frontSwapped));
            auto pipeWirePcm = std::vector<std::byte>{blockRes->bytes.begin(), blockRes->bytes.end()};
            detail::applyRawStreamDelivery(pipeWirePcm, *deliveryRes);
            checkPcm(pipeWirePcm, channels, sourceMask != 0U && frontSwapped);

            // Source-zero observations may refresh back <-> side in either
            // order while retaining the same unidentified client byte stream.
            if (sourceMask == 0U)
            {
              optPrevious = native;
            }
          }
        }
      }

      CHECK(decoderPtr->streamInfo() == decoded);
    }
  } // namespace

  TEST_CASE("FlacUnpositionedRoute - untagged five and six channels admit back and side without permutation",
            "[audio][integration][flac]")
  {
    checkDecodedRoutes(5, 0);
    checkDecodedRoutes(6, 0);
  }

  TEST_CASE("FlacUnpositionedRoute - explicit back and side identities reject opposite native roles",
            "[audio][integration][flac]")
  {
    checkDecodedRoutes(5, 0x37, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x37");
    checkDecodedRoutes(5, 0x607, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x607");
    checkDecodedRoutes(6, 0x3F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F");
    checkDecodedRoutes(6, 0x60F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F");
  }

  TEST_CASE("FlacUnpositionedRoute - three channel default center and explicit LFE identities remain strict",
            "[audio][integration][flac]")
  {
    checkDecodedRoutes(3, 0x7);
    checkDecodedRoutes(3, 0xB, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xB");
  }
} // namespace ao::audio::backend::test
