// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/detail/AacConfigParser.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::audio::detail::test
{
  namespace
  {
    class BitWriter final
    {
    public:
      void write(std::uint32_t value, std::uint8_t bitCount)
      {
        for (auto bit = bitCount; bit > 0; --bit)
        {
          auto const bitValue = (value >> (bit - 1U)) & 1U;

          if ((_bitOffset % 8U) == 0)
          {
            _bytes.push_back(std::byte{0});
          }

          auto& byte = _bytes.back();
          auto const shift = 7U - (_bitOffset % 8U);
          byte |= static_cast<std::byte>(bitValue << shift);
          ++_bitOffset;
        }
      }

      std::span<std::byte const> bytes() const noexcept { return _bytes; }

    private:
      std::vector<std::byte> _bytes;
      std::size_t _bitOffset = 0;
    };

    std::vector<std::byte> makeAsc(std::uint32_t objectType, std::uint32_t sampleRateIndex, std::uint32_t channelConfig)
    {
      auto writer = BitWriter{};
      writer.write(objectType, 5);
      writer.write(sampleRateIndex, 4);
      writer.write(channelConfig, 4);

      auto bytes = writer.bytes();
      return {bytes.begin(), bytes.end()};
    }

    std::vector<std::byte> makeExplicitRateAsc(std::uint32_t sampleRate, std::uint32_t channelConfig)
    {
      auto writer = BitWriter{};
      writer.write(2, 5);
      writer.write(15, 4);
      writer.write(sampleRate, 24);
      writer.write(channelConfig, 4);

      auto bytes = writer.bytes();
      return {bytes.begin(), bytes.end()};
    }

    std::vector<std::byte> makeEscapedObjectTypeAsc(std::uint32_t extension,
                                                    std::uint32_t sampleRateIndex,
                                                    std::uint32_t channelConfig)
    {
      auto writer = BitWriter{};
      writer.write(31, 5);
      writer.write(extension, 6);
      writer.write(sampleRateIndex, 4);
      writer.write(channelConfig, 4);

      auto bytes = writer.bytes();
      return {bytes.begin(), bytes.end()};
    }
  } // namespace

  TEST_CASE("AacConfigParser - parses AudioSpecificConfig", "[audio][unit][aac][config]")
  {
    SECTION("AAC LC with table sample rate")
    {
      auto const bytes = makeAsc(2, 4, 2);
      auto const config = parseAudioSpecificConfig(bytes);

      CHECK(config.sampleRate == 44100);
      CHECK(config.channels == 2);
    }

    SECTION("Explicit sample rate")
    {
      auto const bytes = makeExplicitRateAsc(12345, 6);
      auto const config = parseAudioSpecificConfig(bytes);

      CHECK(config.sampleRate == 12345);
      CHECK(config.channels == 6);
    }

    SECTION("Escaped object type")
    {
      auto const bytes = makeEscapedObjectTypeAsc(5, 3, 7);
      auto const config = parseAudioSpecificConfig(bytes);

      CHECK(config.sampleRate == 48000);
      CHECK(config.channels == 8);
    }

    SECTION("Invalid sample rate index keeps channel count")
    {
      auto const bytes = makeAsc(2, 13, 1);
      auto const config = parseAudioSpecificConfig(bytes);

      CHECK(config.sampleRate == 0);
      CHECK(config.channels == 1);
    }

    SECTION("Invalid channel config keeps sample rate")
    {
      auto const bytes = makeAsc(2, 4, 8);
      auto const config = parseAudioSpecificConfig(bytes);

      CHECK(config.sampleRate == 44100);
      CHECK(config.channels == 0);
    }
  }

  TEST_CASE("AacConfigParser - rejects incomplete AudioSpecificConfig", "[audio][unit][aac][config]")
  {
    SECTION("Empty input")
    {
      auto const config = parseAudioSpecificConfig({});

      CHECK(config.sampleRate == 0);
      CHECK(config.channels == 0);
    }

    SECTION("Escaped object type without full extension")
    {
      auto writer = BitWriter{};
      writer.write(31, 5);

      auto const config = parseAudioSpecificConfig(writer.bytes());

      CHECK(config.sampleRate == 0);
      CHECK(config.channels == 0);
    }

    SECTION("Object type without full sample rate index")
    {
      auto writer = BitWriter{};
      writer.write(2, 5);

      auto const config = parseAudioSpecificConfig(writer.bytes());

      CHECK(config.sampleRate == 0);
      CHECK(config.channels == 0);
    }

    SECTION("Missing explicit sample rate bits")
    {
      auto writer = BitWriter{};
      writer.write(2, 5);
      writer.write(15, 4);
      writer.write(42, 12);

      auto const config = parseAudioSpecificConfig(writer.bytes());

      CHECK(config.sampleRate == 0);
      CHECK(config.channels == 0);
    }
  }
} // namespace ao::audio::detail::test
