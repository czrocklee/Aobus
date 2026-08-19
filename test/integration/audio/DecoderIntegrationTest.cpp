// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/AlacDecoderSession.h"
#include "lib/audio/AudioTime.h"
#include "lib/audio/FlacDecoderSession.h"
#include "lib/audio/Mp3DecoderSession.h"
#include "lib/audio/OpusDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    /**
     * @brief Requires an integration audio fixture, or marks the section skipped.
     */
    std::filesystem::path requireAudioFixture(char const* fileName)
    {
      auto const path = std::filesystem::path{AUDIO_TEST_DATA_DIR} / fileName;

      if (!std::filesystem::exists(path))
      {
        SKIP("Required audio fixture missing: " << path);
      }

      return path;
    }

    /**
     * @brief Extracts a specific number of samples from a decoder runtime for verification.
     */
    template<typename T>
    std::vector<T> extractSamples(DecoderSession& decoder, std::size_t count)
    {
      auto const blockRes = decoder.readNextBlock();

      if (!blockRes || blockRes->bytes.empty())
      {
        return {};
      }

      auto const available =
        std::min<std::size_t>(count, static_cast<std::size_t>(blockRes->frames) * 2); // Assume Stereo
      auto const* data = reinterpret_cast<T const*>(blockRes->bytes.data());

      return {data, data + available};
    }

    /**
     * @brief Reads a fixture into memory so a test can patch its header bytes.
     */
    std::vector<std::uint8_t> readFileBytes(std::filesystem::path const& path)
    {
      auto stream = std::ifstream{path, std::ios::binary};
      REQUIRE(stream);
      return {std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    /**
     * @brief Root mean square of a 16-bit block, used as a stable measure of the
     * decoded signal level at one position.
     */
    double rootMeanSquare(PcmBlock const& block)
    {
      auto const samples = block.bytes.size() / sizeof(std::int16_t);

      if (samples == 0)
      {
        return 0.0;
      }

      double total = 0.0;

      for (std::size_t index = 0; index < samples; ++index)
      {
        std::int16_t sample = 0;
        std::memcpy(&sample, block.bytes.data() + (index * sizeof(sample)), sizeof(sample));
        total += static_cast<double>(sample) * sample;
      }

      return std::sqrt(total / static_cast<double>(samples));
    }

    /**
     * @brief Verifies that high bit-depth samples are correctly shifted (bit-perfect)
     * relative to their source bit-depth.
     */
    template<typename TSource, typename TTarget>
    void checkBitPerfectShift(std::span<TSource const> source, std::span<TTarget const> target, std::uint8_t shift)
    {
      REQUIRE(source.size() == target.size());

      for (std::size_t i = 0; i < source.size(); ++i)
      {
        if (source[i] != 0)
        {
          auto const expected = static_cast<TTarget>(source[i]) << shift;
          CHECK(target[i] == expected);
        }
      }
    }

    /**
     * @brief Helper for ALAC 24-bit unpacking from 3-byte packed PCM.
     */
    std::vector<std::int32_t> unpackS24(std::span<std::byte const> bytes)
    {
      auto samples = std::vector<std::int32_t>{};

      for (std::size_t i = 0; i + 2 < bytes.size(); i += 3)
      {
        std::int32_t val = static_cast<std::uint8_t>(bytes[i]) | (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                           (static_cast<std::uint8_t>(bytes[i + 2]) << 16);

        // Sign extend from 24 to 32 bits
        if ((val & 0x800000) != 0)
        {
          val |= static_cast<std::int32_t>(0xFF000000);
        }

        samples.push_back(val);
      }

      return samples;
    }

    std::uint64_t frameIndexAt(DecodedStreamInfo const& info, std::chrono::milliseconds offset)
    {
      return durationToSamples(offset, info.sourceFormat.sampleRate);
    }

    std::uint64_t totalFrames(DecodedStreamInfo const& info)
    {
      return durationToSamples(info.duration, info.sourceFormat.sampleRate);
    }

    void checkPcmBlockLayout(PcmBlock const& block, PcmFormat const& outputFormat)
    {
      auto const bytesPerFrame = frameBytes(outputFormat);

      REQUIRE(bytesPerFrame > 0);
      CHECK(block.bytes.size() == static_cast<std::size_t>(block.frames) * bytesPerFrame);
    }

    void checkBlockDoesNotRunPastStream(PcmBlock const& block, DecodedStreamInfo const& info)
    {
      if (auto const frameCount = totalFrames(info); frameCount > 0 && block.firstFrameIndex <= frameCount)
      {
        CHECK(block.frames <= frameCount - block.firstFrameIndex);
      }
    }

    void checkNearSeekFrame(PcmBlock const& block, std::uint64_t expectedFrame, std::uint32_t sampleRate)
    {
      auto const toleranceFrames = static_cast<std::uint64_t>(sampleRate / 20U);

      CHECK(block.firstFrameIndex + toleranceFrames >= expectedFrame);
      CHECK(block.firstFrameIndex <= expectedFrame + toleranceFrames);
    }
  } // namespace

  TEST_CASE("Decoder - bit-perfect conversions preserve PCM output", "[audio][integration][decoder]")
  {
    SECTION("FLAC: 16-bit to 32-bit padding alignment")
    {
      auto const testFile = requireAudioFixture("basic_metadata.flac");

      // 1. Acquire reference 16-bit samples
      auto samples16 = std::vector<std::int16_t>{};
      {
        auto decoderRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
        REQUIRE(decoderRes);
        auto& decoder = **decoderRes;
        CHECK(decoder.streamInfo().codec == AudioCodec::Flac);
        samples16 = extractSamples<std::int16_t>(decoder, 100);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto decoderRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed32Le);
        REQUIRE(decoderRes);
        auto& decoder = **decoderRes;
        samples32 = extractSamples<std::int32_t>(decoder, 100);
      }

      // 3. Verify shift (16 -> 32 should be 16-bit shift)
      checkBitPerfectShift<std::int16_t, std::int32_t>(samples16, samples32, 16);
    }

    SECTION("ALAC: 24-bit to 32-bit padding alignment")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      // 1. Acquire reference 24-bit (packed) samples
      auto samples24 = std::vector<std::int32_t>{};
      {
        auto decoderRes = AlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe);
        REQUIRE(decoderRes);
        auto& decoder = **decoderRes;
        CHECK(decoder.streamInfo().codec == AudioCodec::Alac);
        auto const blockRes = decoder.readNextBlock();

        REQUIRE(blockRes);
        samples24 = unpackS24(blockRes->bytes);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto decoderRes = AlacDecoderSession::open(testFile, SampleEncoding::Signed32Le);
        REQUIRE(decoderRes);
        auto& decoder = **decoderRes;
        samples32 = extractSamples<std::int32_t>(decoder, samples24.size());
      }

      // 3. Verify shift (24 -> 32 should be 8-bit shift)
      checkBitPerfectShift<std::int32_t, std::int32_t>(samples24, samples32, 8);
    }
  }

  TEST_CASE("FlacDecoder - fixture decodes with expected integrity", "[audio][integration][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    SECTION("Metadata Extraction")
    {
      auto decoderRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.outputFormat.sampleRate == 44100);
      CHECK(info.outputFormat.channels == 2);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
    }

    SECTION("Seek Consistency")
    {
      auto decoderRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();
      auto const seekOffset = std::chrono::milliseconds{100};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();

      REQUIRE(blockRes);
      REQUIRE_FALSE(blockRes->endOfStream);
      REQUIRE(blockRes->frames > 0);
      CHECK(blockRes->firstFrameIndex == expectedFrame);
      checkPcmBlockLayout(*blockRes, info.outputFormat);
      checkBlockDoesNotRunPastStream(*blockRes, info);
    }
  }

  TEST_CASE("AlacDecoder - fixture decodes with expected integrity", "[audio][integration][alac]")
  {
    auto const testFile = requireAudioFixture("hires.m4a");

    SECTION("Hires Metadata")
    {
      auto decoderRes = AlacDecoderSession::open(testFile, std::nullopt);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 96000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.precisionBits == 24);
    }
  }

  TEST_CASE("Mp3Decoder - fixture decodes with expected integrity", "[audio][integration][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    SECTION("Metadata Extraction")
    {
      auto decoderRes = Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.isLossy == true);
    }
  }

  TEST_CASE("OpusDecoder - fixture decodes with expected integrity", "[audio][integration][opus]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.opus");
    constexpr std::uint64_t kHalfwayFrame = 24000;

    auto openDecoder = [&testFile]
    {
      auto decoderRes = OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      return std::move(*decoderRes);
    };

    SECTION("Metadata Extraction")
    {
      auto const decoderPtr = openDecoder();
      auto const info = decoderPtr->streamInfo();

      CHECK(info.codec == AudioCodec::Opus);
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.isLossy == true);
      CHECK(info.duration == std::chrono::seconds{1});
    }

    SECTION("A seek lands on the same signal a sequential read reaches")
    {
      // Frame bookkeeping alone cannot show that the audio lines up, so this
      // compares the decoded energy at one position reached two ways. The
      // fixture is a constant-amplitude tone, which makes that comparison stable
      // despite the codec being lossy.
      auto sequentialPtr = openDecoder();
      double sequentialRms = 0.0;

      while (true)
      {
        auto const blockRes = sequentialPtr->readNextBlock();
        REQUIRE(blockRes);
        REQUIRE_FALSE(blockRes->bytes.empty());

        if (blockRes->firstFrameIndex + blockRes->frames > kHalfwayFrame)
        {
          sequentialRms = rootMeanSquare(*blockRes);
          break;
        }

        REQUIRE_FALSE(blockRes->endOfStream);
      }

      auto soughtPtr = openDecoder();
      REQUIRE(soughtPtr->seek(std::chrono::milliseconds{500}));

      auto const soughtBlockRes = soughtPtr->readNextBlock();
      REQUIRE(soughtBlockRes);
      CHECK(soughtBlockRes->firstFrameIndex == kHalfwayFrame);

      auto const soughtRms = rootMeanSquare(*soughtBlockRes);
      CHECK(sequentialRms > 0.0);
      CHECK(soughtRms > 0.0);
      CHECK(std::abs(soughtRms - sequentialRms) < sequentialRms * 0.2);
    }
  }

  TEST_CASE("OpusDecoder - a seek pre-rolls the decoder before its target", "[audio][integration][opus]")
  {
    // The default one-second pagination puts every mid-stream seek back at the
    // start of the audio, so the decoder is warm by the time it reaches the
    // target whether or not a pre-roll is applied. One packet per page is the
    // pagination live and remuxed streams carry, and it is the shape where a
    // restart lands close enough to the target to still be converging.
    auto const testFile = requireAudioFixture("short_pages.opus");
    constexpr std::uint64_t kSeekFrame = 24000;

    auto openDecoder = [&testFile]
    {
      auto decoderRes = OpusDecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      return std::move(*decoderRes);
    };

    // Every audible sample the stream holds, addressed by absolute frame, so a
    // block reached by seeking can be compared against the same frames reached
    // by playing from the beginning.
    auto const reference = [&openDecoder]
    {
      auto samples = std::vector<std::int16_t>{};
      auto decoderPtr = openDecoder();

      while (true)
      {
        auto const blockRes = decoderPtr->readNextBlock();
        REQUIRE(blockRes);

        auto const* const data = reinterpret_cast<std::int16_t const*>(blockRes->bytes.data());
        samples.insert(samples.end(), data, data + (blockRes->bytes.size() / sizeof(std::int16_t)));

        if (blockRes->endOfStream)
        {
          return samples;
        }
      }
    }();

    auto soughtPtr = openDecoder();
    REQUIRE(soughtPtr->seek(std::chrono::milliseconds{500}));

    auto const blockRes = soughtPtr->readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->firstFrameIndex == kSeekFrame);
    REQUIRE(blockRes->frames > 0);

    auto const channels = soughtPtr->streamInfo().sourceFormat.channels;
    auto const count = static_cast<std::size_t>(blockRes->frames) * channels;
    auto const offset = static_cast<std::size_t>(kSeekFrame) * channels;
    REQUIRE(offset + count <= reference.size());

    auto const* const sought = reinterpret_cast<std::int16_t const*>(blockRes->bytes.data());
    double error = 0.0;
    double energy = 0.0;

    for (std::size_t index = 0; index < count; ++index)
    {
      auto const expected = static_cast<double>(reference[offset + index]);
      error += std::abs(static_cast<double>(sought[index]) - expected);
      energy += std::abs(expected);
    }

    // A decoder resumed cold reproduces the level but not the waveform, so the
    // comparison has to be per sample. Opus does not converge exactly, hence a
    // tolerance rather than equality.
    REQUIRE(energy > 0.0);
    CHECK(error / energy < 0.1);
  }

  TEST_CASE("OpusDecoder - applies the identification packet output gain", "[audio][integration][opus]")
  {
    // No encoder writes a header gain by default, so the only way to prove the
    // field reaches libopus is to set it and measure the decoded level. -1536 in
    // Q7.8 decibels is -6 dB, which halves the amplitude.
    constexpr std::int16_t kMinusSixDecibels = -1536;

    auto const testFile = requireAudioFixture("basic_metadata.opus");
    auto source = readFileBytes(testFile);

    auto const magic = std::vector<std::uint8_t>{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    auto const found = std::ranges::search(source, magic).begin();
    REQUIRE(found != source.end());

    // The gain follows the magic signature, version, channel count, pre-skip,
    // and input sample rate.
    auto const gainOffset = static_cast<std::size_t>(std::distance(source.begin(), found)) + 16;
    REQUIRE(gainOffset + 1 < source.size());
    source[gainOffset] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(kMinusSixDecibels) & 0xFFU);
    source[gainOffset + 1] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(kMinusSixDecibels) >> 8U) & 0xFFU);

    auto const attenuated = ao::test::TempFile{source, ".opus"};

    auto const firstBlockRms = [](std::filesystem::path const& path)
    {
      auto decoderRes = OpusDecoderSession::open(path, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);

      auto const blockRes = (*decoderRes)->readNextBlock();
      REQUIRE(blockRes);
      REQUIRE_FALSE(blockRes->bytes.empty());
      return rootMeanSquare(*blockRes);
    };

    auto const plainRms = firstBlockRms(testFile);
    auto const attenuatedRms = firstBlockRms(attenuated.path);

    REQUIRE(plainRms > 0.0);
    CHECK(attenuatedRms < plainRms);
    CHECK(attenuatedRms / plainRms > 0.45);
    CHECK(attenuatedRms / plainRms < 0.56);
  }

  TEST_CASE("Decoder - malformed and unsupported inputs fail without crashing", "[audio][integration][decoder]")
  {
    SECTION("Corrupt: Opening a non-FLAC file as FLAC")
    {
      // Use this source file itself as a fake FLAC
      auto const testFile = std::filesystem::path{__FILE__};
      auto const resRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);

      CHECK_FALSE(resRes);
    }

    SECTION("MP3: Seek near EOF")
    {
      auto const testFile = requireAudioFixture("hires.mp3");
      auto decoderRes = Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("MP3 fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);

      if (blockRes->endOfStream)
      {
        CHECK(blockRes->frames == 0);
        CHECK(blockRes->bytes.empty());
      }
      else
      {
        REQUIRE(blockRes->frames > 0);
        checkNearSeekFrame(*blockRes, expectedFrame, info.sourceFormat.sampleRate);
        checkPcmBlockLayout(*blockRes, info.outputFormat);
        checkBlockDoesNotRunPastStream(*blockRes, info);
      }
    }

    SECTION("Seek near EOF")
    {
      auto const testFile = requireAudioFixture("basic_metadata.flac");
      auto decoderRes = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
      REQUIRE(decoderRes);
      auto& decoder = **decoderRes;

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("FLAC fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);

      if (blockRes->endOfStream && blockRes->frames == 0)
      {
        CHECK(blockRes->bytes.empty());
      }
      else
      {
        REQUIRE(blockRes->frames > 0);
        CHECK(blockRes->firstFrameIndex == expectedFrame);
        checkPcmBlockLayout(*blockRes, info.outputFormat);
        checkBlockDoesNotRunPastStream(*blockRes, info);
      }
    }
  }
} // namespace ao::audio::test
