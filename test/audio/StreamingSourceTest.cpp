// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/StreamingSource.h"

#include "EngineTestSupport.h"
#include "ScriptedDecoderSession.h"
#include "lib/audio/PcmFrameRing.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <tuple>
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

  TEST_CASE("StreamingSource - preparation fills the configured preroll target")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic<int>{0};
    auto onError = [&](Error const&, std::stop_token) { errorCount.fetch_add(1); };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(400); // 200ms
    decoderPtr->setReadScript({{block, false}, {{}, true}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    CHECK(sourcePtr->bufferedDuration() >= std::chrono::milliseconds{100});
    sourcePtr->activate(onError);
    sourcePtr.reset(); // Join the producer before asserting callback silence.
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - preparation accepts EOF during preroll without reporting an error")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic<int>{0};
    auto onError = [&](Error const&, std::stop_token) { errorCount.fetch_add(1); };

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
    sourcePtr.reset();
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - preparation prerolls synchronously")
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

  TEST_CASE("StreamingSource - preparation reports preroll decode failure synchronously")
  {
    auto const info = testStreamInfo();

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{{}, false, std::unexpected(Error{.message = "fail"})}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    auto const res = sourcePtr->prepare();

    REQUIRE_FALSE(res);
    CHECK(res.error().message == "fail");
  }

  TEST_CASE("StreamingSource - preparation rejects a decoded block larger than the ring buffer synchronously")
  {
    auto const info = testStreamInfo();

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{silenceBlock(kPcmFrameRingByteCapacity + frameBytes(info.outputFormat)), false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{1}, std::chrono::milliseconds{500});
    auto const res = sourcePtr->prepare();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::DecodeFailed);
  }

  TEST_CASE("StreamingSource - preparation rejects a decoded block that is not a whole number of frames synchronously")
  {
    auto const info = testStreamInfo();

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{silenceBlock(frameBytes(info.outputFormat) + 1), false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{1}, std::chrono::milliseconds{500});
    auto const res = sourcePtr->prepare();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::DecodeFailed);
    CHECK(res.error().message == "Decoded PCM block is not a whole number of frames");
  }

  TEST_CASE("StreamingSource - seek clears buffered data and prerolls the requested offset")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic<int>{0};
    auto onError = [&](Error const&, std::stop_token) { errorCount.fetch_add(1); };
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
    sourcePtr.reset();
    CHECK(errorCount.load() == 0);
  }

  TEST_CASE("StreamingSource - seek reports decoder failure")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic<int>{0};
    auto onError = [&](Error const&, std::stop_token) { errorCount.fetch_add(1); };
    auto block = silenceBlock(400);

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setSeekResult(std::unexpected(Error{.message = "seek fail"}));
    decoderPtr->setReadScript({{block, false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);

    auto const res = sourcePtr->seek(std::chrono::milliseconds{50});
    REQUIRE_FALSE(res);
    CHECK(res.error().message == "seek fail");
    sourcePtr.reset();
    CHECK(errorCount.load() == 1);
  }

  TEST_CASE("StreamingSource - only a current worker seek may report failure", "[concurrency]")
  {
    auto const info = testStreamInfo();
    auto const workerOffset = std::chrono::milliseconds{300};
    auto const timeout = std::chrono::seconds{5};
    auto entered = std::binary_semaphore{0};
    auto finish = std::counting_semaphore{0};
    bool observedStop = false;
    std::size_t readyCount = 0;
    auto errors = std::vector<Error>{};
    auto errorReady = std::counting_semaphore{0};
    auto const afterSeek = std::vector(20, std::byte{0x22});
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setReadScript({{std::vector(20, std::byte{0x11}), true}});
    decoderPtr->setSeekReadScript({{afterSeek, true}});
    decoderPtr->setWorkerSeek(workerOffset,
                              [&](std::stop_token token) -> Result<>
                              {
                                auto wakeOnStop = std::stop_callback{token, [&] { finish.release(); }};
                                entered.release();

                                if (!finish.try_acquire_for(timeout))
                                {
                                  return makeError(Error::Code::InvalidState, "worker seek timed out");
                                }

                                observedStop = token.stop_requested();
                                return makeError(Error::Code::SeekFailed, "worker seek failed");
                              });
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(
      [&](Error const& error, std::stop_token)
      {
        errors.push_back(error);
        errorReady.release();
      },
      [&](std::stop_token) { ++readyCount; });

    REQUIRE(sourcePtr->seek(workerOffset));
    REQUIRE(entered.try_acquire_for(timeout));
    auto output = std::vector<std::byte>(afterSeek.size());
    CHECK(sourcePtr->read(output) == 0);
    CHECK_FALSE(sourcePtr->isDrained());
    CHECK(sourcePtr->isPreparingSeek());

    SECTION("a superseding cheap seek cancels the worker and exposes only its replacement PCM")
    {
      REQUIRE(sourcePtr->seek(std::chrono::milliseconds{50}));
      REQUIRE(sourcePtr->read(output) == output.size());
      CHECK(output == afterSeek);
      CHECK(sourcePtr->isDrained());
      CHECK_FALSE(sourcePtr->isPreparingSeek());
      sourcePtr.reset();
      CHECK(observedStop);
      CHECK(errors.empty());
    }

    SECTION("destruction cancels the worker without emitting its error")
    {
      sourcePtr.reset();
      CHECK(observedStop);
      CHECK(errors.empty());
    }

    SECTION("an uncancelled seek failure reaches the installed callback once")
    {
      finish.release();
      REQUIRE(errorReady.try_acquire_for(timeout));
      sourcePtr.reset();
      CHECK_FALSE(observedStop);
      REQUIRE(errors.size() == 1);
      CHECK(errors.front().code == Error::Code::SeekFailed);
      CHECK(errors.front().message == "worker seek failed");
    }

    CHECK(readyCount == 0);
  }

  TEST_CASE("StreamingSource - a worker seek returning success after cancellation emits neither ready nor error",
            "[concurrency]")
  {
    auto const info = testStreamInfo();
    auto const offset = std::chrono::milliseconds{200};
    auto gate = SeekTestGate{};
    auto workerToken = std::stop_token{};
    std::size_t errors = 0;
    std::size_t readyCount = 0;
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setWorkerSeek(offset,
                              [&](std::stop_token token) -> Result<>
                              {
                                workerToken = token;
                                return {};
                              });
    // The ordinary seek has committed past the fake's cancellation check and will return success.
    decoderPtr->setSeekObserver([&](std::chrono::milliseconds) { std::ignore = gate.enterAndWait(workerToken); });
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare(offset));
    sourcePtr->activate([&](Error const&, std::stop_token) { ++errors; }, [&](std::stop_token) { ++readyCount; });
    REQUIRE(gate.tryWaitForEntry());
    sourcePtr.reset();
    REQUIRE(gate.outcome() == SeekTestGate::Outcome::Cancelled);
    CHECK(errors == 0);
    CHECK(readyCount == 0);
  }

  TEST_CASE("StreamingSource - cancellation during worker preroll delivers neither ready nor the late read failure",
            "[concurrency]")
  {
    auto const info = testStreamInfo();
    auto const offset = std::chrono::milliseconds{200};
    auto const timeout = std::chrono::seconds{5};
    auto readEntered = std::binary_semaphore{0};
    auto workerToken = std::stop_token{};
    bool cancelledInRead = false;
    std::size_t errors = 0;
    std::size_t readyCount = 0;
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    decoderPtr->setWorkerSeek(offset,
                              [&](std::stop_token token) -> Result<>
                              {
                                workerToken = token;
                                return {};
                              });
    decoderPtr->setSeekReadScript(
      {{silenceBlock(20), false}, {{}, false, makeError(Error::Code::DecodeFailed, "late preroll failure")}});
    decoderPtr->setReadObserver(
      [&](std::size_t read)
      {
        if (read != 2)
        {
          return;
        }

        auto stopped = std::binary_semaphore{0};
        auto wakeOnStop = std::stop_callback{workerToken, [&] { stopped.release(); }};
        readEntered.release();
        cancelledInRead = stopped.try_acquire_for(timeout);
      });
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{100}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare(offset));
    sourcePtr->activate([&](Error const&, std::stop_token) { ++errors; }, [&](std::stop_token) { ++readyCount; });
    REQUIRE(readEntered.try_acquire_for(timeout));
    CHECK(sourcePtr->isPreparingSeek());
    CHECK(sourcePtr->bufferedDuration() == std::chrono::milliseconds{10});
    auto output = silenceBlock(20);
    CHECK(sourcePtr->read(output) == 0);
    sourcePtr.reset();
    CHECK(cancelledInRead);
    CHECK(errors == 0);
    CHECK(readyCount == 0);
  }

  TEST_CASE("StreamingSource - background decode failure after activation notifies error callback", "[concurrency]")
  {
    auto const info = testStreamInfo();
    auto errors = std::vector<Error>{};
    auto errorReady = std::counting_semaphore{0};
    auto onError = [&](Error const& error, std::stop_token)
    {
      errors.push_back(error);
      errorReady.release();
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

    REQUIRE(errorReady.try_acquire_for(std::chrono::seconds{5}));
    sourcePtr.reset();
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().message == "async fail");
  }

  TEST_CASE("StreamingSource - background non-whole-frame decoded block notifies the error callback once",
            "[concurrency]")
  {
    auto const info = testStreamInfo();
    auto errors = std::vector<Error>{};
    auto errorReady = std::counting_semaphore{0};
    auto onError = [&](Error const& error, std::stop_token)
    {
      errors.push_back(error);
      errorReady.release();
    };

    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
    auto block = silenceBlock(200); // 100ms, enough for the 50ms preroll
    decoderPtr->setReadScript({{block, false}, {silenceBlock(frameBytes(info.outputFormat) + 1), false}});

    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), info, std::chrono::milliseconds{50}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate(onError);

    REQUIRE(errorReady.try_acquire_for(std::chrono::seconds{5}));
    sourcePtr.reset();
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().code == Error::Code::DecodeFailed);
    CHECK(errors.front().message == "Decoded PCM block is not a whole number of frames");
  }

  TEST_CASE("StreamingSource - read drains source after EOF is reached")
  {
    auto const info = testStreamInfo();
    auto errorCount = std::atomic<int>{0};
    auto onError = [&](Error const&, std::stop_token) { errorCount.fetch_add(1); };

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
    sourcePtr.reset();
    CHECK(errorCount.load() == 0);
  }
} // namespace ao::audio::test
