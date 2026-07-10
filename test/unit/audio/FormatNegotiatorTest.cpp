// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/FormatNegotiator.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace ao::audio::test
{
  TEST_CASE("FormatNegotiator - builds conversion plan from source format and device capabilities",
            "[audio][unit][format-negotiator]")
  {
    auto sourceFormat =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
    auto caps = DeviceCapabilities{};
    caps.sampleRates = {44100, 48000, 96000};
    caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false},
                          {.bitDepth = 24, .validBits = 24, .isFloat = false},
                          {.bitDepth = 32, .validBits = 24, .isFloat = false},
                          {.bitDepth = 32, .validBits = 32, .isFloat = false}};
    caps.bitDepths = {16, 24, 32};
    caps.channelCounts = {2, 6};

    SECTION("Direct passthrough when formats match")
    {
      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

      CHECK(plan.isResampleRequired == false);
      CHECK(plan.isBitDepthConversionRequired == false);
      CHECK(plan.isChannelRemapRequired == false);
      CHECK(plan.deviceFormat.sampleRate == sourceFormat.sampleRate);
      CHECK(plan.deviceFormat.channels == sourceFormat.channels);
      CHECK(plan.deviceFormat.bitDepth == sourceFormat.bitDepth);
      CHECK(plan.reason == "Direct passthrough");
    }

    SECTION("Sample rate resampling required")
    {
      caps.sampleRates = {48000, 96000}; // 44100 is not supported
      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

      CHECK(plan.isResampleRequired == true);
      // (48000 % 44100) = 3900
      // (96000 % 44100) = 7800
      // 48000 is chosen as "closest" via modulo
      CHECK(plan.deviceFormat.sampleRate == 48000);
      CHECK(plan.reason.contains("resampling required"));
    }

    SECTION("Bit depth conversion required")
    {
      caps.bitDepths = {24, 32}; // 16 is not supported
      caps.sampleFormats = {};   // Force fallback logic
      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

      CHECK(plan.isBitDepthConversionRequired == true);
      CHECK(plan.deviceFormat.bitDepth == 32); // Max element
      CHECK(plan.reason.contains("bit depth conversion required"));
    }

    SECTION("Channel remapping required")
    {
      caps.channelCounts = {6, 8}; // 2 is not supported
      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

      CHECK(plan.isChannelRemapRequired == true);
      CHECK(plan.deviceFormat.channels == 6); // First element
      CHECK(plan.reason.contains("channel remapping required"));
    }

    SECTION("Decoder output format negotiation - 24-bit source")
    {
      sourceFormat.bitDepth = 24;
      sourceFormat.validBits = 24;

      // Test 1: Prefer exact 24/24 over padded or full 32-bit formats.
      caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false},
                            {.bitDepth = 24, .validBits = 24, .isFloat = false},
                            {.bitDepth = 32, .validBits = 24, .isFloat = false},
                            {.bitDepth = 32, .validBits = 32, .isFloat = false}};
      caps.bitDepths = {16, 24, 32};
      auto plan1 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan1.decoderOutputFormat.bitDepth == 24);
      CHECK(plan1.decoderOutputFormat.validBits == 24);
      CHECK(plan1.deviceFormat.bitDepth == 24);
      CHECK(plan1.deviceFormat.validBits == 24);

      // Test 2: Prefer 32/24 when packed 24-bit is unavailable.
      caps.sampleFormats = {
        {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 24, .isFloat = false}};
      caps.bitDepths = {16};
      auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan2.decoderOutputFormat.bitDepth == 32);
      CHECK(plan2.decoderOutputFormat.validBits == 24);
      CHECK(plan2.deviceFormat.bitDepth == 32);
      CHECK(plan2.deviceFormat.validBits == 24);

      // Test 3: Fall back to a true 32/32 device when that is the only 32-bit path.
      caps.sampleFormats = {
        {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 32, .isFloat = false}};
      caps.bitDepths = {16, 32};
      auto plan3 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan3.decoderOutputFormat.bitDepth == 32);
      CHECK(plan3.decoderOutputFormat.validBits == 24);
      CHECK(plan3.deviceFormat.bitDepth == 32);
      CHECK(plan3.deviceFormat.validBits == 32);

      // Test 4: Device max 16-bit, decoder outputs 16/16.
      caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false}};
      caps.bitDepths = {16};
      auto plan4 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan4.decoderOutputFormat.bitDepth == 16);
      CHECK(plan4.decoderOutputFormat.validBits == 16);
      CHECK(plan4.deviceFormat.bitDepth == 16);
      CHECK(plan4.deviceFormat.validBits == 16);
    }

    SECTION("Decoder output format negotiation - 32-bit source")
    {
      sourceFormat.bitDepth = 32;
      sourceFormat.validBits = 32;

      // Test 1: True 32-bit sources only accept true 32/32 output.
      caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false},
                            {.bitDepth = 24, .validBits = 24, .isFloat = false},
                            {.bitDepth = 32, .validBits = 24, .isFloat = false},
                            {.bitDepth = 32, .validBits = 32, .isFloat = false}};
      caps.bitDepths = {16, 24, 32};
      auto plan1 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan1.decoderOutputFormat.bitDepth == 32);
      CHECK(plan1.decoderOutputFormat.validBits == 32);
      CHECK(plan1.deviceFormat.bitDepth == 32);
      CHECK(plan1.deviceFormat.validBits == 32);

      // Test 2: 32/24 padded support alone is not enough for a true 32-bit source.
      caps.sampleFormats = {
        {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 24, .isFloat = false}};
      caps.bitDepths = {16};
      auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan2.decoderOutputFormat.bitDepth == 16);
      CHECK(plan2.decoderOutputFormat.validBits == 16);
      CHECK(plan2.deviceFormat.bitDepth == 16);
      CHECK(plan2.deviceFormat.validBits == 16);
    }

    SECTION("32-bit container with 24 valid bits keeps native 32/24 output")
    {
      sourceFormat.bitDepth = 32;
      sourceFormat.validBits = 24;
      caps.sampleFormats = {
        {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 24, .isFloat = false}};
      caps.bitDepths = {16};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK_FALSE(plan.isBitDepthConversionRequired);
      CHECK(plan.decoderOutputFormat.bitDepth == 32);
      CHECK(plan.decoderOutputFormat.validBits == 24);
      CHECK(plan.deviceFormat.bitDepth == 32);
      CHECK(plan.deviceFormat.validBits == 24);
    }

    SECTION("16-bit source pads to 32/16 when only 32-bit container exists")
    {
      sourceFormat.bitDepth = 16;
      sourceFormat.validBits = 16;
      caps.bitDepths = {32};
      caps.sampleFormats = {}; // Force fallback logic

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.decoderOutputFormat.bitDepth == 32);
      CHECK(plan.decoderOutputFormat.validBits == 16);
      CHECK(plan.deviceFormat.bitDepth == 32);
      CHECK(plan.deviceFormat.validBits == 16);
      CHECK(plan.isBitDepthConversionRequired == true);
    }

    SECTION("8-bit integer source decodes into the negotiated storage width with 8 valid bits")
    {
      sourceFormat.bitDepth = 8;
      sourceFormat.validBits = 8;
      caps.sampleFormats = {};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.decoderOutputFormat.bitDepth == 32);
      CHECK(plan.decoderOutputFormat.validBits == 8);
      CHECK_FALSE(plan.decoderOutputFormat.isFloat);
      CHECK(plan.deviceFormat.bitDepth == 32);
      CHECK(plan.deviceFormat.validBits == 8);
    }

    SECTION("32-bit float source keeps float output when supported")
    {
      sourceFormat.bitDepth = 32;
      sourceFormat.validBits = 32;
      sourceFormat.isFloat = true;
      caps.sampleFormats = {
        {.bitDepth = 32, .validBits = 32, .isFloat = false}, {.bitDepth = 32, .validBits = 32, .isFloat = true}};
      caps.bitDepths = {32};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.decoderOutputFormat.bitDepth == 32);
      CHECK(plan.decoderOutputFormat.validBits == 32);
      CHECK(plan.decoderOutputFormat.isFloat);
      CHECK(plan.deviceFormat.bitDepth == 32);
      CHECK(plan.deviceFormat.validBits == 32);
      CHECK(plan.deviceFormat.isFloat);
      CHECK_FALSE(plan.isBitDepthConversionRequired);
    }

    SECTION("32-bit float source uses integer decoder output when float is unsupported")
    {
      sourceFormat.bitDepth = 32;
      sourceFormat.validBits = 32;
      sourceFormat.isFloat = true;
      caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false}};
      caps.bitDepths = {16};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.isBitDepthConversionRequired);
      CHECK_FALSE(plan.decoderOutputFormat.isFloat);
      CHECK(plan.decoderOutputFormat.bitDepth == 16);
      CHECK(plan.decoderOutputFormat.validBits == 16);
      CHECK(plan.deviceFormat.bitDepth == 16);
      CHECK(plan.deviceFormat.validBits == 16);
    }

    SECTION("Reason string concatenates multiple required conversions in stable order")
    {
      caps.sampleRates = {48000};
      caps.bitDepths = {32};
      caps.sampleFormats = {}; // Force fallback logic
      caps.channelCounts = {6};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.isResampleRequired == true);
      CHECK(plan.isBitDepthConversionRequired == true);
      CHECK(plan.isChannelRemapRequired == true);

      CHECK_THAT(plan.reason, Catch::Matchers::ContainsSubstring("sample rate resampling required"));
      CHECK_THAT(plan.reason, Catch::Matchers::ContainsSubstring("bit depth conversion required"));
      CHECK_THAT(plan.reason, Catch::Matchers::ContainsSubstring("channel remapping required"));
    }

    SECTION("requireBitDepthConversion with existing reason")
    {
      sourceFormat.bitDepth = 24;
      sourceFormat.validBits = 24;
      caps.sampleRates = {48000}; // resampling required
      caps.bitDepths = {24, 32};  // 24 is supported, so negotiate(bitDepth) does nothing
      caps.sampleFormats = {{.bitDepth = 32, .validBits = 24, .isFloat = false}}; // but 24/24 is NOT in sampleFormats

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.isResampleRequired == true);
      CHECK(plan.isBitDepthConversionRequired == true);
      CHECK_THAT(plan.reason,
                 Catch::Matchers::ContainsSubstring("sample rate resampling required; bit depth conversion required"));
    }

    SECTION("Empty sampleFormats still chooses a safe 24-bit fallback")
    {
      sourceFormat.sampleRate = 96000;
      sourceFormat.bitDepth = 24;
      sourceFormat.validBits = 24;
      caps.sampleRates = {96000};
      caps.bitDepths = {32};
      caps.sampleFormats = {};

      auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);
      CHECK(plan.decoderOutputFormat.bitDepth == 32);
      CHECK(plan.decoderOutputFormat.validBits == 24);
      CHECK(plan.deviceFormat.bitDepth == 32);
      CHECK(plan.deviceFormat.validBits == 24);
    }
  }
} // namespace ao::audio::test
