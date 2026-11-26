// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/AacDecoderSession.h"
#include "lib/audio/StreamingSource.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    using std::chrono::milliseconds;

    struct DecodeObservation final
    {
      std::size_t calls = 0;
      std::size_t stopAt = 0;
      std::size_t failAt = 0;
      std::stop_source stop{};
    };

    std::atomic<DecodeObservation*>& activeObservation()
    {
      static constinit auto observation = std::atomic<DecodeObservation*>{nullptr};
      return observation;
    }

    auto observe(DecodeObservation& observation)
    {
      activeObservation().store(&observation);
      return utility::ScopedRegistration{[] { activeObservation().store(nullptr); }};
    }

    auto openHe()
    {
      return ao::test::requireValue(
        AacDecoderSession::open(requireAudioFixture("aac-seek/he-single-rate.m4a"), SampleEncoding::Signed16Le));
    }

    // The retained single-rate HE fixture yields 1024 frames (32 ms) per AU.
    PcmBlock readPackets(AacDecoderSession& decoder, std::size_t count)
    {
      auto block = PcmBlock{};

      for (std::size_t index = 0; index < count; ++index)
      {
        block = ao::test::requireValue(decoder.readNextBlock());
      }

      REQUIRE(block.frames == 1024);
      return block;
    }

    auto linearPacket(std::size_t index)
    {
      auto decoderPtr = openHe();
      auto const block = readPackets(*decoderPtr, index + 1);
      return std::vector<std::byte>{block.bytes.begin(), block.bytes.end()};
    }
  } // namespace

  // Linux test-executable interposition only. Always let the real FDK call
  // consume its input, including when modelling an error on return.
  extern "C"
  {
    ::AAC_DECODER_ERROR nativeDecode(::HANDLE_AACDECODER decoder,
                                     ::INT_PCM* pcm,
                                     ::INT size,
                                     ::UINT flags) asm("__real_aacDecoder_DecodeFrame");
    ::AAC_DECODER_ERROR observedDecode(::HANDLE_AACDECODER decoder,
                                       ::INT_PCM* pcm,
                                       ::INT size,
                                       ::UINT flags) asm("__wrap_aacDecoder_DecodeFrame");

    ::AAC_DECODER_ERROR observedDecode(::HANDLE_AACDECODER decoder, ::INT_PCM* pcm, ::INT size, ::UINT flags)
    {
      auto const result = nativeDecode(decoder, pcm, size, flags);

      if (auto* const observation = activeObservation().load(); observation != nullptr)
      {
        ++observation->calls;

        if (observation->calls == observation->stopAt)
        {
          observation->stop.request_stop();
        }

        if (observation->calls == observation->failAt)
        {
          return AAC_DEC_PARSE_ERROR;
        }
      }

      return result;
    }
  }

  TEST_CASE("StreamingSource - a forward HE-AAC worker seek decodes only the gap from live playback", "[concurrency]")
  {
    auto const expected = linearPacket(60);
    auto decoderPtr = openHe();
    auto const info = decoderPtr->streamInfo();
    readPackets(*decoderPtr, 44);
    auto ready = std::binary_semaphore{0};
    auto observation = DecodeObservation{};
    auto guard = observe(observation);
    auto sourcePtr = std::make_unique<StreamingSource>(std::move(decoderPtr), info, milliseconds{1}, milliseconds{1});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate({}, [&](std::stop_token) { ready.release(); });
    REQUIRE(sourcePtr->seek(milliseconds{1920}));
    REQUIRE(ready.try_acquire_for(std::chrono::seconds{5}));
    auto output = std::vector<std::byte>(expected.size());
    REQUIRE(sourcePtr->read(output) == output.size());
    sourcePtr.reset(); // Join the producer before inspecting its observation.
    CHECK(output == expected);
    // Includes preroll and a possible refill after the read. A fresh scan would decode 60 AUs.
    CHECK(observation.calls <= 18);
  }

  TEST_CASE("AacDecoderSession - a superseding seek uses only validated HE-AAC history", "[concurrency]")
  {
    auto decoderPtr = openHe();
    readPackets(*decoderPtr, 44);
    auto observation = DecodeObservation{};
    auto guard = observe(observation);

    SECTION("Cancellation between AUs retains exact progress for a backward replacement")
    {
      observation.stopAt = 12;
      auto const res = decoderPtr->seekAfterDiscard(milliseconds{2304}, observation.stop.get_token());
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
      CHECK(observation.calls == 12);
      CHECK_FALSE(decoderPtr->needsSeekWorker(milliseconds{1600}));
    }

    SECTION("A failed native handle cannot seed the backward replacement")
    {
      observation.failAt = 12;
      auto const res = decoderPtr->seekAfterDiscard(milliseconds{2304}, {});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::DecodeFailed);
      CHECK(decoderPtr->needsSeekWorker(milliseconds{1600}));
    }

    guard.reset();
    REQUIRE(decoderPtr->seekAfterDiscard(milliseconds{1600}, {}));
    auto referencePtr = openHe();
    readPackets(*referencePtr, 50);

    // After cancellation, packets 50–55 are cached. Continue past the first fresh decode at 56.
    for (std::size_t packet = 50; packet <= 60; ++packet)
    {
      CAPTURE(packet);
      auto const expected = ao::test::requireValue(referencePtr->readNextBlock());
      auto const block = ao::test::requireValue(decoderPtr->readNextBlock());
      CHECK(block.firstFrameIndex == packet * 1024);
      CHECK(std::ranges::equal(block.bytes, expected.bytes));
    }
  }

  TEST_CASE("AacDecoderSession - an interrupted direct forward scan retains the pending replay and borrow",
            "[concurrency]")
  {
    auto const expected = linearPacket(40);
    auto decoderPtr = openHe();
    readPackets(*decoderPtr, 44);
    REQUIRE(decoderPtr->seek(milliseconds{1248})); // Rewind to packet 39, still behind the live cursor.
    auto const borrowed = ao::test::requireValue(decoderPtr->readNextBlock());
    auto const saved = std::vector<std::byte>{borrowed.bytes.begin(), borrowed.bytes.end()};
    auto observation = DecodeObservation{};
    auto guard = observe(observation);

    SECTION("Cancellation between access units")
    {
      observation.stopAt = 12;
      auto const res = decoderPtr->seekInterruptibly(milliseconds{2304}, observation.stop.get_token());
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::Conflict);
    }

    SECTION("Failure after consuming native input")
    {
      observation.failAt = 12;
      auto const res = decoderPtr->seek(milliseconds{2304});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::DecodeFailed);
    }

    CHECK(std::ranges::equal(borrowed.bytes, saved));
    auto const next = ao::test::requireValue(decoderPtr->readNextBlock());
    CHECK(next.firstFrameIndex == 40 * 1024);
    CHECK(std::ranges::equal(next.bytes, expected));
  }
} // namespace ao::audio::test
