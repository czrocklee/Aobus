// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/PipeWireFormatParsing.h"

#include "lib/audio/backend/detail/PipeWireMonitorParsing.h"
#include <ao/Error.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/pod/body.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_map>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    ::spa_pod const* nativeFormat(std::span<std::byte> buffer,
                                  std::uint32_t encoding,
                                  std::uint32_t channels,
                                  std::span<std::uint32_t const> positions,
                                  std::uint32_t sampleRate = 48000)
    {
      auto info = ::spa_audio_info_raw{};
      info.format = static_cast<::spa_audio_format>(encoding);
      info.rate = sampleRate;
      info.channels = channels;
      info.flags = positions.empty() ? SPA_AUDIO_FLAG_UNPOSITIONED : 0U;
      std::ranges::copy(positions, std::span<std::uint32_t>{info.position}.begin());
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
      return ::spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info);
    }

    ::spa_pod const* nativeFixedChoiceFormat(std::span<std::byte> buffer,
                                             std::span<std::uint32_t const> positions,
                                             std::uint32_t positionChoice = SPA_CHOICE_None,
                                             std::span<std::uint32_t const> retainedPositions = {})
    {
      // Independently authored native shape: EnumFormat object ID survives
      // fixation, with three retained S16 values and fixed scalar choices.
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
      auto object = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
      ::spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
      ::spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_format, 0);
      auto encoding = ::spa_pod_frame{};
      ::spa_pod_builder_push_choice(&builder, &encoding, SPA_CHOICE_None, 0);
      ::spa_pod_builder_id(&builder, SPA_AUDIO_FORMAT_S16_LE);
      ::spa_pod_builder_id(&builder, SPA_AUDIO_FORMAT_S16_LE);
      ::spa_pod_builder_id(&builder, SPA_AUDIO_FORMAT_S16_LE);
      std::ignore = ::spa_pod_builder_pop(&builder, &encoding);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_rate, 0);
      auto rate = ::spa_pod_frame{};
      ::spa_pod_builder_push_choice(&builder, &rate, SPA_CHOICE_None, 0);
      ::spa_pod_builder_int(&builder, 48000);
      ::spa_pod_builder_int(&builder, 48000);
      ::spa_pod_builder_int(&builder, 48000);
      std::ignore = ::spa_pod_builder_pop(&builder, &rate);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_channels, 0);
      auto channels = ::spa_pod_frame{};
      ::spa_pod_builder_push_choice(&builder, &channels, SPA_CHOICE_None, 0);
      ::spa_pod_builder_int(&builder, 2);
      ::spa_pod_builder_int(&builder, 2);
      ::spa_pod_builder_int(&builder, 2);
      std::ignore = ::spa_pod_builder_pop(&builder, &channels);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
      REQUIRE(positions.size() == 2U);
      REQUIRE((retainedPositions.empty() || retainedPositions.size() == 2U));
      // Literal native Choice -> Array body: child.size=16, with an inner
      // Id child of width 4 and two slot IDs. Later array bodies are storage
      // retained by fixation, not additional current values of ChoiceNone.
      std::uint32_t const storedValues = retainedPositions.empty() ? 1U : 3U;
      auto const header = std::to_array<std::uint32_t>(
        {16U + (16U * storedValues), SPA_TYPE_Choice, positionChoice, 0U, 16U, SPA_TYPE_Array, 4U, SPA_TYPE_Id});
      std::ignore = ::spa_pod_builder_raw(&builder, header.data(), sizeof(header));
      std::ignore = ::spa_pod_builder_raw(&builder, positions.data(), 2U * sizeof(std::uint32_t));

      if (!retainedPositions.empty())
      {
        auto const arrayHeader = std::to_array<std::uint32_t>({4U, SPA_TYPE_Id});

        for (std::uint32_t slot = 0; slot < 2U; ++slot)
        {
          std::ignore = ::spa_pod_builder_raw(&builder, arrayHeader.data(), sizeof(arrayHeader));
          std::ignore = ::spa_pod_builder_raw(&builder, retainedPositions.data(), 2U * sizeof(std::uint32_t));
        }
      }

      return static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &object));
    }

    void requireSigned16EncodingOffer(::spa_pod const* pod)
    {
      auto const* property = ::spa_pod_find_prop(pod, nullptr, SPA_FORMAT_AUDIO_format);
      REQUIRE(property != nullptr);
      REQUIRE(::spa_pod_is_choice(&property->value));
      std::uint32_t count = 0;
      std::uint32_t choice = 0;
      auto const* values = ::spa_pod_get_values(&property->value, &count, &choice);
      REQUIRE(values != nullptr);
      REQUIRE(choice == SPA_CHOICE_Enum);
      REQUIRE(count == 2U);
      REQUIRE(values->type == SPA_TYPE_Id);
      REQUIRE(values->size == sizeof(std::uint32_t));
      auto encodings = std::array<std::uint32_t, 2>{};
      std::memcpy(encodings.data(), SPA_POD_BODY_CONST(values), sizeof(encodings));
      CHECK(encodings == std::array<std::uint32_t, 2>{SPA_AUDIO_FORMAT_S16_LE, SPA_AUDIO_FORMAT_S16_LE});
    }
  } // namespace

  TEST_CASE("PipeWireFormatParsing - offers independently specified canonical two-channel roles")
  {
    struct OfferCase final
    {
      std::uint32_t mask;
      std::array<std::uint32_t, 2> roles;
    };
    auto const cases = std::array{OfferCase{0x0, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}},
                                  OfferCase{0x3, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}},
                                  OfferCase{0x30, {SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR}},
                                  OfferCase{0x5, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FC}}};
    auto const encodings = std::to_array({SampleEncoding::Signed16Le});

    for (auto const& fixture : cases)
    {
      CAPTURE(fixture.mask);
      auto buffer = std::array<std::byte, 1024>{};
      auto const* pod = buildRawStreamFormatOffer(
        buffer, {.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = fixture.mask}, encodings);
      REQUIRE(pod != nullptr);
      requireSigned16EncodingOffer(pod);
      auto info = ::spa_audio_info_raw{};
      // SPA returns the number of parsed properties; only negative values fail.
      REQUIRE(::spa_format_audio_raw_parse(pod, &info) >= 0);
      CHECK(info.rate == 48000U);
      CHECK(info.channels == 2U);
      CHECK(info.position[0] == fixture.roles[0]);
      CHECK(info.position[1] == fixture.roles[1]);
      CHECK((info.flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
    }
  }

  TEST_CASE("PipeWireFormatParsing - zero and FL FR stereo identities install the same native byte transport")
  {
    struct NativeOrderCase final
    {
      std::array<std::uint32_t, 2> positions;
      std::array<std::uint8_t, 2> sourceSlots;
      std::uint32_t canonicalMask;
      bool needsPermutation;
      std::array<std::byte, 8> expected;
    };
    // Two S16LE frames: L=0x1211, R=0x2221; L=-2, R=0x4241.
    // Expected native bytes are literal oracles, not another delivery result.
    auto const canonical = std::to_array<std::byte>({std::byte{0x11},
                                                     std::byte{0x12},
                                                     std::byte{0x21},
                                                     std::byte{0x22},
                                                     std::byte{0xfe},
                                                     std::byte{0xff},
                                                     std::byte{0x41},
                                                     std::byte{0x42}});
    auto const cases = std::array{NativeOrderCase{{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR},
                                                  {0, 1},
                                                  3,
                                                  false,
                                                  {std::byte{0x11},
                                                   std::byte{0x12},
                                                   std::byte{0x21},
                                                   std::byte{0x22},
                                                   std::byte{0xfe},
                                                   std::byte{0xff},
                                                   std::byte{0x41},
                                                   std::byte{0x42}}},
                                  NativeOrderCase{{SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL},
                                                  {1, 0},
                                                  0,
                                                  true,
                                                  {std::byte{0x21},
                                                   std::byte{0x22},
                                                   std::byte{0x11},
                                                   std::byte{0x12},
                                                   std::byte{0x41},
                                                   std::byte{0x42},
                                                   std::byte{0xfe},
                                                   std::byte{0xff}}}};
    auto const encodings = std::to_array({SampleEncoding::Signed16Le});

    for (auto const sourceMask : std::to_array<std::uint32_t>({0, 3}))
    {
      auto const source =
        SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = sourceMask};
      auto offerBuffer = std::array<std::byte, 1024>{};
      auto const* offer = buildRawStreamFormatOffer(offerBuffer, source, encodings);
      REQUIRE(offer != nullptr);
      requireSigned16EncodingOffer(offer);
      auto offered = ::spa_audio_info_raw{};
      REQUIRE(::spa_format_audio_raw_parse(offer, &offered) >= 0);
      // The offer's encoding remains an unfixed Enum choice, checked above;
      // SPA's fixed-format parser does not project that choice into format.
      CHECK(offered.rate == 48000U);
      CHECK(offered.channels == 2U);
      CHECK((offered.flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      CHECK(offered.position[0] == SPA_AUDIO_CHANNEL_FL);
      CHECK(offered.position[1] == SPA_AUDIO_CHANNEL_FR);

      for (auto const& fixture : cases)
      {
        CAPTURE(sourceMask, fixture.needsPermutation);
        auto nativeBuffer = std::array<std::byte, 1024>{};
        auto const optNative =
          parseRawStreamObservation(nativeFormat(nativeBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, fixture.positions));
        REQUIRE(optNative);
        CHECK(optNative->format.sampleRate == 48000U);
        CHECK(optNative->format.channels == 2U);
        CHECK(optNative->format.encoding == SampleEncoding::Signed16Le);
        CHECK(optNative->format.speakerMask == fixture.canonicalMask);
        CHECK(optNative->nativeSpeakerMask == 3U);
        CHECK_FALSE(optNative->hasUnsupportedPositions);
        CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
        CHECK(optNative->positions[0] == fixture.positions[0]);
        CHECK(optNative->positions[1] == fixture.positions[1]);
        auto const deliveryRes = rawStreamDelivery(source, *optNative);
        REQUIRE(deliveryRes);
        CHECK(deliveryRes->clientFormat ==
              PcmFormat{
                .sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le, .speakerMask = sourceMask});
        CHECK(deliveryRes->channelDelivery.isMapped);
        CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 3U);
        CHECK_FALSE(deliveryRes->channelDelivery.hasSharedConversion);
        CHECK(deliveryRes->needsPermutation == fixture.needsPermutation);
        CHECK(deliveryRes->sourceSlots[0] == fixture.sourceSlots[0]);
        CHECK(deliveryRes->sourceSlots[1] == fixture.sourceSlots[1]);
        auto bytes = canonical;
        applyRawStreamDelivery(bytes, *deliveryRes);
        CHECK(bytes == fixture.expected);
      }
    }
  }

  TEST_CASE("PipeWireFormatParsing - fixed native array choices retain actual current positions")
  {
    struct NativeCase final
    {
      std::array<std::uint32_t, 2> positions;
      std::uint32_t nativeMask;
      std::uint32_t canonicalMask;
    };
    auto const cases = std::array{NativeCase{{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}, 0x3, 0x3},
                                  NativeCase{{SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL}, 0x3, 0},
                                  NativeCase{{SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR}, 0x30, 0x30},
                                  NativeCase{{SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_RL}, 0x30, 0},
                                  NativeCase{{SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FC}, 0x5, 0x5}};

    for (auto const& fixture : cases)
    {
      for (bool const hasRetainedValues : {false, true})
      {
        CAPTURE(fixture.nativeMask, fixture.canonicalMask, hasRetainedValues);
        auto buffer = std::array<std::byte, 1024>{};
        auto const retained = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR});
        auto const* pod = nativeFixedChoiceFormat(
          buffer,
          fixture.positions,
          SPA_CHOICE_None,
          hasRetainedValues ? std::span<std::uint32_t const>{retained} : std::span<std::uint32_t const>{});
        REQUIRE(pod != nullptr);
        CHECK(::spa_pod_is_object_id(pod, SPA_PARAM_EnumFormat));
        auto const* property = ::spa_pod_find_prop(pod, nullptr, SPA_FORMAT_AUDIO_position);
        REQUIRE(property != nullptr);
        auto body = ::spa_pod_choice_body{};
        REQUIRE(property->value.type == SPA_TYPE_Choice);
        REQUIRE(property->value.size >= sizeof(body));
        std::memcpy(&body, SPA_POD_BODY_CONST(&property->value), sizeof(body));
        CHECK(body.type == SPA_CHOICE_None);
        CHECK(body.child.type == SPA_TYPE_Array);
        CHECK(body.child.size == sizeof(::spa_pod_array_body) + (2U * sizeof(std::uint32_t)));
        auto info = ::spa_audio_info_raw{};
        REQUIRE(::spa_format_audio_raw_parse(pod, &info) >= 0);
        CHECK(info.rate == 48000U);
        CHECK(info.channels == 2U);
        CHECK(info.format == SPA_AUDIO_FORMAT_S16_LE);
        CHECK(info.position[0] == fixture.positions[0]);
        CHECK(info.position[1] == fixture.positions[1]);
        auto const optNative = parseRawStreamObservation(pod);
        REQUIRE(optNative);
        CHECK(optNative->format.sampleRate == 48000U);
        CHECK(optNative->format.channels == 2U);
        CHECK(optNative->format.encoding == SampleEncoding::Signed16Le);
        CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
        CHECK(optNative->positions[0] == fixture.positions[0]);
        CHECK(optNative->positions[1] == fixture.positions[1]);
        CHECK(optNative->nativeSpeakerMask == fixture.nativeMask);
        CHECK(optNative->format.speakerMask == fixture.canonicalMask);
        auto const optMonitor = parseRawMonitorObservation(pod);
        REQUIRE(optMonitor);
        CHECK(*optMonitor == *optNative);
      }
    }
  }

  TEST_CASE("PipeWireFormatParsing - live native position choices are not fixed observations")
  {
    auto const positions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto const alternatives = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});

    for (auto const choice : {SPA_CHOICE_Enum, SPA_CHOICE_Range})
    {
      CAPTURE(choice);
      auto buffer = std::array<std::byte, 1024>{};
      auto const* pod = nativeFixedChoiceFormat(buffer, positions, choice, alternatives);
      REQUIRE(pod != nullptr);
      CHECK_FALSE(parseRawStreamObservation(pod));
      CHECK_FALSE(parseRawMonitorObservation(pod));
    }
  }

  TEST_CASE("PipeWireFormatParsing - live scalar choices cannot become current numerical observations")
  {
    auto const positions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});

    for (auto const choice : {SPA_CHOICE_Enum, SPA_CHOICE_Range})
    {
      for (auto const key : {SPA_FORMAT_AUDIO_format, SPA_FORMAT_AUDIO_rate, SPA_FORMAT_AUDIO_channels})
      {
        CAPTURE(choice, key);
        auto buffer = std::array<std::byte, 1024>{};
        auto const* pod = nativeFixedChoiceFormat(buffer, positions);
        REQUIRE(pod != nullptr);
        auto const* property = ::spa_pod_find_prop(pod, nullptr, key);
        REQUIRE(property != nullptr);
        REQUIRE(property->value.type == SPA_TYPE_Choice);
        auto body = ::spa_pod_choice_body{};
        std::memcpy(&body, SPA_POD_BODY_CONST(&property->value), sizeof(body));
        body.type = choice;
        auto const offset =
          static_cast<std::size_t>(std::as_bytes(std::span{&property->value, 1}).data() - buffer.data());
        std::memcpy(buffer.data() + offset + sizeof(::spa_pod), &body, sizeof(body));
        CHECK_FALSE(parseRawStreamObservation(pod));
        CHECK_FALSE(parseRawMonitorObservation(pod));
      }
    }
  }

  TEST_CASE("PipeWireFormatParsing - unpadded object storage cannot become fresh monitor facts")
  {
    auto initialBuffer = std::array<std::byte, 512>{};
    auto currentBuffer = std::array<std::byte, 512>{};
    auto const initialRoles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
    auto const* initial = nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S32_LE, 2, initialRoles, 44100);
    auto const* current = nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, {});
    REQUIRE(initial != nullptr);
    REQUIRE(current != nullptr);
    REQUIRE(current->size == 128U);
    auto validNative = ::spa_audio_info_raw{};
    REQUIRE(::spa_format_audio_raw_parse(current, &validNative) >= 0);
    CHECK(validNative.format == SPA_AUDIO_FORMAT_S16_LE);
    CHECK(validNative.rate == 48000U);
    CHECK(validNative.channels == 2U);
    auto header = *current;
    // Direct lookup can still reach the complete final channels value, but the
    // enclosing native parser also requires the object's padded storage extent.
    header.size = 124U;
    std::memcpy(currentBuffer.data(), &header, sizeof(header));

    auto const* channelsProperty = ::spa_pod_find_prop(current, nullptr, SPA_FORMAT_AUDIO_channels);
    REQUIRE(channelsProperty != nullptr);
    std::int32_t channels = 0;
    CHECK(::spa_pod_get_int(&channelsProperty->value, &channels) == 0);
    CHECK(channels == 2);
    auto native = ::spa_audio_info_raw{};
    CHECK(::spa_format_audio_raw_parse(current, &native) == -EPIPE);
    CHECK_FALSE(parseRawStreamObservation(current));
    CHECK_FALSE(parseRawMonitorObservation(current));
    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, initial);
    REQUIRE(cache.contains(42));
    REQUIRE(cache.at(42).nativeSpeakerMask == 0x30U);
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, current);
    CHECK_FALSE(cache.contains(42));
  }

  TEST_CASE("PipeWireFormatParsing - malformed fixed scalar storage rejects readback and invalidates current cache")
  {
    struct ScalarStorageCase final
    {
      char const* name;
      std::uint32_t valueSize;
      std::uint32_t childSize;
      bool hasWrongChildType;
    };
    // Choice body = 16 bytes, followed by three four-byte scalar values.
    // The fractional case retains a complete first value but truncates a later one.
    auto const cases = std::to_array<ScalarStorageCase>({
      {"truncated choice header", 15, 4, false},
      {"zero child width", 28, 0, false},
      {"wrong child width", 28, 8, false},
      {"wrong child type", 28, 4, true},
      {"fractional retained payload", 27, 4, false},
      {"no current scalar payload", 16, 4, false},
    });
    auto const positions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});

    for (auto const key : {SPA_FORMAT_AUDIO_format, SPA_FORMAT_AUDIO_rate, SPA_FORMAT_AUDIO_channels})
    {
      for (auto const& fixture : cases)
      {
        CAPTURE(key, fixture.name);
        auto buffer = std::array<std::byte, 1024>{};
        auto const* pod = nativeFixedChoiceFormat(buffer, positions, SPA_CHOICE_None, positions);
        REQUIRE(pod != nullptr);
        auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
        updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
        REQUIRE(cache.contains(42));
        REQUIRE(cache.at(42).format.sampleRate == 48000U);
        REQUIRE(cache.at(42).format.channels == 2U);
        REQUIRE(cache.at(42).format.encoding == SampleEncoding::Signed16Le);
        REQUIRE(cache.at(42).nativeSpeakerMask == 3U);
        REQUIRE(parseRawStreamObservation(pod));
        auto const* property = ::spa_pod_find_prop(pod, nullptr, key);
        REQUIRE(property != nullptr);
        auto value = property->value;
        REQUIRE(value.type == SPA_TYPE_Choice);
        REQUIRE(value.size == 28U);
        auto body = ::spa_pod_choice_body{};
        std::memcpy(&body, SPA_POD_BODY_CONST(&property->value), sizeof(body));
        REQUIRE(body.type == SPA_CHOICE_None);
        REQUIRE(body.child.size == 4U);
        body.child.size = fixture.childSize;

        if (fixture.hasWrongChildType)
        {
          body.child.type = key == SPA_FORMAT_AUDIO_format ? SPA_TYPE_Int : SPA_TYPE_Id;
        }

        auto const valueOffset =
          static_cast<std::size_t>(std::as_bytes(std::span{&property->value, 1}).data() - buffer.data());
        value.size = fixture.valueSize;

        if (fixture.valueSize <= 16U)
        {
          // Keep the enclosing object and later properties well formed: the
          // shorter choice occupies 32 rather than 48 padded property bytes.
          // Thus rejection must inspect the scalar, not a stale outer extent.
          auto const nextPropertyOffset = valueOffset + 40U;
          auto header = *pod;
          auto const suffixSize = sizeof(::spa_pod) + header.size - nextPropertyOffset;
          std::memmove(buffer.data() + nextPropertyOffset - 16U, buffer.data() + nextPropertyOffset, suffixSize);
          header.size -= 16U;
          std::memcpy(buffer.data(), &header, sizeof(header));
        }

        std::memcpy(buffer.data() + valueOffset, &value, sizeof(value));
        std::memcpy(buffer.data() + valueOffset + sizeof(value), &body, sizeof(body));
        CHECK_FALSE(parseRawStreamObservation(pod));
        CHECK_FALSE(parseRawMonitorObservation(pod));
        CHECK_FALSE(currentFormatFromNodeParam(SPA_PARAM_Format, pod));
        updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
        CHECK_FALSE(cache.contains(42));
      }
    }
  }

  TEST_CASE("PipeWireFormatParsing - malformed fixed position array storage stays rejected")
  {
    auto const positions = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto buffer = std::array<std::byte, 1024>{};
    auto const* pod = nativeFixedChoiceFormat(buffer, positions, SPA_CHOICE_None, positions);
    REQUIRE(pod != nullptr);
    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
    REQUIRE(cache.contains(42));
    REQUIRE(cache.at(42).format.sampleRate == 48000U);
    REQUIRE(cache.at(42).format.channels == 2U);
    REQUIRE(cache.at(42).format.encoding == SampleEncoding::Signed16Le);
    REQUIRE(cache.at(42).nativeSpeakerMask == 3U);
    auto const* property = ::spa_pod_find_prop(pod, nullptr, SPA_FORMAT_AUDIO_position);
    REQUIRE(property != nullptr);
    auto value = property->value;
    auto choice = ::spa_pod_choice_body{};
    std::memcpy(&choice, SPA_POD_BODY_CONST(&property->value), sizeof(choice));
    auto array = ::spa_pod_array_body{};
    auto const* arrayData = static_cast<std::byte const*>(SPA_POD_BODY_CONST(&property->value)) + sizeof(choice);
    std::memcpy(&array, arrayData, sizeof(array));

    SECTION("truncated choice header")
    {
      value.size = sizeof(choice) - 1U;
    }

    SECTION("wrong active child type")
    {
      choice.child.type = SPA_TYPE_Id;
    }

    SECTION("zero active child size")
    {
      choice.child.size = 0;
    }

    SECTION("short active array header")
    {
      choice.child.size = sizeof(array) - 1U;
    }

    SECTION("active child extends beyond the outer choice")
    {
      choice.child.size = value.size - sizeof(choice) + 4U;
    }

    SECTION("truncated retained value cannot be hidden after a valid first array")
    {
      --value.size;
    }

    SECTION("active array contains a fractional Id value")
    {
      choice.child.size = sizeof(array) + 7U;
      auto const declaredSize = sizeof(choice) + (static_cast<std::size_t>(choice.child.size) * 3U);
      REQUIRE(declaredSize <= property->value.size);
      value.size = static_cast<std::uint32_t>(declaredSize);
    }

    SECTION("zero Id element size")
    {
      array.child.size = 0;
    }

    SECTION("wrong Id element width")
    {
      array.child.size = 8;
    }

    SECTION("wrong array element type")
    {
      array.child.type = SPA_TYPE_Int;
    }

    auto const valueOffset =
      static_cast<std::size_t>(std::as_bytes(std::span{&property->value, 1}).data() - buffer.data());
    std::memcpy(buffer.data() + valueOffset, &value, sizeof(value));
    std::memcpy(buffer.data() + valueOffset + sizeof(value), &choice, sizeof(choice));
    std::memcpy(buffer.data() + valueOffset + sizeof(value) + sizeof(choice), &array, sizeof(array));
    // Position is the final property. Keep its enclosing object extent valid
    // even when the malformed value declares a shorter payload.
    auto header = *pod;
    auto const paddedValueSize = (sizeof(value) + static_cast<std::size_t>(value.size) + 7U) & ~std::size_t{7U};
    header.size = static_cast<std::uint32_t>(valueOffset + paddedValueSize - sizeof(header));
    std::memcpy(buffer.data(), &header, sizeof(header));
    CHECK_FALSE(parseRawStreamObservation(pod));
    CHECK_FALSE(parseRawMonitorObservation(pod));
    CHECK_FALSE(currentFormatFromNodeParam(SPA_PARAM_Format, pod));
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
    CHECK_FALSE(cache.contains(42));
  }

  TEST_CASE("PipeWireFormatParsing - retains unpositioned mono and multichannel offers")
  {
    auto const encodings = std::to_array({SampleEncoding::Signed16Le});

    for (auto const source :
         {SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 1},
          SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16, .speakerMask = 4},
          SignalFormat{.sampleRate = 48000, .channels = 4, .precisionBits = 16, .speakerMask = 0x33}})
    {
      auto buffer = std::array<std::byte, 1024>{};
      auto const* pod = buildRawStreamFormatOffer(buffer, source, encodings);
      REQUIRE(pod != nullptr);
      requireSigned16EncodingOffer(pod);
      auto info = ::spa_audio_info_raw{};
      REQUIRE(::spa_format_audio_raw_parse(pod, &info) >= 0);
      CHECK(info.rate == 48000U);
      CHECK(info.channels == source.channels);
      CHECK((info.flags & SPA_AUDIO_FLAG_UNPOSITIONED) != 0U);
      CHECK(::spa_pod_find_prop(pod, nullptr, SPA_FORMAT_AUDIO_position) == nullptr);
      // An encoding choice is an offer, not a fixed negotiated observation.
      CHECK_FALSE(parseRawStreamObservation(pod));
      auto nativeBuffer = std::array<std::byte, 1024>{};
      auto const optNative =
        parseRawStreamObservation(nativeFormat(nativeBuffer, SPA_AUDIO_FORMAT_S16_LE, source.channels, {}));
      REQUIRE(optNative);
      CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) != 0U);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK(optNative->nativeSpeakerMask == 0U);
      auto const deliveryRes = rawStreamDelivery(source, *optNative);
      REQUIRE(deliveryRes);
      CHECK(deliveryRes->clientFormat.speakerMask == source.speakerMask);
      CHECK_FALSE(deliveryRes->channelDelivery.isMapped);
      CHECK_FALSE(deliveryRes->needsPermutation);
    }
  }

  TEST_CASE("PipeWireFormatParsing - actual positions never inherit the application request")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x30};

    SECTION("different known positions remain an inconsistent successful negotiation")
    {
      auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
      auto const* pod = nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles);
      auto const optNative = parseRawStreamObservation(pod);
      REQUIRE(optNative);
      CHECK(optNative->format.speakerMask == 0x3U);
      auto const deliveryRes = rawStreamDelivery(source, *optNative);
      REQUIRE_FALSE(deliveryRes);
      CHECK(deliveryRes.error().error.code == Error::Code::FormatRejected);
      CHECK(deliveryRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("missing positions are retained as unpositioned but cannot confirm an exact offer")
    {
      auto const* pod = nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, {});
      auto const optNative = parseRawStreamObservation(pod);
      REQUIRE(optNative);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) != 0U);
      CHECK_FALSE(rawStreamDelivery(source, *optNative));
    }

    SECTION("unknown explicit positions cannot acquire the offered identities")
    {
      auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN});
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles));
      REQUIRE(optNative);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK_FALSE(rawStreamDelivery(source, *optNative));
    }

    SECTION("duplicate known positions are malformed")
    {
      auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FL});
      CHECK_FALSE(parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles)));
    }

    SECTION("partial positions are valid observations but cannot satisfy an exact stereo offer")
    {
      auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_UNKNOWN});
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles));
      REQUIRE(optNative);
      CHECK(optNative->nativeSpeakerMask == 1U);
      CHECK(optNative->format.speakerMask == 0U);
      auto const deliveryRes = rawStreamDelivery(source, *optNative);
      REQUIRE_FALSE(deliveryRes);
      CHECK(deliveryRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("PipeWireFormatParsing - negotiated same-set order transports independent marker bytes")
  {
    struct EncodingCase final
    {
      SampleEncoding encoding;
      std::uint32_t nativeEncoding;
      std::size_t sampleBytes;
    };
    auto const cases = std::array{EncodingCase{SampleEncoding::Signed16Le, SPA_AUDIO_FORMAT_S16_LE, 2},
                                  EncodingCase{SampleEncoding::Signed24PackedLe, SPA_AUDIO_FORMAT_S24_LE, 3},
                                  EncodingCase{SampleEncoding::Signed24In32Le, SPA_AUDIO_FORMAT_S24_32_LE, 4},
                                  EncodingCase{SampleEncoding::Signed32Le, SPA_AUDIO_FORMAT_S32_LE, 4},
                                  EncodingCase{SampleEncoding::Float32Le, SPA_AUDIO_FORMAT_F32_LE, 4}};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_RL});
    auto const canonical = std::to_array<std::byte>({std::byte{0x11},
                                                     std::byte{0x12},
                                                     std::byte{0x13},
                                                     std::byte{0x00},
                                                     std::byte{0x21},
                                                     std::byte{0x22},
                                                     std::byte{0x23},
                                                     std::byte{0x00}});
    auto const expected = std::to_array<std::byte>({std::byte{0x21},
                                                    std::byte{0x22},
                                                    std::byte{0x23},
                                                    std::byte{0x00},
                                                    std::byte{0x11},
                                                    std::byte{0x12},
                                                    std::byte{0x13},
                                                    std::byte{0x00}});

    for (auto const& fixture : cases)
    {
      CAPTURE(fixture.encoding);
      auto buffer = std::array<std::byte, 1024>{};
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, fixture.nativeEncoding, 2, roles));
      REQUIRE(optNative);
      CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_RR);
      CHECK(optNative->nativeSpeakerMask == 0x30U);
      CHECK(optNative->format.speakerMask == 0U);
      auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 2, .speakerMask = 0x30}, *optNative);
      REQUIRE(deliveryRes);
      CHECK(deliveryRes->clientFormat.encoding == fixture.encoding);
      CHECK(deliveryRes->clientFormat.speakerMask == 0x30U);
      CHECK(deliveryRes->channelDelivery.isMapped);
      CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 0x30U);
      CHECK(deliveryRes->needsPermutation);
      auto bytes = std::array<std::byte, 8>{};
      std::ranges::copy(std::span{canonical}.first(fixture.sampleBytes), bytes.begin());
      std::ranges::copy(std::span{canonical}.subspan(4, fixture.sampleBytes), bytes.begin() + fixture.sampleBytes);
      applyRawStreamDelivery(std::span{bytes}.first(fixture.sampleBytes * 2), *deliveryRes);
      CHECK(std::ranges::equal(
        std::span{bytes}.first(fixture.sampleBytes), std::span{expected}.first(fixture.sampleBytes)));
      CHECK(std::ranges::equal(std::span{bytes}.subspan(fixture.sampleBytes, fixture.sampleBytes),
                               std::span{expected}.subspan(4, fixture.sampleBytes)));
    }
  }

  TEST_CASE("PipeWireFormatParsing - a known legacy multichannel order is preserved without a new position offer")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_FR});
    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles));
    REQUIRE(optNative);
    auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4, .speakerMask = 0x33}, *optNative);
    REQUIRE(deliveryRes);
    // Complete observed WAVE set, including non-canonical slot order.
    CHECK(deliveryRes->channelDelivery.isMapped);
    CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 0x33U);
    CHECK(optNative->format.speakerMask == 0U);
    auto bytes = std::to_array<std::byte>({std::byte{0x11},
                                           std::byte{0x12},
                                           std::byte{0x21},
                                           std::byte{0x22},
                                           std::byte{0x31},
                                           std::byte{0x32},
                                           std::byte{0x41},
                                           std::byte{0x42}});
    auto const expected = std::to_array<std::byte>({std::byte{0x41},
                                                    std::byte{0x42},
                                                    std::byte{0x11},
                                                    std::byte{0x12},
                                                    std::byte{0x31},
                                                    std::byte{0x32},
                                                    std::byte{0x21},
                                                    std::byte{0x22}});
    applyRawStreamDelivery(bytes, *deliveryRes);
    CHECK(bytes == expected);
  }

  TEST_CASE("PipeWireFormatParsing - later position or flag changes cannot replace the initial mapping")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto const optInitial = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles));
    REQUIRE(optInitial);
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto currentRoles = roles;
    auto currentPositions = std::span<std::uint32_t const>{currentRoles};

    SECTION("same-set slot order changed after activation")
    {
      currentRoles = {SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL};
    }

    SECTION("positions became unavailable after activation")
    {
      currentPositions = {};
    }

    SECTION("known native role set changed after activation")
    {
      currentRoles = {SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
    }

    auto const optCurrent =
      parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, currentPositions));
    REQUIRE(optCurrent);

    for (auto const sourceMask : std::to_array<std::uint32_t>({0, 3}))
    {
      CAPTURE(sourceMask);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .speakerMask = sourceMask};
      auto const unchangedRes = rawStreamDelivery(source, *optInitial, optInitial);
      REQUIRE(unchangedRes);
      CHECK(unchangedRes->clientFormat.speakerMask == sourceMask);
      CHECK(unchangedRes->channelDelivery.isMapped);
      CHECK(unchangedRes->channelDelivery.nativeSpeakerMask == 3U);
      CHECK_FALSE(unchangedRes->needsPermutation);
      auto const changedRes = rawStreamDelivery(source, *optCurrent, optInitial);
      REQUIRE_FALSE(changedRes);
      CHECK(changedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(changedRes.error().error.message == "PipeWire changed PCM mode after stream activation");
      CHECK(changedRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("PipeWireFormatParsing - source-zero legacy active role refresh keeps identity transport")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const left = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL});
    auto const center = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FC});
    auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, left));
    auto const optCurrent = parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, center));
    REQUIRE(optInitial);
    REQUIRE(optCurrent);
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16};
    auto const initialRes = rawStreamDelivery(source, *optInitial);
    REQUIRE(initialRes);
    CHECK_FALSE(initialRes->needsPermutation);
    CHECK(initialRes->channelDelivery.nativeSpeakerMask == 1U);
    auto const currentRes = rawStreamDelivery(source, *optCurrent, optInitial);
    REQUIRE(currentRes);
    CHECK(currentRes->clientFormat ==
          PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le});
    CHECK(currentRes->channelDelivery.nativeSpeakerMask == 4U);
    // Complete observed mono role; identity transport stays unpermuted.
    CHECK(currentRes->channelDelivery.isMapped);
    CHECK_FALSE(currentRes->needsPermutation);
    auto bytes = std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}});
    applyRawStreamDelivery(bytes, *currentRes);
    CHECK(bytes == std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}}));
  }

  TEST_CASE("PipeWireFormatParsing - source-zero multichannel active role refresh keeps identity transport")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const quad = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
    auto const changed = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR});
    auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, quad));
    auto const optCurrent = parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, changed));
    REQUIRE(optInitial);
    REQUIRE(optCurrent);
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 4};
    auto const initialRes = rawStreamDelivery(source, *optInitial);
    REQUIRE(initialRes);
    CHECK_FALSE(initialRes->needsPermutation);
    auto const currentRes = rawStreamDelivery(source, *optCurrent, optInitial);
    REQUIRE(currentRes);
    CHECK(currentRes->clientFormat.speakerMask == 0U);
    CHECK(currentRes->channelDelivery.nativeSpeakerMask == 0x60cU);
    // Complete observed quad; zero client mask and identity transport stay separate.
    CHECK(currentRes->channelDelivery.isMapped);
    CHECK_FALSE(currentRes->needsPermutation);
  }

  TEST_CASE("PipeWireFormatParsing - the open snapshot projection follows the latest accepted native roles")
  {
    // Wiring proof for the PipeWireBackend::open() snapshot boundary: that
    // boundary re-runs this same active pure seam on the latest already
    // accepted observation under the loop lock, so the calls below reproduce
    // exactly the reported channelDelivery projection. This is not a native
    // callback-schedule reproduction: the sequential param_changed events
    // that produce these observations require a live PipeWire daemon, and
    // their acceptance semantics are covered by the refresh cases above.
    SECTION("an accepted non-stereo identity refresh projects the refreshed role")
    {
      auto initialBuffer = std::array<std::byte, 1024>{};
      auto currentBuffer = std::array<std::byte, 1024>{};
      auto const left = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL});
      auto const center = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FC});
      auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, left));
      auto const optCurrent =
        parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, center));
      REQUIRE(optInitial);
      REQUIRE(optCurrent);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 1, .precisionBits = 16};
      auto const admittedRes = rawStreamDelivery(source, *optInitial);
      REQUIRE(admittedRes);
      CHECK(admittedRes->channelDelivery.nativeSpeakerMask == 1U);
      auto const refreshRes = rawStreamDelivery(source, *optCurrent, optInitial);
      REQUIRE(refreshRes);
      auto const projectionRes = rawStreamDelivery(source, *optCurrent);
      REQUIRE(projectionRes);
      CHECK(projectionRes->channelDelivery.nativeSpeakerMask == 4U);
      CHECK(projectionRes->channelDelivery.nativeSpeakerMask != admittedRes->channelDelivery.nativeSpeakerMask);
      // Complete observed role. The admitted identity transport stays installed:
      // the projection installs no permutation, so client bytes stay unchanged.
      CHECK(projectionRes->channelDelivery.isMapped);
      CHECK_FALSE(projectionRes->needsPermutation);
      auto bytes = std::array<std::byte, 2>{std::byte{0x11}, std::byte{0x12}};
      applyRawStreamDelivery(bytes, *projectionRes);
      CHECK(bytes == std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}}));
    }

    SECTION("an accepted multichannel role refresh projects the refreshed set")
    {
      auto initialBuffer = std::array<std::byte, 1024>{};
      auto currentBuffer = std::array<std::byte, 1024>{};
      auto const quad = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
      auto const changed = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR});
      auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, quad));
      auto const optCurrent =
        parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, changed));
      REQUIRE(optInitial);
      REQUIRE(optCurrent);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 4};
      auto const admittedRes = rawStreamDelivery(source, *optInitial);
      REQUIRE(admittedRes);
      CHECK(admittedRes->channelDelivery.nativeSpeakerMask == 0x33U);
      auto const refreshRes = rawStreamDelivery(source, *optCurrent, optInitial);
      REQUIRE(refreshRes);
      auto const projectionRes = rawStreamDelivery(source, *optCurrent);
      REQUIRE(projectionRes);
      CHECK(projectionRes->channelDelivery.nativeSpeakerMask == 0x60cU);
      CHECK(projectionRes->channelDelivery.nativeSpeakerMask != admittedRes->channelDelivery.nativeSpeakerMask);
      // Complete observed quad evidence does not install a permutation.
      CHECK(projectionRes->channelDelivery.isMapped);
      CHECK_FALSE(projectionRes->needsPermutation);
      auto bytes = std::to_array<std::byte>({std::byte{0x11},
                                             std::byte{0x12},
                                             std::byte{0x21},
                                             std::byte{0x22},
                                             std::byte{0x31},
                                             std::byte{0x32},
                                             std::byte{0x41},
                                             std::byte{0x42}});
      applyRawStreamDelivery(bytes, *projectionRes);
      CHECK(bytes == std::to_array<std::byte>({std::byte{0x11},
                                               std::byte{0x12},
                                               std::byte{0x21},
                                               std::byte{0x22},
                                               std::byte{0x31},
                                               std::byte{0x32},
                                               std::byte{0x41},
                                               std::byte{0x42}}));
    }

    SECTION("an unchanged source-zero stereo swap projects the installed swap")
    {
      auto buffer = std::array<std::byte, 1024>{};
      auto const swapped = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL});
      auto const optSwapped = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, swapped));
      REQUIRE(optSwapped);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 2};
      auto const admittedRes = rawStreamDelivery(source, *optSwapped);
      REQUIRE(admittedRes);
      REQUIRE(admittedRes->needsPermutation);
      CHECK(admittedRes->sourceSlots[0] == 1U);
      CHECK(admittedRes->sourceSlots[1] == 0U);
      auto const refreshRes = rawStreamDelivery(source, *optSwapped, optSwapped);
      REQUIRE(refreshRes);
      auto const projectionRes = rawStreamDelivery(source, *optSwapped);
      REQUIRE(projectionRes);
      CHECK(projectionRes->channelDelivery == admittedRes->channelDelivery);
      REQUIRE(projectionRes->needsPermutation);
      CHECK(projectionRes->sourceSlots[0] == 1U);
      CHECK(projectionRes->sourceSlots[1] == 0U);
      auto bytes = std::array<std::byte, 4>{std::byte{0x11}, std::byte{0x12}, std::byte{0x21}, std::byte{0x22}};
      applyRawStreamDelivery(bytes, *projectionRes);
      CHECK(bytes == std::to_array<std::byte>({std::byte{0x21}, std::byte{0x22}, std::byte{0x11}, std::byte{0x12}}));
    }
  }

  TEST_CASE("PipeWireFormatParsing - zero and FL FR stereo cannot replace an installed swap after activation")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const swapped = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL});
    auto const identity = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, swapped));
    auto const optCurrent =
      parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, identity));
    REQUIRE(optInitial);
    REQUIRE(optCurrent);

    for (auto const sourceMask : std::to_array<std::uint32_t>({0, 3}))
    {
      CAPTURE(sourceMask);
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 2, .speakerMask = sourceMask};
      auto const installedRes = rawStreamDelivery(source, *optInitial);
      REQUIRE(installedRes);
      CHECK(installedRes->clientFormat.speakerMask == sourceMask);
      CHECK(installedRes->channelDelivery.isMapped);
      CHECK(installedRes->channelDelivery.nativeSpeakerMask == 3U);
      REQUIRE(installedRes->needsPermutation);
      CHECK(installedRes->sourceSlots[0] == 1U);
      CHECK(installedRes->sourceSlots[1] == 0U);
      auto bytes = std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}, std::byte{0x21}, std::byte{0x22}});
      applyRawStreamDelivery(bytes, *installedRes);
      CHECK(bytes == std::to_array<std::byte>({std::byte{0x21}, std::byte{0x22}, std::byte{0x11}, std::byte{0x12}}));
      auto const currentRes = rawStreamDelivery(source, *optCurrent, optInitial);
      REQUIRE_FALSE(currentRes);
      CHECK(currentRes.error().error.code == Error::Code::FormatRejected);
      CHECK(currentRes.error().error.message == "PipeWire changed PCM mode after stream activation");
      CHECK(currentRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }
  }

  TEST_CASE("PipeWireFormatParsing - source-zero legacy geometry or encoding changes remain terminal")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL});
    auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, roles));
    REQUIRE(optInitial);
    std::uint32_t rate = 48000;
    std::uint32_t channels = 1;
    std::uint32_t encoding = SPA_AUDIO_FORMAT_S16_LE;

    SECTION("sample rate changed")
    {
      rate = 44100;
    }

    SECTION("channel count changed")
    {
      channels = 4;
    }

    SECTION("sample encoding changed")
    {
      encoding = SPA_AUDIO_FORMAT_S32_LE;
    }

    auto const optCurrent = parseRawStreamObservation(nativeFormat(currentBuffer, encoding, channels, {}, rate));
    REQUIRE(optCurrent);
    auto const currentRes = rawStreamDelivery({.sampleRate = 48000, .channels = 1}, *optCurrent, optInitial);
    REQUIRE_FALSE(currentRes);
    CHECK(currentRes.error().error.code == Error::Code::FormatRejected);
    CHECK(currentRes.error().error.message == "PipeWire changed PCM mode after stream activation");
    CHECK(currentRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
  }

  TEST_CASE("PipeWireFormatParsing - native MONO remains unknown while known conflicting mono is not hidden")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 1, .speakerMask = 1};
    auto const mono = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_MONO});
    auto const optMono = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 1, mono));
    REQUIRE(optMono);
    CHECK(optMono->format.speakerMask == 0U);
    auto const monoRes = rawStreamDelivery(source, *optMono);
    REQUIRE(monoRes);
    CHECK(monoRes->clientFormat.speakerMask == 1U);
    CHECK_FALSE(monoRes->channelDelivery.isMapped);
    auto const center = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FC});
    auto const optCenter = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 1, center));
    REQUIRE(optCenter);
    CHECK(optCenter->format.speakerMask == 4U);
    auto const conflictRes = rawStreamDelivery(source, *optCenter);
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("PipeWireFormatParsing - partial legacy constraints retain facts through a constraint-preserving bijection")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto roles = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN});
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 4, .speakerMask = 0x33};
    auto expected = std::to_array<std::byte>({std::byte{0x11},
                                              std::byte{0x12},
                                              std::byte{0x21},
                                              std::byte{0x22},
                                              std::byte{0x31},
                                              std::byte{0x32},
                                              std::byte{0x41},
                                              std::byte{0x42}});
    auto bytes = expected;
    std::uint32_t expectedKnownMask = 1;
    bool needsPermutation = false;
    bool incompatible = false;

    SECTION("matching first-slot FL leaves other roles genuinely unknown")
    {
      roles[0] = SPA_AUDIO_CHANNEL_FL;
    }

    SECTION("first-slot RR is a valid partial constraint completed by a constraint-preserving bijection")
    {
      // Unknown slots receive a deterministic unused-source completion, not observed identities.
      roles[0] = SPA_AUDIO_CHANNEL_RR;
      expectedKnownMask = 0x20;
      needsPermutation = true;
      expected = {std::byte{0x41},
                  std::byte{0x42},
                  std::byte{0x21},
                  std::byte{0x22},
                  std::byte{0x31},
                  std::byte{0x32},
                  std::byte{0x11},
                  std::byte{0x12}};
    }

    SECTION("an identified FC cannot be supplied by quad source bytes")
    {
      roles[0] = SPA_AUDIO_CHANNEL_FC;
      expectedKnownMask = 4;
      incompatible = true;
    }

    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles));
    REQUIRE(optNative);
    CHECK(optNative->nativeSpeakerMask == expectedKnownMask);
    CHECK(optNative->format.speakerMask == 0U);
    auto const deliveryRes = rawStreamDelivery(source, *optNative);

    if (incompatible)
    {
      REQUIRE_FALSE(deliveryRes);
      CHECK(deliveryRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
      return;
    }

    REQUIRE(deliveryRes);
    CHECK_FALSE(deliveryRes->channelDelivery.isMapped);
    CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == expectedKnownMask);
    CHECK(deliveryRes->needsPermutation == needsPermutation);
    applyRawStreamDelivery(bytes, *deliveryRes);
    CHECK(bytes == expected);
  }

  TEST_CASE("PipeWireFormatParsing - identified unsupported roles are not erased into unknown positions")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_RLC, SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN});
    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles));
    REQUIRE(optNative);
    CHECK(optNative->hasUnsupportedPositions);
    CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_RLC);
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 4, .speakerMask = 0x33};
    auto const conflictRes = rawStreamDelivery(source, *optNative);
    REQUIRE_FALSE(conflictRes);
    CHECK(conflictRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    auto unidentified = source;
    unidentified.speakerMask = 0;
    auto const legacyRes = rawStreamDelivery(unidentified, *optNative);
    REQUIRE(legacyRes);
    CHECK(legacyRes->clientFormat.speakerMask == 0);
    CHECK_FALSE(legacyRes->channelDelivery.isMapped);
    CHECK_FALSE(legacyRes->needsPermutation);
    CHECK(optNative->hasUnsupportedPositions);
    CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_RLC);

    auto const laterRes = rawStreamDelivery(source, *optNative, RawStreamFormat{});
    REQUIRE_FALSE(laterRes);
    CHECK(laterRes.error().cause == BackendOpenFailure::Cause::OutputFailure);

    auto const invalid = std::to_array<std::uint32_t>({0xffffffffU});
    CHECK_FALSE(parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 1, invalid)));
    auto const duplicateAux = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_AUX0, SPA_AUDIO_CHANNEL_AUX0});
    CHECK_FALSE(parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, duplicateAux)));
  }

  TEST_CASE("PipeWireFormatParsing - repeated silent NA remains an unsupported constraint")
  {
    auto buffer = std::array<std::byte, 1024>{};

    SECTION("repeat all NA admits zero-source identity transport")
    {
      auto const roles3 =
        std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optThree = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 3, roles3));
      REQUIRE(optThree);
      CHECK(optThree->hasUnsupportedPositions);
      CHECK(optThree->nativeSpeakerMask == 0U);
      CHECK(optThree->format.speakerMask == 0U);
      CHECK((optThree->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      CHECK(optThree->positions[0] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optThree->positions[1] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optThree->positions[2] == SPA_AUDIO_CHANNEL_NA);
      auto const threeRes = rawStreamDelivery({.sampleRate = 48000, .channels = 3}, *optThree);
      REQUIRE(threeRes);
      CHECK(threeRes->clientFormat.sampleRate == 48000U);
      CHECK(threeRes->clientFormat.channels == 3U);
      CHECK(threeRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(threeRes->clientFormat.speakerMask == 0U);
      CHECK(threeRes->channelDelivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(threeRes->channelDelivery.isMapped);
      CHECK_FALSE(threeRes->needsPermutation);

      auto const roles4 = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optFour = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles4));
      REQUIRE(optFour);
      CHECK(optFour->hasUnsupportedPositions);
      CHECK(optFour->nativeSpeakerMask == 0U);
      CHECK(optFour->format.speakerMask == 0U);
      CHECK((optFour->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      CHECK(optFour->positions[0] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optFour->positions[1] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optFour->positions[2] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optFour->positions[3] == SPA_AUDIO_CHANNEL_NA);
      auto const fourRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4}, *optFour);
      REQUIRE(fourRes);
      CHECK(fourRes->clientFormat.sampleRate == 48000U);
      CHECK(fourRes->clientFormat.channels == 4U);
      CHECK(fourRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(fourRes->clientFormat.speakerMask == 0U);
      CHECK(fourRes->channelDelivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(fourRes->channelDelivery.isMapped);
      CHECK_FALSE(fourRes->needsPermutation);
    }

    SECTION("partial FL NA NA admits an unmapped native subset")
    {
      auto const roles =
        std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 3, roles));
      REQUIRE(optNative);
      CHECK(optNative->hasUnsupportedPositions);
      CHECK(optNative->nativeSpeakerMask == 1U);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_FL);
      CHECK(optNative->positions[1] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optNative->positions[2] == SPA_AUDIO_CHANNEL_NA);
      auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 3}, *optNative);
      REQUIRE(deliveryRes);
      CHECK(deliveryRes->clientFormat.sampleRate == 48000U);
      CHECK(deliveryRes->clientFormat.channels == 3U);
      CHECK(deliveryRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(deliveryRes->clientFormat.speakerMask == 0U);
      CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 1U);
      CHECK_FALSE(deliveryRes->channelDelivery.isMapped);
      CHECK_FALSE(deliveryRes->needsPermutation);
    }

    SECTION("FL NA FR NA admits an unmapped FL/FR subset")
    {
      auto const roles = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_NA});
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles));
      REQUIRE(optNative);
      CHECK(optNative->hasUnsupportedPositions);
      CHECK(optNative->nativeSpeakerMask == 0x3U);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_FL);
      CHECK(optNative->positions[1] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optNative->positions[2] == SPA_AUDIO_CHANNEL_FR);
      CHECK(optNative->positions[3] == SPA_AUDIO_CHANNEL_NA);
      auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4}, *optNative);
      REQUIRE(deliveryRes);
      CHECK(deliveryRes->clientFormat.sampleRate == 48000U);
      CHECK(deliveryRes->clientFormat.channels == 4U);
      CHECK(deliveryRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(deliveryRes->clientFormat.speakerMask == 0U);
      CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 0x3U);
      CHECK_FALSE(deliveryRes->channelDelivery.isMapped);
      CHECK_FALSE(deliveryRes->needsPermutation);
    }

    SECTION("known source remains route-incompatible")
    {
      // Observed FL/FR is a subset of the identified source. Silent NA still withholds the route.
      auto const quad = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_NA});
      auto const optQuad = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, quad));
      REQUIRE(optQuad);
      CHECK(optQuad->hasUnsupportedPositions);
      CHECK(optQuad->nativeSpeakerMask == 0x3U);
      auto const quadRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4, .speakerMask = 0x33}, *optQuad);
      REQUIRE_FALSE(quadRes);
      CHECK(quadRes.error().error.code == Error::Code::FormatRejected);
      CHECK(quadRes.error().error.message == "PipeWire client channel positions conflict with the source");
      CHECK(quadRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);

      auto const trio =
        std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optTrio = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 3, trio));
      REQUIRE(optTrio);
      CHECK(optTrio->hasUnsupportedPositions);
      auto const trioRes = rawStreamDelivery({.sampleRate = 48000, .channels = 3, .speakerMask = 0x7}, *optTrio);
      REQUIRE_FALSE(trioRes);
      CHECK(trioRes.error().error.code == Error::Code::FormatRejected);
      CHECK(trioRes.error().error.message == "PipeWire client channel positions conflict with the source");
      CHECK(trioRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }

    SECTION("exact stereo including zero-source NA remains an output failure")
    {
      auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles));
      REQUIRE(optNative);
      CHECK(optNative->hasUnsupportedPositions);
      CHECK(optNative->nativeSpeakerMask == 0U);
      CHECK(optNative->format.speakerMask == 0U);
      CHECK(optNative->positions[0] == SPA_AUDIO_CHANNEL_NA);
      CHECK(optNative->positions[1] == SPA_AUDIO_CHANNEL_NA);
      // These zero-mask and FL/FR sources both offer the exact FL/FR set.
      auto const zeroRes = rawStreamDelivery({.sampleRate = 48000, .channels = 2}, *optNative);
      REQUIRE_FALSE(zeroRes);
      CHECK(zeroRes.error().error.code == Error::Code::FormatRejected);
      CHECK(zeroRes.error().error.message == "PipeWire client channel positions conflict with the source");
      CHECK(zeroRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      auto const exactRes = rawStreamDelivery({.sampleRate = 48000, .channels = 2, .speakerMask = 0x3}, *optNative);
      REQUIRE_FALSE(exactRes);
      CHECK(exactRes.error().error.code == Error::Code::FormatRejected);
      CHECK(exactRes.error().error.message == "PipeWire client channel positions conflict with the source");
      CHECK(exactRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("byte proof leaves literal source bytes unchanged")
    {
      auto const partial3 =
        std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optPartial3 = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 3, partial3));
      REQUIRE(optPartial3);
      auto const partial3Res = rawStreamDelivery({.sampleRate = 48000, .channels = 3}, *optPartial3);
      REQUIRE(partial3Res);
      CHECK_FALSE(partial3Res->needsPermutation);
      auto partial3Bytes = std::to_array<std::byte>(
        {std::byte{0x11}, std::byte{0x12}, std::byte{0x21}, std::byte{0x22}, std::byte{0x31}, std::byte{0x32}});
      applyRawStreamDelivery(partial3Bytes, *partial3Res);
      CHECK(partial3Bytes ==
            std::to_array<std::byte>(
              {std::byte{0x11}, std::byte{0x12}, std::byte{0x21}, std::byte{0x22}, std::byte{0x31}, std::byte{0x32}}));

      auto const partial4 = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_NA});
      auto const optPartial4 = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, partial4));
      REQUIRE(optPartial4);
      auto const partial4Res = rawStreamDelivery({.sampleRate = 48000, .channels = 4}, *optPartial4);
      REQUIRE(partial4Res);
      CHECK_FALSE(partial4Res->needsPermutation);
      auto partial4Bytes = std::to_array<std::byte>({std::byte{0x11},
                                                     std::byte{0x12},
                                                     std::byte{0x21},
                                                     std::byte{0x22},
                                                     std::byte{0x31},
                                                     std::byte{0x32},
                                                     std::byte{0x41},
                                                     std::byte{0x42}});
      applyRawStreamDelivery(partial4Bytes, *partial4Res);
      CHECK(partial4Bytes == std::to_array<std::byte>({std::byte{0x11},
                                                       std::byte{0x12},
                                                       std::byte{0x21},
                                                       std::byte{0x22},
                                                       std::byte{0x31},
                                                       std::byte{0x32},
                                                       std::byte{0x41},
                                                       std::byte{0x42}}));

      auto const allNa3 =
        std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optAllNa3 = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 3, allNa3));
      REQUIRE(optAllNa3);
      auto const allNa3Res = rawStreamDelivery({.sampleRate = 48000, .channels = 3}, *optAllNa3);
      REQUIRE(allNa3Res);
      CHECK_FALSE(allNa3Res->needsPermutation);
      auto allNa3Bytes = std::to_array<std::byte>(
        {std::byte{0x51}, std::byte{0x52}, std::byte{0x61}, std::byte{0x62}, std::byte{0x71}, std::byte{0x72}});
      applyRawStreamDelivery(allNa3Bytes, *allNa3Res);
      CHECK(allNa3Bytes ==
            std::to_array<std::byte>(
              {std::byte{0x51}, std::byte{0x52}, std::byte{0x61}, std::byte{0x62}, std::byte{0x71}, std::byte{0x72}}));

      auto const allNa4 = std::to_array<std::uint32_t>(
        {SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA});
      auto const optAllNa4 = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, allNa4));
      REQUIRE(optAllNa4);
      auto const allNa4Res = rawStreamDelivery({.sampleRate = 48000, .channels = 4}, *optAllNa4);
      REQUIRE(allNa4Res);
      CHECK_FALSE(allNa4Res->needsPermutation);
      auto allNa4Bytes = std::to_array<std::byte>({std::byte{0x81},
                                                   std::byte{0x82},
                                                   std::byte{0x91},
                                                   std::byte{0x92},
                                                   std::byte{0xa1},
                                                   std::byte{0xa2},
                                                   std::byte{0xb1},
                                                   std::byte{0xb2}});
      applyRawStreamDelivery(allNa4Bytes, *allNa4Res);
      CHECK(allNa4Bytes == std::to_array<std::byte>({std::byte{0x81},
                                                     std::byte{0x82},
                                                     std::byte{0x91},
                                                     std::byte{0x92},
                                                     std::byte{0xa1},
                                                     std::byte{0xa2},
                                                     std::byte{0xb1},
                                                     std::byte{0xb2}}));
    }
  }

  TEST_CASE("PipeWireFormatParsing - custom channel ids are unsupported role constraints")
  {
    static_assert(SPA_AUDIO_CHANNEL_START_Custom == 0x10000U);
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles =
      std::to_array<std::uint32_t>({0x10000U, 0x10001U, 0x10002U, 0x10003U, 0x10004U, 0x10005U, 0x10006U, 0x10007U});
    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 8, roles));
    REQUIRE(optNative);
    CHECK(optNative->format.sampleRate == 48000U);
    CHECK(optNative->format.channels == 8U);
    CHECK(optNative->format.encoding == SampleEncoding::Signed16Le);
    CHECK(optNative->format.speakerMask == 0U);
    CHECK(optNative->nativeSpeakerMask == 0U);
    CHECK(optNative->hasUnsupportedPositions);
    CHECK((optNative->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
    CHECK(std::ranges::equal(roles, std::span<std::uint32_t const>{optNative->positions}.first(roles.size())));

    SECTION("zero-source admission leaves interleaved bytes unchanged")
    {
      auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 8, .precisionBits = 16}, *optNative);
      REQUIRE(deliveryRes);
      CHECK(deliveryRes->clientFormat.speakerMask == 0U);
      CHECK(deliveryRes->clientFormat.channels == 8U);
      CHECK(deliveryRes->clientFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(deliveryRes->channelDelivery.nativeSpeakerMask == 0U);
      CHECK_FALSE(deliveryRes->channelDelivery.isMapped);
      CHECK_FALSE(deliveryRes->needsPermutation);
      auto bytes = std::to_array<std::byte>({std::byte{0x10},
                                             std::byte{0x11},
                                             std::byte{0x20},
                                             std::byte{0x21},
                                             std::byte{0x30},
                                             std::byte{0x31},
                                             std::byte{0x40},
                                             std::byte{0x41},
                                             std::byte{0x50},
                                             std::byte{0x51},
                                             std::byte{0x60},
                                             std::byte{0x61},
                                             std::byte{0x70},
                                             std::byte{0x71},
                                             std::byte{0x80},
                                             std::byte{0x81}});
      auto const expected = bytes;
      applyRawStreamDelivery(bytes, *deliveryRes);
      CHECK(bytes == expected);
    }

    SECTION("identified eight-channel mask is route-incompatible")
    {
      // FL FR FC LFE RL RR SL SR: assigned bits 0-5, 9, and 10.
      auto const source = SignalFormat{.sampleRate = 48000, .channels = 8, .precisionBits = 16, .speakerMask = 0x63f};
      auto const deliveryRes = rawStreamDelivery(source, *optNative);
      REQUIRE_FALSE(deliveryRes);
      CHECK(deliveryRes.error().error.code == Error::Code::FormatRejected);
      CHECK(deliveryRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }

    SECTION("an ordinary two-channel offer stays strict")
    {
      auto stereoBuffer = std::array<std::byte, 1024>{};
      auto const stereoRoles = std::to_array<std::uint32_t>({0x10000U, 0x10001U});
      auto const optStereo =
        parseRawStreamObservation(nativeFormat(stereoBuffer, SPA_AUDIO_FORMAT_S16_LE, 2, stereoRoles));
      REQUIRE(optStereo);
      CHECK(optStereo->hasUnsupportedPositions);
      CHECK(optStereo->nativeSpeakerMask == 0U);
      auto const exactRes =
        rawStreamDelivery({.sampleRate = 48000, .channels = 2, .precisionBits = 16, .speakerMask = 0x3}, *optStereo);
      REQUIRE_FALSE(exactRes);
      CHECK(exactRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
      // A zero source mask still offers exact FL/FR for two channels.
      auto const zeroMaskRes = rawStreamDelivery({.sampleRate = 48000, .channels = 2}, *optStereo);
      REQUIRE_FALSE(zeroMaskRes);
      CHECK(zeroMaskRes.error().cause == BackendOpenFailure::Cause::OutputFailure);
    }

    SECTION("mono custom admits a zero source and refuses identified FC")
    {
      auto monoBuffer = std::array<std::byte, 256>{};
      auto const monoRole = std::to_array<std::uint32_t>({0x10000U});
      auto const optMono = parseRawStreamObservation(nativeFormat(monoBuffer, SPA_AUDIO_FORMAT_S16_LE, 1, monoRole));
      REQUIRE(optMono);
      CHECK(optMono->positions[0] == 0x10000U);
      CHECK(optMono->hasUnsupportedPositions);
      CHECK(optMono->nativeSpeakerMask == 0U);
      CHECK(optMono->format.speakerMask == 0U);
      auto const admittedRes = rawStreamDelivery({.sampleRate = 48000, .channels = 1}, *optMono);
      REQUIRE(admittedRes);
      CHECK(admittedRes->clientFormat.speakerMask == 0U);
      CHECK_FALSE(admittedRes->channelDelivery.isMapped);
      CHECK_FALSE(admittedRes->needsPermutation);
      auto bytes = std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}});
      applyRawStreamDelivery(bytes, *admittedRes);
      CHECK(bytes == std::to_array<std::byte>({std::byte{0x11}, std::byte{0x12}}));
      auto const refusedRes = rawStreamDelivery({.sampleRate = 48000, .channels = 1, .speakerMask = 4}, *optMono);
      REQUIRE_FALSE(refusedRes);
      CHECK(refusedRes.error().error.code == Error::Code::FormatRejected);
      CHECK(refusedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
    }
  }

  TEST_CASE("PipeWireFormatParsing - mixed known and custom positions retain assigned role constraints")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, 0x10000U, SPA_AUDIO_CHANNEL_RL, 0x10001U});
    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 4, roles));
    REQUIRE(optNative);
    CHECK(optNative->hasUnsupportedPositions);
    CHECK(optNative->format.speakerMask == 0U);
    CHECK(optNative->nativeSpeakerMask == 0x11U);
    CHECK(std::ranges::equal(roles, std::span<std::uint32_t const>{optNative->positions}.first(roles.size())));
    auto const admittedRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4}, *optNative);
    REQUIRE(admittedRes);
    CHECK(admittedRes->clientFormat.speakerMask == 0U);
    CHECK(admittedRes->channelDelivery.nativeSpeakerMask == 0x11U);
    CHECK_FALSE(admittedRes->channelDelivery.isMapped);
    CHECK_FALSE(admittedRes->needsPermutation);
    auto const refusedRes = rawStreamDelivery({.sampleRate = 48000, .channels = 4, .speakerMask = 0x33}, *optNative);
    REQUIRE_FALSE(refusedRes);
    CHECK(refusedRes.error().cause == BackendOpenFailure::Cause::TrackRouteIncompatible);
  }

  TEST_CASE("PipeWireFormatParsing - zero-source quad custom refresh keeps identity transport unmapped")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const quad = std::to_array<std::uint32_t>(
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
    auto const custom = std::to_array<std::uint32_t>({0x10000U, 0x10001U, 0x10002U, 0x10003U});
    auto const optInitial = parseRawStreamObservation(nativeFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, quad));
    auto const optCurrent = parseRawStreamObservation(nativeFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 4, custom));
    REQUIRE(optInitial);
    REQUIRE(optCurrent);
    CHECK(optInitial->nativeSpeakerMask == 0x33U);
    CHECK(optInitial->format.speakerMask == 0x33U);
    CHECK(optCurrent->hasUnsupportedPositions);
    CHECK(optCurrent->nativeSpeakerMask == 0U);
    CHECK(optCurrent->format.speakerMask == 0U);
    auto const source = SignalFormat{.sampleRate = 48000, .channels = 4};
    auto const initialRes = rawStreamDelivery(source, *optInitial);
    REQUIRE(initialRes);
    CHECK(initialRes->clientFormat.speakerMask == 0U);
    CHECK(initialRes->channelDelivery.nativeSpeakerMask == 0x33U);
    CHECK(initialRes->channelDelivery.isMapped);
    CHECK_FALSE(initialRes->needsPermutation);
    auto const currentRes = rawStreamDelivery(source, *optCurrent, optInitial);
    REQUIRE(currentRes);
    CHECK(currentRes->clientFormat == initialRes->clientFormat);
    CHECK(currentRes->channelDelivery.nativeSpeakerMask == 0U);
    CHECK_FALSE(currentRes->channelDelivery.isMapped);
    CHECK_FALSE(currentRes->needsPermutation);
    auto bytes = std::to_array<std::byte>({std::byte{0x11},
                                           std::byte{0x12},
                                           std::byte{0x21},
                                           std::byte{0x22},
                                           std::byte{0x31},
                                           std::byte{0x32},
                                           std::byte{0x41},
                                           std::byte{0x42}});
    auto const expected = bytes;
    applyRawStreamDelivery(bytes, *currentRes);
    CHECK(bytes == expected);
  }

  TEST_CASE("PipeWireFormatParsing - reserved position gaps and the invalid sentinel remain malformed")
  {
    auto buffer = std::array<std::byte, 1024>{};

    for (auto const position : std::to_array<std::uint32_t>({0x0fffU, 0x2000U, 0xffffU, 0xffffffffU}))
    {
      CAPTURE(position);
      auto const roles = std::to_array<std::uint32_t>({position});
      CHECK_FALSE(parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 1, roles)));
    }

    auto const duplicateCustom = std::to_array<std::uint32_t>({0x10000U, 0x10000U});
    CHECK_FALSE(parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, duplicateCustom)));
  }

  TEST_CASE("PipeWireFormatParsing - ordinary mask-zero stereo retains its offer without proving source roles")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto const optNative = parseRawStreamObservation(nativeFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 2, roles));
    REQUIRE(optNative);
    auto const deliveryRes = rawStreamDelivery({.sampleRate = 48000, .channels = 2}, *optNative);
    REQUIRE(deliveryRes);
    CHECK(deliveryRes->clientFormat.speakerMask == 0U);
    CHECK(optNative->format.speakerMask == 3U);
    // Complete native FL/FR is mapped evidence; the zero client mask is not source identity.
    CHECK(deliveryRes->channelDelivery.isMapped);
  }

  TEST_CASE("PipeWireFormatParsing - a short native position array cannot describe a complete stream")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto builder = ::spa_pod_builder{};
    ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
    auto frame = ::spa_pod_frame{};
    ::spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
    ::spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_format, 0);
    ::spa_pod_builder_id(&builder, SPA_AUDIO_FORMAT_S16_LE);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_rate, 0);
    ::spa_pod_builder_int(&builder, 48000);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_channels, 0);
    ::spa_pod_builder_int(&builder, 2);
    ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL});
    ::spa_pod_builder_array(&builder, sizeof(std::uint32_t), SPA_TYPE_Id, roles.size(), roles.data());
    auto const* pod = static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &frame));
    REQUIRE(pod != nullptr);
    CHECK_FALSE(parseRawStreamObservation(pod));
  }

  TEST_CASE("PipeWireFormatParsing - invalid geometry layout and empty encoding cannot build an offer")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto const encodings = std::to_array({SampleEncoding::Signed16Le});
    CHECK(buildRawStreamFormatOffer(buffer, {.sampleRate = 0, .channels = 2}, encodings) == nullptr);
    CHECK(buildRawStreamFormatOffer(buffer, {.sampleRate = 48000, .channels = 0}, encodings) == nullptr);
    CHECK(buildRawStreamFormatOffer(buffer, {.sampleRate = 48000, .channels = 2}, {}) == nullptr);
    CHECK(buildRawStreamFormatOffer(buffer, {.sampleRate = 48000, .channels = 2, .speakerMask = 1}, encodings) ==
          nullptr);
    CHECK(buildRawStreamFormatOffer(
            buffer, {.sampleRate = 48000, .channels = 2, .speakerMask = 0x80000001}, encodings) == nullptr);
  }
} // namespace ao::audio::backend::detail::test
