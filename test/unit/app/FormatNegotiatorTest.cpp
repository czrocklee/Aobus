#include "core/playback/FormatNegotiator.h"
#include <catch2/catch.hpp>

using namespace app::core::playback;
using namespace app::core::backend;
using namespace app::core;

TEST_CASE("FormatNegotiator - Build Plan", "[playback][format_negotiator]")
{
  AudioFormat sourceFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
  DeviceCapabilities caps;
  caps.sampleRates = {44100, 48000, 96000};
  caps.bitDepths = {16, 24, 32};
  caps.channelCounts = {2, 6};

  SECTION("Direct passthrough when formats match")
  {
    auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    REQUIRE(plan.requiresResample == false);
    REQUIRE(plan.requiresBitDepthConversion == false);
    REQUIRE(plan.requiresChannelRemap == false);
    REQUIRE(plan.deviceFormat.sampleRate == sourceFormat.sampleRate);
    REQUIRE(plan.deviceFormat.channels == sourceFormat.channels);
    REQUIRE(plan.deviceFormat.bitDepth == sourceFormat.bitDepth);
    REQUIRE(plan.reason == "Direct passthrough");
  }

  SECTION("Sample rate resampling required")
  {
    caps.sampleRates = {48000, 96000}; // 44100 is not supported
    auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    REQUIRE(plan.requiresResample == true);
    // (48000 % 44100) = 3900
    // (96000 % 44100) = 7800
    // 48000 is chosen as "closest" via modulo
    REQUIRE(plan.deviceFormat.sampleRate == 48000);
    REQUIRE(plan.reason.find("resampling required") != std::string::npos);
  }

  SECTION("Bit depth conversion required")
  {
    caps.bitDepths = {24, 32}; // 16 is not supported
    auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    REQUIRE(plan.requiresBitDepthConversion == true);
    REQUIRE(plan.deviceFormat.bitDepth == 32); // Max element
    REQUIRE(plan.reason.find("bit depth conversion required") != std::string::npos);
  }

  SECTION("Channel remapping required")
  {
    caps.channelCounts = {6, 8}; // 2 is not supported
    auto plan = FormatNegotiator::buildPlan(sourceFormat, caps);

    REQUIRE(plan.requiresChannelRemap == true);
    REQUIRE(plan.deviceFormat.channels == 6); // First element
    REQUIRE(plan.reason.find("channel remapping required") != std::string::npos);
  }

  SECTION("Decoder output format negotiation - 24-bit source")
  {
    sourceFormat.bitDepth = 24;

    // Test 1: Device supports 32-bit, decoder outputs 32/24
    caps.bitDepths = {16, 24, 32};
    auto plan1 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan1.decoderOutputFormat.bitDepth == 32);
    REQUIRE(plan1.decoderOutputFormat.validBits == 24);

    // Test 2: Device max 24-bit, decoder outputs 24/24
    caps.bitDepths = {16, 24};
    auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan2.decoderOutputFormat.bitDepth == 24);
    REQUIRE(plan2.decoderOutputFormat.validBits == 24);

    // Test 3: Device max 16-bit, decoder outputs 16/16
    caps.bitDepths = {16};
    auto plan3 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan3.decoderOutputFormat.bitDepth == 16);
    REQUIRE(plan3.decoderOutputFormat.validBits == 16);
  }

  SECTION("Decoder output format negotiation - 32-bit source")
  {
    sourceFormat.bitDepth = 32;

    // Test 1: Device supports 32-bit, decoder outputs 32/32
    caps.bitDepths = {16, 24, 32};
    auto plan1 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan1.decoderOutputFormat.bitDepth == 32);
    REQUIRE(plan1.decoderOutputFormat.validBits == 32);

    // Test 2: Device max 24-bit (no 32), decoder drops to 16/16
    caps.bitDepths = {16, 24};
    auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan2.decoderOutputFormat.bitDepth == 16);
    REQUIRE(plan2.decoderOutputFormat.validBits == 16);
  }
}
