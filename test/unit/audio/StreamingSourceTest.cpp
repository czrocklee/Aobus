// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/StreamingSource.h"

#include "ScriptedDecoderSession.h"
#include "lib/audio/PcmRingBuffer.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

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
      auto const sourceFormat = SignalFormat{.sampleRate = 1000, .channels = 1, .precisionBits = 16};
      return DecodedStreamInfo{.sourceFormat = sourceFormat,
                               .outputFormat = pcmFormat(sourceFormat, SampleEncoding::Signed16Le),
                               .duration = std::chrono::seconds{1}};
    }

    std::vector<std::byte> silenceBlock(std::size_t byteCount)
    {
      return std::vector(byteCount, std::byte{0});
    }
  } // namespace

  TEST_CASE("ScriptedDecoderSession - invalid output frame layout is rejected", "[audio][unit][test-support]")
  {
    auto info = testStreamInfo();
    info.outputFormat.channels = 0;
    auto decoder = ScriptedDecoderSession{info};
    decoder.setReadScript({{{std::byte{0}}, true}});

    auto const blockRes = decoder.readNextBlock();

    REQUIRE_FALSE(blockRes);
    CHECK(blockRes.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("StreamingSource - preparation fills the configured preroll target", "[audio][unit][streaming-source]")
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
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    CHECK(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
    sourcePtr->activate(onError);
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - preparation accepts EOF during preroll without reporting an error",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(20); // 10ms
    decoderPtr->setReadScript({{block, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{10});
    sourcePtr->activate(onError);

    auto out = std::vector<std::byte>(20);
    CHECK(sourcePtr->read(out) == 20);
    CHECK(sourcePtr->isDrained());
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - preparation prerolls synchronously", "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto* const decoderRaw = decoderPtr.get();
    auto block = silenceBlock(400); // 200ms, enough for the 100ms preroll.
    decoderPtr->setReadScript({{block, false}, {{}, true}});
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});

    REQUIRE(sourcePtr->prepare());

    CHECK(decoderRaw->readCount() == 1);
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{200});
  }

  TEST_CASE("StreamingSource - preparation reports preroll decode failure synchronously",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{{}, false, std::unexpected(Error{.message = "fail"})}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    auto const result = sourcePtr->prepare();

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "fail");
  }

  TEST_CASE("StreamingSource - preparation rejects a decoded block larger than the ring buffer synchronously",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{silenceBlock(kRingBufferCapacity + 1), false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{1}, std::chrono::milliseconds{500});
    auto const result = sourcePtr->prepare();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::DecodeFailed);
  }

  TEST_CASE("StreamingSource - seek clears buffered data and prerolls the requested offset",
            "[audio][unit][streaming-source]")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic{0};
    auto onError = [&](Error const&) { errorCount.fetch_add(1); };
    auto beforeSeekBlock = std::vector(400, std::byte{0x11});
    auto afterSeekBlock = std::vector(400, std::byte{0x22});

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto* const decoderRaw = decoderPtr.get();
    decoderPtr->setReadScript({{beforeSeekBlock, false}, {{}, true}});
    decoderPtr->setSeekReadScript({{afterSeekBlock, false}, {{}, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);

    CHECK(sourcePtr->seek(std::chrono::milliseconds{50}));
    CHECK(decoderRaw->lastSeekOffset() == std::chrono::milliseconds{50});
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{200});

    auto output = std::vector<std::byte>(afterSeekBlock.size());
    REQUIRE(sourcePtr->read(output) == output.size());
    CHECK(output == afterSeekBlock);
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
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);

    auto result = sourcePtr->seek(std::chrono::milliseconds{50});
    CHECK_FALSE(result);
    CHECK(errorCount.load() == 1);
  }

  TEST_CASE("StreamingSource - background decode failure after activation notifies error callback",
            "[audio][unit][streaming-source][concurrency]")
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
      std::move(decoderPtr), info, std::chrono::milliseconds{50}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);

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
      std::move(decoderPtr), info, std::chrono::milliseconds{20}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{10});

    auto out = std::vector<std::byte>(20);
    CHECK(sourcePtr->read(out) == 20);

    CHECK(sourcePtr->isDrained());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{0});
    CHECK(errorCount.load() == 0);
  }
} // namespace ao::audio::test
