// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/StreamingSource.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("StreamingSource - Core Logic", "[audio][unit][streaming_source]")
  {
    auto const format =
      Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
    auto const info =
      DecodedStreamInfo{.sourceFormat = format, .outputFormat = format, .duration = std::chrono::seconds{1}};

    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    // NOTE: StreamingSource contains a ~2MB inline ring buffer. Under ASAN, the
    // stack frame for this test function would be ~14MB which exceeds the 8MB
    // default stack. All source instances are heap-allocated for this reason.
    SECTION("Initialize matrix")
    {
      SECTION("Preroll success starts thread")
      {
        auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
        auto block = std::vector(400, std::byte{0}); // 200ms
        decoderPtr->setReadScript({{block, false}, {{}, true}});

        auto sourcePtr = std::make_unique<StreamingSource>(
          std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
        REQUIRE(sourcePtr->initialize());
        REQUIRE(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
        REQUIRE(errorCount.load() == 0);
      }

      SECTION("EOF during preroll succeeds without thread error")
      {
        auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
        auto block = std::vector(20, std::byte{0}); // 10ms
        decoderPtr->setReadScript({{block, true}}); // EOF immediately

        auto sourcePtr = std::make_unique<StreamingSource>(
          std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
        REQUIRE(sourcePtr->initialize());
        REQUIRE(sourcePtr->isDrained());
        REQUIRE(errorCount.load() == 0);
      }

      SECTION("Preroll decode failure returns error")
      {
        auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
        decoderPtr->setReadScript({{{}, false, std::unexpected(Error{.message = "fail"})}});

        auto sourcePtr = std::make_unique<StreamingSource>(
          std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
        auto result = sourcePtr->initialize();
        REQUIRE_FALSE(result);
        REQUIRE(errorCount.load() == 1);
      }
    }

    SECTION("Seek matrix")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      auto block = std::vector(400, std::byte{0});
      decoderPtr->setReadScript({{block, false}, {block, false}, {{}, true}});

      auto sourcePtr = std::make_unique<StreamingSource>(
        std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
      REQUIRE(sourcePtr->initialize());

      SECTION("Successful seek clears and re-prerolls")
      {
        REQUIRE(sourcePtr->seek(std::chrono::milliseconds{50}));
        REQUIRE(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
        REQUIRE(errorCount.load() == 0);
      }

      SECTION("Failed seek returns error")
      {
        auto decoder2Ptr = std::make_unique<ScriptedDecoderSession>(info);
        decoder2Ptr->setSeekResult(std::unexpected(Error{.message = "seek fail"}));
        decoder2Ptr->setReadScript({{block, false}});

        auto source2Ptr = std::make_unique<StreamingSource>(
          std::move(decoder2Ptr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
        REQUIRE(source2Ptr->initialize());

        auto result = source2Ptr->seek(std::chrono::milliseconds{50});
        REQUIRE_FALSE(result);
        REQUIRE(errorCount.load() >= 1);
      }
    }

    SECTION("Background decode failure marks source failed")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      auto block = std::vector(200, std::byte{0}); // 100ms
      decoderPtr->setReadScript({
        {block, false},                                              // first block for preroll
        {{}, false, std::unexpected(Error{.message = "async fail"})} // second block fails
      });

      auto sourcePtr = std::make_unique<StreamingSource>(
        std::move(decoderPtr), info, onError, std::chrono::milliseconds{50}, std::chrono::milliseconds{500});
      REQUIRE(sourcePtr->initialize());

      // Wait for async failure — polling with timeout, exits as soon as error fires
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

      while (errorCount.load() == 0 && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }

      REQUIRE(errorCount.load() == 1);
    }

    SECTION("Buffered duration and isDrained track EOF plus ring exhaustion")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      auto block = std::vector(20, std::byte{0}); // 10ms
      decoderPtr->setReadScript({{block, false}, {{}, true}});

      auto sourcePtr = std::make_unique<StreamingSource>(
        std::move(decoderPtr), info, onError, std::chrono::milliseconds{5}, std::chrono::milliseconds{500});
      REQUIRE(sourcePtr->initialize());

      // Consume data
      auto out = std::vector<std::byte>(20);
      REQUIRE(sourcePtr->read(out) == 20);

      // Wait for EOF to be processed — polling with timeout
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

      while (!sourcePtr->isDrained() && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }

      REQUIRE(sourcePtr->isDrained());
      REQUIRE(sourcePtr->bufferedDuration() == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::audio::test
