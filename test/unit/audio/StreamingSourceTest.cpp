// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmRingBuffer.h>
#include <ao/audio/StreamingSource.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    DecodedStreamInfo testStreamInfo()
    {
      auto const format =
        Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
      return DecodedStreamInfo{.sourceFormat = format, .outputFormat = format, .duration = std::chrono::seconds{1}};
    }

    std::vector<std::byte> silenceBlock(std::size_t byteCount)
    {
      return std::vector(byteCount, std::byte{0});
    }
  } // namespace

  TEST_CASE("StreamingSource - initialize starts decode thread after successful preroll",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(400); // 200ms
    decoderPtr->setReadScript({{block, false}, {{}, true}});

    // StreamingSource contains a ~2MB inline ring buffer; heap allocation keeps
    // ASAN stack usage below the default 8MB limit.
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());
    CHECK(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - initialize accepts EOF during preroll without reporting an error",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(20); // 10ms
    decoderPtr->setReadScript({{block, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{10});

    auto out = std::vector<std::byte>(20);
    CHECK(sourcePtr->read(out) == 20);
    CHECK(sourcePtr->isDrained());
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - initialize reports preroll decode failure", "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{{}, false, std::unexpected(Error{.message = "fail"})}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    auto result = sourcePtr->initialize();
    CHECK_FALSE(result);
    CHECK(errorCount.load() == 1);
  }

  TEST_CASE("StreamingSource - initialize rejects a decoded block larger than the ring buffer",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto callbackError = Error{};
    auto onError = [&](Error const& error) { callbackError = error; };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{silenceBlock(kRingBufferCapacity + 1), false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{1}, std::chrono::milliseconds{500});
    auto const result = sourcePtr->initialize();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::DecodeFailed);
    CHECK(callbackError.code == Error::Code::DecodeFailed);
  }

  TEST_CASE("StreamingSource - seek clears buffered data and prerolls the requested offset",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };
    auto block = silenceBlock(400);

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto* const decoderRaw = decoderPtr.get();
    decoderPtr->setReadScript({{block, false}, {block, false}, {{}, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());

    CHECK(sourcePtr->seek(std::chrono::milliseconds{50}));
    CHECK(decoderRaw->lastSeekOffset() == std::chrono::milliseconds{50});
    CHECK(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - seek reports decoder failure", "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };
    auto block = silenceBlock(400);

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setSeekResult(std::unexpected(Error{.message = "seek fail"}));
    decoderPtr->setReadScript({{block, false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());

    auto result = sourcePtr->seek(std::chrono::milliseconds{50});
    CHECK_FALSE(result);
    CHECK(errorCount.load() == 1);
  }

  TEST_CASE("StreamingSource - background decode failure notifies error callback", "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto errorMutex = std::mutex{};
    auto errorCv = std::condition_variable{};
    auto onError = [&](Error const&)
    {
      errorCount.fetch_add(1);
      errorCv.notify_one();
    };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(200); // 100ms
    decoderPtr->setReadScript({
      {block, false},                                              // first block for preroll
      {{}, false, std::unexpected(Error{.message = "async fail"})} // second block fails
    });

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{50}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());

    auto lock = std::unique_lock{errorMutex};
    REQUIRE(errorCv.wait_for(lock, std::chrono::seconds{5}, [&] { return errorCount.load() == 1; }));
    CHECK(errorCount.load() == 1);
  }

  TEST_CASE("StreamingSource - read drains source after EOF is reached", "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(20); // 10ms
    decoderPtr->setReadScript({{block, false}, {{}, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, onError, std::chrono::milliseconds{20}, std::chrono::milliseconds{500});
    CHECK(sourcePtr->initialize());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{10});

    auto out = std::vector<std::byte>(20);
    CHECK(sourcePtr->read(out) == 20);

    CHECK(sourcePtr->isDrained());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{0});
    CHECK(errorCount.load() == 0);
  }
} // namespace ao::audio::test
