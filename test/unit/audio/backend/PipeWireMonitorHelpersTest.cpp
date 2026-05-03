// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/PipeWireMonitorHelpers.h>

#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
}

#include <algorithm>

using namespace ao::audio;
using namespace ao::audio::backend::detail;

TEST_CASE("PipeWireMonitorHelpers - Property Parsing", "[audio][pipewire][monitor]")
{
  SECTION("isSinkMediaClass")
  {
    CHECK(isSinkMediaClass("Audio/Sink"));
    CHECK(isSinkMediaClass("Audio/Duplex"));
    CHECK(isSinkMediaClass("Stream/Output/Audio/Sink"));
    CHECK_FALSE(isSinkMediaClass("Audio/Source"));
    CHECK_FALSE(isSinkMediaClass("Video/Sink"));
  }

  SECTION("lookupProperty")
  {
    struct spa_dict_item items[] = {SPA_DICT_ITEM_INIT("key1", "val1"), SPA_DICT_ITEM_INIT("key2", "val2")};
    struct spa_dict dict = SPA_DICT_INIT(items, 2);

    CHECK(lookupProperty(&dict, "key1") == "val1");
    CHECK(lookupProperty(&dict, "key2") == "val2");
    CHECK(lookupProperty(&dict, "key3").empty());
    CHECK(lookupProperty(nullptr, "key1").empty());
  }

  SECTION("parseNodeRecord")
  {
    struct spa_dict_item items[] = {SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CLASS, "Audio/Sink"),
                                    SPA_DICT_ITEM_INIT(PW_KEY_NODE_NAME, "test-node"),
                                    SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_SERIAL, "1234"),
                                    SPA_DICT_ITEM_INIT("node.driver-id", "5678")};
    struct spa_dict dict = SPA_DICT_INIT(items, 4);

    auto record = parseNodeRecord(1, &dict);
    CHECK(record.version == 1);
    CHECK(record.mediaClass == "Audio/Sink");
    CHECK(record.nodeName == "test-node");
    CHECK(record.objectSerial == 1234);
    CHECK(record.driverId == 5678);
  }
}

TEST_CASE("PipeWireMonitorHelpers - Format Capabilities", "[audio][pipewire][monitor]")
{
  SECTION("sampleFormatCapabilityFromSpaFormat")
  {
    auto cap16 = sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_S16_LE);
    REQUIRE(cap16);
    CHECK(cap16->bitDepth == 16);
    CHECK(cap16->isFloat == false);

    auto capF32 = sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_F32_LE);
    REQUIRE(capF32);
    CHECK(capF32->bitDepth == 32);
    CHECK(capF32->isFloat == true);

    CHECK_FALSE(sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_UNKNOWN));
  }

  SECTION("addSampleFormatCapability")
  {
    DeviceCapabilities caps;
    SampleFormatCapability cap{.bitDepth = 32, .validBits = 24, .isFloat = false};

    addSampleFormatCapability(caps, cap);
    CHECK(caps.sampleFormats.size() == 1);
    CHECK(caps.bitDepths.empty()); // Not added because validBits != bitDepth

    SampleFormatCapability cap16{.bitDepth = 16, .validBits = 16, .isFloat = false};
    addSampleFormatCapability(caps, cap16);
    CHECK(caps.sampleFormats.size() == 2);
    CHECK(caps.bitDepths == std::vector<uint8_t>{16});
  }
}

TEST_CASE("PipeWireMonitorHelpers - SPA Pod Parsing", "[audio][pipewire][monitor]")
{
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  SECTION("parseEnumFormat - Sample Rates and Channels")
  {
    struct spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b,
                        SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format,
                        SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                        SPA_FORMAT_AUDIO_channels,
                        SPA_POD_Int(2),
                        0);

    // Add rate as a range
    spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);
    struct spa_pod_frame f2;
    spa_pod_builder_push_choice(&b, &f2, SPA_CHOICE_Range, 0);
    spa_pod_builder_int(&b, 48000);  // default
    spa_pod_builder_int(&b, 44100);  // min
    spa_pod_builder_int(&b, 192000); // max
    spa_pod_builder_pop(&b, &f2);

    struct spa_pod* pod = (struct spa_pod*)spa_pod_builder_pop(&b, &f);

    DeviceCapabilities caps;
    parseEnumFormat(pod, caps);

    CHECK_FALSE(caps.sampleRates.empty());
    CHECK(std::ranges::contains(caps.sampleRates, 48000u));
    CHECK(std::ranges::contains(caps.sampleRates, 44100u));
    CHECK(std::ranges::contains(caps.sampleRates, 192000u));
    REQUIRE(caps.channelCounts.size() == 1);
    CHECK(caps.channelCounts[0] == 2);
    REQUIRE(caps.bitDepths.size() == 1);
    CHECK(caps.bitDepths[0] == 16);
  }

  SECTION("parseEnumFormat - Discrete Sample Rates")
  {
    struct spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b,
                        SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format,
                        SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                        SPA_FORMAT_AUDIO_channels,
                        SPA_POD_Int(2),
                        0);

    // Add rate as an enum
    spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);
    struct spa_pod_frame f2;
    spa_pod_builder_push_choice(&b, &f2, SPA_CHOICE_Enum, 0);
    spa_pod_builder_int(&b, 44100); // default
    spa_pod_builder_int(&b, 44100); // choice 1
    spa_pod_builder_int(&b, 48000); // choice 2
    spa_pod_builder_pop(&b, &f2);

    struct spa_pod* pod = (struct spa_pod*)spa_pod_builder_pop(&b, &f);

    DeviceCapabilities caps;
    parseEnumFormat(pod, caps);

    CHECK(caps.sampleRates.size() == 2);
    CHECK(std::ranges::contains(caps.sampleRates, 44100u));
    CHECK(std::ranges::contains(caps.sampleRates, 48000u));
  }
}
