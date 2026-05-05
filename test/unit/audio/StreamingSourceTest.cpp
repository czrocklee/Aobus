// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ScriptedDecoderSession.h"

#include <ao/audio/StreamingSource.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace ao::audio;
using namespace std::chrono_literals;

TEST_CASE("StreamingSource - Core Logic", "[audio][unit][streaming_source]")
{
  auto const format =
    Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
  auto const info = DecodedStreamInfo{.sourceFormat = format, .outputFormat = format, .durationMs = 1000};

  std::atomic<int> errorCount{0};
  auto onError = [&](ao::Error const&) { errorCount.fetch_add(1); };

  SECTION("Initialize matrix")
  {
    SECTION("Preroll success starts thread")
    {
      auto decoder = std::make_unique<ScriptedDecoderSession>(info);
      std::vector<std::byte> block(400, std::byte{0}); // 200ms
      decoder->setReadScript({{block, false}, {{}, true}});

      StreamingSource source(std::move(decoder), info, onError, 100, 500);
      REQUIRE(source.initialize());
      REQUIRE(source.bufferedMs() >= 100);
      REQUIRE(errorCount.load() == 0);
    }

    SECTION("EOF during preroll succeeds without thread error")
    {
      auto decoder = std::make_unique<ScriptedDecoderSession>(info);
      std::vector<std::byte> block(20, std::byte{0}); // 10ms
      decoder->setReadScript({{block, true}});        // EOF immediately

      StreamingSource source(std::move(decoder), info, onError, 100, 500);
      REQUIRE(source.initialize());
      REQUIRE(source.isDrained());
      REQUIRE(errorCount.load() == 0);
    }

    SECTION("Preroll decode failure returns error")
    {
      auto decoder = std::make_unique<ScriptedDecoderSession>(info);
      decoder->setReadScript({{{}, false, std::unexpected(ao::Error{.message = "fail"})}});

      StreamingSource source(std::move(decoder), info, onError, 100, 500);
      auto result = source.initialize();
      REQUIRE_FALSE(result);
      REQUIRE(errorCount.load() == 1);
    }
  }

  SECTION("Seek matrix")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    std::vector<std::byte> block(400, std::byte{0});
    decoder->setReadScript({{block, false}, {block, false}, {{}, true}});

    StreamingSource source(std::move(decoder), info, onError, 100, 500);
    REQUIRE(source.initialize());

    SECTION("Successful seek clears and re-prerolls")
    {
      REQUIRE(source.seek(50));
      REQUIRE(source.bufferedMs() >= 100);
      REQUIRE(errorCount.load() == 0);
    }

    SECTION("Failed seek returns error")
    {
      auto decoder2 = std::make_unique<ScriptedDecoderSession>(info);
      decoder2->setSeekResult(std::unexpected(ao::Error{.message = "seek fail"}));
      decoder2->setReadScript({{block, false}});

      StreamingSource source2(std::move(decoder2), info, onError, 100, 500);
      REQUIRE(source2.initialize());

      auto result = source2.seek(50);
      REQUIRE_FALSE(result);
      REQUIRE(errorCount.load() >= 1);
    }
  }

  SECTION("Background decode failure marks source failed")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    std::vector<std::byte> block(200, std::byte{0}); // 100ms
    decoder->setReadScript({
      {block, false},                                                  // first block for preroll
      {{}, false, std::unexpected(ao::Error{.message = "async fail"})} // second block fails
    });

    StreamingSource source(std::move(decoder), info, onError, 50, 500);
    REQUIRE(source.initialize());

    // Wait for async failure
    int retries = 0;
    while (errorCount.load() == 0 && retries < 100)
    {
      std::this_thread::sleep_for(10ms);
      retries++;
    }

    REQUIRE(errorCount.load() == 1);
  }

  SECTION("BufferedMs and isDrained track EOF plus ring exhaustion")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    std::vector<std::byte> block(20, std::byte{0}); // 10ms
    decoder->setReadScript({{block, false}, {{}, true}});

    StreamingSource source(std::move(decoder), info, onError, 5, 500);
    REQUIRE(source.initialize());

    // Consume data
    std::vector<std::byte> out(20);
    REQUIRE(source.read(out) == 20);

    // Wait for EOF to be processed if not already
    int retries = 0;
    while (!source.isDrained() && retries < 100)
    {
      std::this_thread::sleep_for(10ms);
      retries++;
    }

    REQUIRE(source.isDrained());
    REQUIRE(source.bufferedMs() == 0);
  }
}
