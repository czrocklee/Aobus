#include <ao/audio/FormatNegotiator.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

using namespace ao::audio;
using namespace ao::audio;
using namespace ao::audio;

TEST_CASE("FormatNegotiator - Build Plan", "[playback][format_negotiator]")
{
  Format sourceFormat{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
  DeviceCapabilities caps;
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
    sourceFormat.validBits = 24;

    // Test 1: Prefer exact 24/24 over padded or full 32-bit formats.
    caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false},
                          {.bitDepth = 24, .validBits = 24, .isFloat = false},
                          {.bitDepth = 32, .validBits = 24, .isFloat = false},
                          {.bitDepth = 32, .validBits = 32, .isFloat = false}};
    caps.bitDepths = {16, 24, 32};
    auto plan1 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan1.decoderOutputFormat.bitDepth == 24);
    REQUIRE(plan1.decoderOutputFormat.validBits == 24);
    REQUIRE(plan1.deviceFormat.bitDepth == 24);
    REQUIRE(plan1.deviceFormat.validBits == 24);

    // Test 2: Prefer 32/24 when packed 24-bit is unavailable.
    caps.sampleFormats = {
      {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 24, .isFloat = false}};
    caps.bitDepths = {16};
    auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan2.decoderOutputFormat.bitDepth == 32);
    REQUIRE(plan2.decoderOutputFormat.validBits == 24);
    REQUIRE(plan2.deviceFormat.bitDepth == 32);
    REQUIRE(plan2.deviceFormat.validBits == 24);

    // Test 3: Fall back to a true 32/32 device when that is the only 32-bit path.
    caps.sampleFormats = {
      {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 32, .isFloat = false}};
    caps.bitDepths = {16, 32};
    auto plan3 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan3.decoderOutputFormat.bitDepth == 32);
    REQUIRE(plan3.decoderOutputFormat.validBits == 24);
    REQUIRE(plan3.deviceFormat.bitDepth == 32);
    REQUIRE(plan3.deviceFormat.validBits == 32);

    // Test 4: Device max 16-bit, decoder outputs 16/16.
    caps.sampleFormats = {{.bitDepth = 16, .validBits = 16, .isFloat = false}};
    caps.bitDepths = {16};
    auto plan4 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan4.decoderOutputFormat.bitDepth == 16);
    REQUIRE(plan4.decoderOutputFormat.validBits == 16);
    REQUIRE(plan4.deviceFormat.bitDepth == 16);
    REQUIRE(plan4.deviceFormat.validBits == 16);
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
    REQUIRE(plan1.decoderOutputFormat.bitDepth == 32);
    REQUIRE(plan1.decoderOutputFormat.validBits == 32);
    REQUIRE(plan1.deviceFormat.bitDepth == 32);
    REQUIRE(plan1.deviceFormat.validBits == 32);

    // Test 2: 32/24 padded support alone is not enough for a true 32-bit source.
    caps.sampleFormats = {
      {.bitDepth = 16, .validBits = 16, .isFloat = false}, {.bitDepth = 32, .validBits = 24, .isFloat = false}};
    caps.bitDepths = {16};
    auto plan2 = FormatNegotiator::buildPlan(sourceFormat, caps);
    REQUIRE(plan2.decoderOutputFormat.bitDepth == 16);
    REQUIRE(plan2.decoderOutputFormat.validBits == 16);
    REQUIRE(plan2.deviceFormat.bitDepth == 16);
    REQUIRE(plan2.deviceFormat.validBits == 16);
  }
}
