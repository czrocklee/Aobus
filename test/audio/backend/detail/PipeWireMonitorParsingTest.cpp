// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/backend/detail/PipeWireMonitorParsing.h"

#include "lib/audio/backend/detail/PipeWireFormatParsing.h"
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/body.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#endif

namespace ao::audio::backend::detail::test
{
  namespace
  {
    // Author the pod directly: oversized positions must never pass through
    // spa_audio_info_raw's fixed-capacity array, even in the fixture.
    ::spa_pod const* monitorFormat(std::span<std::byte> buffer,
                                   std::uint32_t encoding,
                                   std::int32_t rate,
                                   std::int32_t channels,
                                   std::span<std::uint32_t const> positions)
    {
      auto builder = ::spa_pod_builder{};
      ::spa_pod_builder_init(&builder, buffer.data(), buffer.size());
      auto object = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
      ::spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
      ::spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_format, 0);
      ::spa_pod_builder_id(&builder, encoding);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_rate, 0);
      ::spa_pod_builder_int(&builder, rate);
      ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_channels, 0);
      ::spa_pod_builder_int(&builder, channels);

      if (!positions.empty())
      {
        ::spa_pod_builder_prop(&builder, SPA_FORMAT_AUDIO_position, 0);
        ::spa_pod_builder_array(&builder, sizeof(std::uint32_t), SPA_TYPE_Id, positions.size(), positions.data());
      }

      return static_cast<::spa_pod const*>(::spa_pod_builder_pop(&builder, &object));
    }

    ::spa_dict makeDict(std::span<::spa_dict_item const> items)
    {
      return ::spa_dict{.flags = 0, .n_items = static_cast<std::uint32_t>(items.size()), .items = items.data()};
    }

    ::spa_pod_builder makePodBuilder(std::span<std::byte> buffer)
    {
      return ::spa_pod_builder{.data = buffer.data(),
                               .size = static_cast<std::uint32_t>(buffer.size()),
                               ._padding = 0,
                               .state = {.offset = 0, .flags = 0, .frame = nullptr},
                               .callbacks = {.funcs = nullptr, .data = nullptr}};
    }
  } // namespace

  TEST_CASE("PipeWireMonitorParsing - sink media classes")
  {
    CHECK(isSinkMediaClass("Audio/Sink"));
    CHECK(isSinkMediaClass("Audio/Duplex"));
    CHECK(isSinkMediaClass("Stream/Output/Audio/Sink"));
    CHECK_FALSE(isSinkMediaClass("Audio/Source"));
    CHECK_FALSE(isSinkMediaClass("Video/Sink"));
  }

  TEST_CASE("PipeWireMonitorParsing - dictionary property lookup")
  {
    auto const items = std::to_array<::spa_dict_item>(
      {::spa_dict_item{.key = "key1", .value = "val1"}, ::spa_dict_item{.key = "key2", .value = "val2"}});
    auto const dict = makeDict(items);

    CHECK(lookupProperty(&dict, "key1") == "val1");
    CHECK(lookupProperty(&dict, "key2") == "val2");
    CHECK(lookupProperty(&dict, "key3").empty());
    CHECK(lookupProperty(nullptr, "key1").empty());
  }

  TEST_CASE("PipeWireMonitorParsing - strict unsigned integer properties")
  {
    CHECK(parsePipeWireUint32(nullptr) == std::nullopt);
    CHECK(parsePipeWireUint32("") == std::nullopt);
    CHECK(parsePipeWireUint32("abc") == std::nullopt);
    CHECK(parsePipeWireUint32("12abc") == std::nullopt);
    CHECK(parsePipeWireUint32("abc12") == std::nullopt);
    CHECK(parsePipeWireUint32(" 12") == std::nullopt);
    CHECK(parsePipeWireUint32("4294967296") == std::nullopt);

    CHECK(parsePipeWireUint32("42") == 42);
    CHECK(parsePipeWireUint32("0") == 0);
    CHECK(parsePipeWireUint32("4294967295") == 4294967295U);
  }

  TEST_CASE("PipeWireMonitorParsing - node record fields")
  {
    auto const items =
      std::to_array<::spa_dict_item>({::spa_dict_item{.key = PW_KEY_MEDIA_CLASS, .value = "Audio/Sink"},
                                      ::spa_dict_item{.key = PW_KEY_NODE_NAME, .value = "test-node"},
                                      ::spa_dict_item{.key = PW_KEY_OBJECT_SERIAL, .value = "1234"},
                                      ::spa_dict_item{.key = "node.driver-id", .value = "5678"}});
    auto const dict = makeDict(items);

    auto record = parseNodeRecord(1, &dict);
    CHECK(record.version == 1);
    CHECK(record.mediaClass == "Audio/Sink");
    CHECK(record.nodeName == "test-node");
    CHECK(record.optObjectSerial == 1234);
    CHECK(record.optDriverId == 5678);
  }

  TEST_CASE("SinkProps::classifyVolume - zero channel gain remains the range minimum regardless of order")
  {
    for (auto const& channelVolumes : {std::vector<float>{0.0F, 0.8F}, std::vector<float>{0.8F, 0.0F}})
    {
      auto props = SinkProps{};
      props.hasChannelVolumes = true;
      props.channelVolumes = channelVolumes;

      auto const sinkCls = props.classifyVolume();
      CHECK(sinkCls.unclassifiedNotUnity == true);
      CHECK(sinkCls.maxUnclassifiedGain == 0.8F);
      CHECK(sinkCls.minUnclassifiedGain == 0.0F);

      auto const streamCls = props.classifyVolume(SinkProps::VolumeClassificationScope::Stream);
      CHECK(streamCls.softwareNotUnity == true);
      CHECK(streamCls.maxSoftwareGain == 0.8F);
      CHECK(streamCls.minSoftwareGain == 0.0F);
    }
  }

  TEST_CASE("PipeWireMonitorParsing - preferred offer encoding is also a valid enum alternative")
  {
    auto offerBuffer = std::array<std::byte, 1024>{};
    auto const encodings =
      std::to_array({SampleEncoding::Signed24PackedLe, SampleEncoding::Signed24In32Le, SampleEncoding::Signed32Le});
    auto const* pod = buildRawStreamFormatOffer(
      offerBuffer, SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24}, encodings);
    REQUIRE(pod != nullptr);

    auto const* property = ::spa_pod_find_prop(pod, nullptr, SPA_FORMAT_AUDIO_format);
    REQUIRE(property != nullptr);
    REQUIRE(::spa_pod_is_choice(&property->value) != 0);

    auto const valueBytes =
      utility::bytes::view(static_cast<void const*>(&property->value), property->value.size + sizeof(::spa_pod));
    auto const* choice = utility::layout::view<::spa_pod_choice>(valueBytes);
    REQUIRE(SPA_POD_CHOICE_VALUE_TYPE(choice) == SPA_TYPE_Id);
    REQUIRE(SPA_POD_CHOICE_N_VALUES(choice) == encodings.size() + 1U);

    auto const* values = static_cast<std::uint32_t const*>(SPA_POD_CHOICE_VALUES(choice));
    CHECK(values[0] == SPA_AUDIO_FORMAT_S24_LE);
    CHECK(values[1] == SPA_AUDIO_FORMAT_S24_LE);
    CHECK(values[2] == SPA_AUDIO_FORMAT_S24_32_LE);
    CHECK(values[3] == SPA_AUDIO_FORMAT_S32_LE);
  }

  TEST_CASE("PipeWireMonitorParsing - only current-format parameters describe the opened format")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);

    SECTION("currentFormatFromNodeParam - EnumFormat is not current format")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(44100),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(2),
                            0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto const optNative = parseRawStreamObservation(pod);
      REQUIRE(optNative);
      CHECK(optNative->format.sampleRate == 44100);
      CHECK(optNative->format.channels == 2);
      CHECK(optNative->format.encoding == SampleEncoding::Signed16Le);
      CHECK_FALSE(currentFormatFromNodeParam(SPA_PARAM_EnumFormat, pod));
    }

    SECTION("currentFormatFromNodeParam - Format is current format")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(44100),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(2),
                            0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto const optFormat = currentFormatFromNodeParam(SPA_PARAM_Format, pod);
      REQUIRE(optFormat);
      CHECK(optFormat->format.sampleRate == 44100);
      CHECK(optFormat->format.channels == 2);
      CHECK(optFormat->format.encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("PipeWireMonitorParsing - rejects an unsupported negotiated encoding")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);
    auto f = ::spa_pod_frame{};
    ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
    ::spa_pod_builder_add(&b,
                          SPA_FORMAT_mediaType,
                          SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                          SPA_FORMAT_mediaSubtype,
                          SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_AUDIO_format,
                          SPA_POD_Id(SPA_AUDIO_FORMAT_U8),
                          SPA_FORMAT_AUDIO_rate,
                          SPA_POD_Int(48000),
                          SPA_FORMAT_AUDIO_channels,
                          SPA_POD_Int(2),
                          0);
    auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

    CHECK_FALSE(parseRawStreamObservation(pod));
  }

  TEST_CASE(
    "PipeWireMonitorParsing - rejects raw formats whose rate or channel count cannot narrow to the client format")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);

    SECTION("a zero sample rate is not a usable client format")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(0),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(2),
                            0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      CHECK_FALSE(parseRawStreamObservation(pod));
    }

    SECTION("a zero channel count is not a usable client format")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(48000),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(0),
                            0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      CHECK_FALSE(parseRawStreamObservation(pod));
    }

    SECTION("a 258-channel stream cannot wrap into stereo")
    {
      // Narrowing 258 to std::uint8_t would wrap to 2, silently turning an
      // exotic layout into a stereo client format; the bounds check refuses it.
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(48000),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(258),
                            0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      CHECK_FALSE(parseRawStreamObservation(pod));
    }
  }

  TEST_CASE("PipeWireMonitorParsing - failed current format clears the stale cache")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);
    auto f = ::spa_pod_frame{};
    ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
    ::spa_pod_builder_add(&b,
                          SPA_FORMAT_mediaType,
                          SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                          SPA_FORMAT_mediaSubtype,
                          SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_AUDIO_format,
                          SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                          SPA_FORMAT_AUDIO_rate,
                          SPA_POD_Int(44100),
                          SPA_FORMAT_AUDIO_channels,
                          SPA_POD_Int(2),
                          0);
    auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
    REQUIRE(cache.contains(42));

    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, nullptr);
    CHECK_FALSE(cache.contains(42));
  }

  TEST_CASE("PipeWireMonitorParsing - current observations replace position evidence without copying the request")
  {
    struct PositionCase final
    {
      std::uint32_t left;
      std::uint32_t right;
      std::uint32_t flags;
      std::uint32_t nativeMask;
      std::uint32_t canonicalMask;
    };
    // Independent actual native orders: FR FL, BL BR, and unpositioned.
    auto const cases = std::to_array<PositionCase>({
      {SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL, 0, 0x3, 0},
      {SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, 0, 0x30, 0x30},
      {SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN, SPA_AUDIO_FLAG_UNPOSITIONED, 0, 0},
    });
    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};

    for (auto const& expected : cases)
    {
      auto buffer = std::array<std::byte, 1024>{};
      auto builder = makePodBuilder(buffer);
      auto native = ::spa_audio_info_raw{};
      native.format = SPA_AUDIO_FORMAT_S16_LE;
      native.flags = expected.flags;
      native.rate = 48000;
      native.channels = 2;
      native.position[0] = expected.left;
      native.position[1] = expected.right;
      auto const* pod = ::spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &native);
      REQUIRE(pod != nullptr);
      updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, pod);
      REQUIRE(cache.contains(42));
      auto const& observed = cache.at(42);
      CHECK(observed.format.sampleRate == 48000);
      CHECK(observed.format.channels == 2);
      CHECK(observed.format.encoding == SampleEncoding::Signed16Le);
      CHECK(observed.format.speakerMask == expected.canonicalMask);
      CHECK(observed.nativeSpeakerMask == expected.nativeMask);
      CHECK((observed.flags & SPA_AUDIO_FLAG_UNPOSITIONED) == expected.flags);

      if (expected.nativeMask != 0)
      {
        CHECK(observed.positions[0] == expected.left);
        CHECK(observed.positions[1] == expected.right);
      }

      auto const saved = observed;
      updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_EnumFormat, nullptr);
      CHECK(cache.at(42) == saved);
    }

    // The final current observation removed the previous known BL/BR roles;
    // null readback then removes even the storage observation.
    CHECK(cache.at(42).nativeSpeakerMask == 0);
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, nullptr);
    CHECK_FALSE(cache.contains(42));
  }

  TEST_CASE("PipeWireMonitorParsing - unusable position semantics preserve fresh numerics and known constraints")
  {
    auto const cases = std::to_array<std::array<std::uint32_t, 3>>({
      {SPA_AUDIO_CHANNEL_FL, 0xffffffffU, SPA_AUDIO_CHANNEL_UNKNOWN},
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_NA, SPA_AUDIO_CHANNEL_NA},
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_MONO, SPA_AUDIO_CHANNEL_MONO},
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_AUX0, SPA_AUDIO_CHANNEL_AUX0},
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_UNKNOWN},
    });
    auto const initialRoles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});

    for (auto const& roles : cases)
    {
      auto initialBuffer = std::array<std::byte, 1024>{};
      auto currentBuffer = std::array<std::byte, 1024>{};
      auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
      auto const* initial = monitorFormat(initialBuffer, SPA_AUDIO_FORMAT_S32_LE, 44100, 2, initialRoles);
      REQUIRE(initial != nullptr);
      updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, initial);
      REQUIRE(cache.contains(42));
      REQUIRE(cache.at(42).nativeSpeakerMask == 0x30U);
      auto const* current = monitorFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 48000, 3, roles);
      REQUIRE(current != nullptr);
      // Repeated silent NA is a strict-valid unsupported constraint. Duplicate
      // FL, MONO, AUX, and the invalid sentinel remain malformed readback.
      bool const repeatedSilentNa =
        roles[0] == SPA_AUDIO_CHANNEL_FL && roles[1] == SPA_AUDIO_CHANNEL_NA && roles[2] == SPA_AUDIO_CHANNEL_NA;

      if (repeatedSilentNa)
      {
        auto const optStrict = parseRawStreamObservation(current);
        REQUIRE(optStrict);
        CHECK(optStrict->hasUnsupportedPositions);
        CHECK(optStrict->format.sampleRate == 48000U);
        CHECK(optStrict->format.channels == 3U);
        CHECK(optStrict->format.encoding == SampleEncoding::Signed16Le);
        CHECK(optStrict->format.speakerMask == 0U);
        CHECK(optStrict->nativeSpeakerMask == 1U);
        CHECK(optStrict->positions[0] == SPA_AUDIO_CHANNEL_FL);
        CHECK(optStrict->positions[1] == SPA_AUDIO_CHANNEL_NA);
        CHECK(optStrict->positions[2] == SPA_AUDIO_CHANNEL_NA);
        CHECK((optStrict->flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
      }
      else
      {
        CHECK_FALSE(parseRawStreamObservation(current));
      }

      updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_EnumFormat, current);
      CHECK(cache.at(42).nativeSpeakerMask == 0x30U);
      updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, current);
      REQUIRE(cache.contains(42));
      auto const& observed = cache.at(42);
      CHECK(observed.format.sampleRate == 48000U);
      CHECK(observed.format.channels == 3U);
      CHECK(observed.format.encoding == SampleEncoding::Signed16Le);
      CHECK(observed.format.speakerMask == 0U);
      CHECK(observed.nativeSpeakerMask == 1U);
      CHECK(observed.hasUnsupportedPositions);
      CHECK(observed.positions[0] == roles[0]);
      CHECK(observed.positions[1] == roles[1]);
      CHECK(observed.positions[2] == roles[2]);
      CHECK(observed.positions[3] == SPA_AUDIO_CHANNEL_UNKNOWN);
      CHECK((observed.flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0U);
    }
  }

  TEST_CASE("PipeWireMonitorParsing - oversized position storage preserves numerics without a native array copy")
  {
    auto buffer = std::array<std::byte, 2048>{};
    auto roles = std::array<std::uint32_t, 65>{};
    roles.fill(SPA_AUDIO_CHANNEL_UNKNOWN);
    roles.front() = SPA_AUDIO_CHANNEL_FL;
    roles.back() = SPA_AUDIO_CHANNEL_FC;
    std::int32_t channels = 0;
    std::uint32_t expectedMask = 0;
    std::uint32_t expectedFirst = SPA_AUDIO_CHANNEL_UNKNOWN;
    std::uint32_t expectedFlags = SPA_AUDIO_FLAG_UNPOSITIONED;

    SECTION("all 65 slots are active and the last known role remains evidence")
    {
      channels = 65;
      expectedMask = 5;
      expectedFirst = SPA_AUDIO_CHANNEL_FL;
      expectedFlags = 0;
    }

    SECTION("excess position slots cannot overwrite count or establish a prefix layout")
    {
      channels = 2;
    }

    auto const* pod = monitorFormat(buffer, SPA_AUDIO_FORMAT_S16_LE, 48000, channels, roles);
    REQUIRE(pod != nullptr);
    CHECK_FALSE(parseRawStreamObservation(pod));
    auto const optMonitor = currentFormatFromNodeParam(SPA_PARAM_Format, pod);
    REQUIRE(optMonitor);
    CHECK(optMonitor->format.sampleRate == 48000U);
    CHECK(std::cmp_equal(optMonitor->format.channels, channels));
    CHECK(optMonitor->format.encoding == SampleEncoding::Signed16Le);
    CHECK(optMonitor->format.speakerMask == 0U);
    CHECK(optMonitor->nativeSpeakerMask == expectedMask);
    CHECK(optMonitor->hasUnsupportedPositions);
    CHECK(optMonitor->flags == expectedFlags);
    CHECK(optMonitor->positions.front() == expectedFirst);
    CHECK(optMonitor->positions.back() == SPA_AUDIO_CHANNEL_UNKNOWN);
  }

  TEST_CASE("PipeWireMonitorParsing - mismatched position cardinality keeps numerics without prefix role claims")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const initialRoles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR});
    auto roles = std::vector<std::uint32_t>{SPA_AUDIO_CHANNEL_FC};
    auto const* initial = monitorFormat(initialBuffer, SPA_AUDIO_FORMAT_S32_LE, 44100, 2, initialRoles);
    REQUIRE(initial != nullptr);
    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, initial);
    REQUIRE(cache.contains(42));
    REQUIRE(cache.at(42).nativeSpeakerMask == 0x30U);
    bool hasFixedChoice = false;

    SECTION("short direct array")
    {
      hasFixedChoice = false;
    }

    SECTION("short fixed ChoiceNone array")
    {
      hasFixedChoice = true;
    }

    SECTION("excess entries do not establish an active prefix")
    {
      roles = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_FR};
    }

    auto const* current = monitorFormat(currentBuffer, SPA_AUDIO_FORMAT_S16_LE, 48000, 2, roles);
    REQUIRE(current != nullptr);

    if (hasFixedChoice)
    {
      auto const* property = ::spa_pod_find_prop(current, nullptr, SPA_FORMAT_AUDIO_position);
      REQUIRE(property != nullptr);
      REQUIRE(property->value.size == 12U);
      auto const offset =
        static_cast<std::size_t>(std::as_bytes(std::span{&property->value, 1}).data() - currentBuffer.data());
      // A complete one-slot Choice -> Array body plus its four padding bytes.
      // This final property grows by 16 bytes without truncating the object.
      auto const choice = std::to_array<std::uint32_t>(
        {28U, SPA_TYPE_Choice, SPA_CHOICE_None, 0U, 12U, SPA_TYPE_Array, 4U, SPA_TYPE_Id, SPA_AUDIO_CHANNEL_FC, 0U});
      std::memcpy(currentBuffer.data() + offset, choice.data(), sizeof(choice));
      auto header = *current;
      header.size += 16U;
      std::memcpy(currentBuffer.data(), &header, sizeof(header));
    }

    // The native parser unpositions these two-channel count mismatches. Their
    // FC-bearing prefix must not invent a conflict with stereo.
    CHECK_FALSE(parseRawStreamObservation(current));
    auto const optMonitor = currentFormatFromNodeParam(SPA_PARAM_Format, current);
    REQUIRE(optMonitor);
    CHECK(optMonitor->format.sampleRate == 48000U);
    CHECK(optMonitor->format.channels == 2U);
    CHECK(optMonitor->format.encoding == SampleEncoding::Signed16Le);
    CHECK(optMonitor->format.speakerMask == 0U);
    CHECK(optMonitor->nativeSpeakerMask == 0U);
    CHECK(optMonitor->hasUnsupportedPositions);
    CHECK(optMonitor->flags == SPA_AUDIO_FLAG_UNPOSITIONED);
    CHECK(optMonitor->positions[0] == SPA_AUDIO_CHANNEL_UNKNOWN);
    CHECK(optMonitor->positions[1] == SPA_AUDIO_CHANNEL_UNKNOWN);
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, current);
    REQUIRE(cache.contains(42));
    CHECK(cache.at(42).format.sampleRate == 48000U);
    CHECK(cache.at(42).format.channels == 2U);
    CHECK(cache.at(42).format.encoding == SampleEncoding::Signed16Le);
    CHECK(cache.at(42).format.speakerMask == 0U);
    CHECK(cache.at(42).nativeSpeakerMask == 0U);
    CHECK(cache.at(42).flags == SPA_AUDIO_FLAG_UNPOSITIONED);
    CHECK(cache.at(42).positions[0] == SPA_AUDIO_CHANNEL_UNKNOWN);
    CHECK(cache.at(42).positions[1] == SPA_AUDIO_CHANNEL_UNKNOWN);
  }

  TEST_CASE("PipeWireMonitorParsing - invalid current geometry or pod storage invalidates all cached facts")
  {
    auto initialBuffer = std::array<std::byte, 1024>{};
    auto currentBuffer = std::array<std::byte, 1024>{};
    auto const roles = std::to_array<std::uint32_t>({SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    auto const* initial = monitorFormat(initialBuffer, SPA_AUDIO_FORMAT_S16_LE, 48000, 2, roles);
    REQUIRE(initial != nullptr);
    auto cache = std::unordered_map<std::uint32_t, RawStreamFormat>{};
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, initial);
    REQUIRE(cache.contains(42));
    REQUIRE(cache.at(42).nativeSpeakerMask == 3U);
    std::uint32_t encoding = SPA_AUDIO_FORMAT_S16_LE;
    std::int32_t rate = 48000;
    std::int32_t channels = 2;
    bool truncateObject = false;
    bool overrunProperty = false;
    bool malformedArray = false;

    SECTION("zero rate")
    {
      rate = 0;
    }

    SECTION("negative rate")
    {
      rate = -1;
    }

    SECTION("zero channel count")
    {
      channels = 0;
    }

    SECTION("channel count exceeds the independent format representation")
    {
      channels = 258;
    }

    SECTION("unsupported encoding")
    {
      encoding = SPA_AUDIO_FORMAT_U8;
    }

    SECTION("truncated whole object storage")
    {
      truncateObject = true;
    }

    SECTION("property payload extends beyond the enclosing object")
    {
      overrunProperty = true;
    }

    SECTION("position array storage has a wrong child width")
    {
      malformedArray = true;
    }

    auto const* current = monitorFormat(currentBuffer, encoding, rate, channels, roles);
    REQUIRE(current != nullptr);

    if (truncateObject)
    {
      auto header = *current;
      --header.size;
      std::memcpy(currentBuffer.data(), &header, sizeof(header));
    }

    if (overrunProperty || malformedArray)
    {
      auto const* property = ::spa_pod_find_prop(current, nullptr, SPA_FORMAT_AUDIO_position);
      REQUIRE(property != nullptr);
      auto const offset =
        static_cast<std::size_t>(std::as_bytes(std::span{&property->value, 1}).data() - currentBuffer.data());

      if (overrunProperty)
      {
        auto value = property->value;
        value.size = current->size;
        std::memcpy(currentBuffer.data() + offset, &value, sizeof(value));
      }
      else
      {
        auto body = ::spa_pod_array_body{};
        std::memcpy(&body, SPA_POD_BODY_CONST(&property->value), sizeof(body));
        body.child.size = 8;
        std::memcpy(currentBuffer.data() + offset + sizeof(::spa_pod), &body, sizeof(body));
      }
    }

    CHECK_FALSE(parseRawStreamObservation(current));
    CHECK_FALSE(currentFormatFromNodeParam(SPA_PARAM_Format, current));
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_Format, current);
    CHECK_FALSE(cache.contains(42));
    updateCurrentFormatFromNodeParam(cache, 42, SPA_PARAM_EnumFormat, initial);
    CHECK_FALSE(cache.contains(42));
  }

  TEST_CASE("PipeWireMonitorParsing - merges sink property updates")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);

    SECTION("mergeSinkProps - volume and mute")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_add(&b, SPA_PROP_volume, SPA_POD_Float(0.5F), SPA_PROP_mute, SPA_POD_Bool(true), 0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);
      CHECK(std::abs(props.volume - 0.5F) < 1e-4F);
      CHECK(props.isMuted == true);
      CHECK(props.classifyVolume().unclassifiedNotUnity == true);
    }

    SECTION("mergeSinkProps - channel volumes")
    {
      auto const vols = std::to_array<float>({1.0F, 0.8F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);
      REQUIRE(props.channelVolumes.size() == 2);
      CHECK(props.channelVolumes[0] == 1.0F);
      CHECK(props.channelVolumes[1] == 0.8F);
      CHECK(props.classifyVolume().unclassifiedNotUnity == true);
    }

    SECTION("mergeSinkProps - preserves channel volumes beyond sixteen channels")
    {
      auto vols = std::array<float, 20>{};
      vols.fill(1.0F);
      vols.back() = 0.5F;
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      REQUIRE(props.channelVolumes.size() == vols.size());
      CHECK(props.channelVolumes.back() == 0.5F);
      CHECK(props.classifyVolume().unclassifiedNotUnity == true);
    }

    SECTION("mergeSinkProps - preserves hardware flags across unflagged updates")
    {
      // First event has hardware flag
      auto f1 = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f1, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      auto const vols1 = std::to_array<float>({0.5F, 0.5F});
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols1.size(), utility::layout::asLegacyPtr<float>(vols1.data()));
      auto* pod1 = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f1));

      auto props = SinkProps{};
      mergeSinkProps(props, pod1);
      CHECK(props.channelVolumesAreHardware == true);

      // Second event updates volume without repeating hardware flag (e.g. just raw param update)
      auto f2 = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f2, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      auto const vols2 = std::to_array<float>({0.25F, 0.25F});
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols2.size(), utility::layout::asLegacyPtr<float>(vols2.data()));
      auto* pod2 = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f2));

      mergeSinkProps(props, pod2);

      // Hardware flag should be preserved
      CHECK(props.channelVolumesAreHardware == true);
      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
    }
  }

  TEST_CASE("PipeWireMonitorParsing - classifies hardware software and ambiguous volume evidence")
  {
    auto buffer = std::array<std::byte, 1024>{};
    auto b = makePodBuilder(buffer);

    SECTION("SinkProps::classifyVolume - Hardware-only")
    {
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto const softVols = std::to_array<float>({1.0F, 1.0F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Mixed Scalar Hardware / Channel Unclassified")
    {
      // Deliberately distinct magnitudes so a hardware value leaking into the unclassified range
      // cannot hide behind an equal channel value.
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

      // Scalar volume with hardware flag
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_float(&b, 0.25F);

      // Channel volume without hardware flag
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 0.5F);
      CHECK(cls.minUnclassifiedGain == 0.5F);

      auto const streamCls = props.classifyVolume(SinkProps::VolumeClassificationScope::Stream);
      CHECK(streamCls.hardwareNotUnity == true);
      CHECK(streamCls.softwareNotUnity == true);
      CHECK(streamCls.maxSoftwareGain == 0.5F);
      CHECK(streamCls.minSoftwareGain == 0.5F);
      CHECK(streamCls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Mixed Channel Hardware / Scalar Unclassified")
    {
      auto const vols = std::to_array<float>({0.25F, 0.25F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

      ::spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
      ::spa_pod_builder_float(&b, 0.5F);

      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 0.5F);
      CHECK(cls.minUnclassifiedGain == 0.5F);

      auto const streamCls = props.classifyVolume(SinkProps::VolumeClassificationScope::Stream);
      CHECK(streamCls.hardwareNotUnity == true);
      CHECK(streamCls.softwareNotUnity == true);
      CHECK(streamCls.maxSoftwareGain == 0.5F);
      CHECK(streamCls.minSoftwareGain == 0.5F);
      CHECK(streamCls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Channel-only ambiguous attenuation records every channel")
    {
      auto const vols = std::to_array<float>({1.0F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 1.0F);
      CHECK(cls.minUnclassifiedGain == 0.5F);
      CHECK(cls.maxSoftwareGain == 0.0F);
      CHECK(cls.minSoftwareGain == 0.0F);
    }

    SECTION("SinkProps::classifyVolume - Scalar-only ambiguous attenuation")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
      ::spa_pod_builder_float(&b, 0.25F);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 0.25F);
      CHECK(cls.minUnclassifiedGain == 0.25F);
    }

    SECTION("SinkProps::classifyVolume - Scalar and channel both ambiguous contribute")
    {
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
      ::spa_pod_builder_float(&b, 0.25F);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 0.5F);
      CHECK(cls.minUnclassifiedGain == 0.25F);

      auto const streamCls = props.classifyVolume(SinkProps::VolumeClassificationScope::Stream);
      CHECK(streamCls.softwareNotUnity == true);
      CHECK(streamCls.maxSoftwareGain == 0.5F);
      CHECK(streamCls.minSoftwareGain == 0.25F);
      CHECK(streamCls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - All non-unity evidence hardware-flagged carries no range")
    {
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_float(&b, 0.25F);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      for (auto const scope :
           {SinkProps::VolumeClassificationScope::Sink, SinkProps::VolumeClassificationScope::Stream})
      {
        auto const cls = props.classifyVolume(scope);
        CHECK(cls.hardwareNotUnity == true);
        CHECK(cls.softwareNotUnity == false);
        CHECK(cls.unclassifiedNotUnity == false);
        CHECK(cls.maxSoftwareGain == 0.0F);
        CHECK(cls.minSoftwareGain == 0.0F);
        CHECK(cls.maxUnclassifiedGain == 0.0F);
        CHECK(cls.minUnclassifiedGain == 0.0F);
      }
    }

    SECTION("SinkProps::classifyVolume - Ambiguous amplification keeps the above-unity factor")
    {
      auto const vols = std::to_array<float>({0.5F, 1.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.unclassifiedNotUnity == true);
      CHECK(cls.maxUnclassifiedGain == 1.5F);
      CHECK(cls.minUnclassifiedGain == 0.5F);
    }

    SECTION("SinkProps::classifyVolume - Soft volumes keep ambiguous evidence out of the unclassified range")
    {
      auto const vols = std::to_array<float>({0.25F, 0.25F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto const softVols = std::to_array<float>({0.5F, 0.5F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.softwareNotUnity == true);
      CHECK(cls.maxSoftwareGain == 0.5F);
      CHECK(cls.minSoftwareGain == 0.5F);
      CHECK(cls.unclassifiedNotUnity == false);
      CHECK(cls.maxUnclassifiedGain == 0.0F);
      CHECK(cls.minUnclassifiedGain == 0.0F);
    }

    SECTION("SinkProps::classifyVolume - Software-only")
    {
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0); // No hardware flag
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto const softVols = std::to_array<float>({0.5F, 0.5F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == true);
      CHECK(cls.maxSoftwareGain == 0.5F);
      CHECK(cls.minSoftwareGain == 0.5F);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Software amplification")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

      auto const softVols = std::to_array<float>({1.0F, 1.25F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == true);
      CHECK(cls.maxSoftwareGain == 1.25F);
      CHECK(cls.minSoftwareGain == 1.0F);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Mixed")
    {
      auto const vols = std::to_array<float>({0.25F, 0.25F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto const softVols = std::to_array<float>({0.5F, 0.5F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
      CHECK(cls.softwareNotUnity == true);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Ambiguous")
    {
      auto const vols = std::to_array<float>({0.5F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0); // No hardware flag
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      // No softVolumes
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == true);
    }

    SECTION("SinkProps::classifyVolume - stream ambiguous volume is software")
    {
      auto const vols = std::to_array<float>({1.0F, 0.5F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume(SinkProps::VolumeClassificationScope::Stream);
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == true);
      CHECK(cls.maxSoftwareGain == 1.0F);
      CHECK(cls.minSoftwareGain == 0.5F);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Hardware-capable unity")
    {
      auto const vols = std::to_array<float>({1.0F, 1.0F});
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), utility::layout::asLegacyPtr<float>(vols.data()));

      auto const softVols = std::to_array<float>({1.0F, 1.0F});
      ::spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, softVols.size(), utility::layout::asLegacyPtr<float>(softVols.data()));
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Scalar hardware volume")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, SPA_POD_PROP_FLAG_HARDWARE);
      ::spa_pod_builder_float(&b, 0.5F);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == true);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == false);
    }

    SECTION("SinkProps::classifyVolume - Scalar ambiguous volume")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
      ::spa_pod_builder_float(&b, 0.5F);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);

      auto const cls = props.classifyVolume();
      CHECK(cls.hardwareNotUnity == false);
      CHECK(cls.softwareNotUnity == false);
      CHECK(cls.unclassifiedNotUnity == true);
    }

    SECTION("SinkProps::classifyVolume - Unity tolerance")
    {
      auto props = SinkProps{};
      props.hasVolume = true;
      props.volumeIsHardware = true;

      props.volume = 0.99999F; // Within tolerance
      auto const cls1 = props.classifyVolume();
      CHECK(cls1.hardwareNotUnity == false);

      props.volume = 0.999F; // Outside tolerance
      auto const cls2 = props.classifyVolume();
      CHECK(cls2.hardwareNotUnity == true);
    }
  }

#ifdef __clang__
#pragma clang diagnostic pop
#endif
} // namespace ao::audio::backend::detail::test
