// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlacFrameTestSupport.h"

#include "test/media/mp4/TestAtoms.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Authors uncompressed ALAC elements independently of the production
    // decoder, an encoder, or a production channel-order table.
    class AlacPacketBitWriter final
    {
    public:
      void write(std::uint32_t value, std::uint8_t bitCount)
      {
        for (auto bit = bitCount; bit > 0; --bit)
        {
          if ((_bitOffset % 8U) == 0U)
          {
            _bytes.push_back(0);
          }

          auto const shift = 7U - (_bitOffset % 8U);
          _bytes.back() |= static_cast<std::uint8_t>(((value >> (bit - 1U)) & 1U) << shift);
          ++_bitOffset;
        }
      }

      std::vector<std::uint8_t> const& bytes() const noexcept { return _bytes; }

    private:
      std::vector<std::uint8_t> _bytes;
      std::size_t _bitOffset = 0;
    };

    void writeElementHeader(AlacPacketBitWriter& writer, bool pair, std::uint8_t instanceTag)
    {
      writer.write(pair ? 1U : 0U, 3);
      writer.write(instanceTag, 4);
      writer.write(0, 12);
      writer.write(0b1001, 4);
      writer.write(16, 32);
    }
  } // namespace

  std::vector<std::uint8_t> makeAlacChannelLayoutAtom(std::uint32_t tag,
                                                      std::uint32_t bitmap,
                                                      std::span<std::uint32_t const> labels)
  {
    REQUIRE(labels.size() <= std::numeric_limits<std::uint32_t>::max());
    auto body = std::vector<std::uint8_t>{};
    ao::test::mp4::appendBe32(body, 0);
    ao::test::mp4::appendBe32(body, tag);
    ao::test::mp4::appendBe32(body, bitmap);
    ao::test::mp4::appendBe32(body, static_cast<std::uint32_t>(labels.size()));

    for (auto const label : labels)
    {
      ao::test::mp4::appendBe32(body, label);
      ao::test::mp4::appendBe32(body, 0);
      ao::test::mp4::appendBe32(body, 0);
      ao::test::mp4::appendBe32(body, 0);
      ao::test::mp4::appendBe32(body, 0);
    }

    return ao::test::mp4::makeAtom("chan", body);
  }

  std::vector<std::uint8_t> buildAlacUncompressedFileBytes(std::uint8_t channels)
  {
    return buildAlacUncompressedFileBytes(AlacFrameTestOptions{.channels = channels});
  }

  std::vector<std::uint8_t> buildAlacUncompressedFileBytes(AlacFrameTestOptions const& options)
  {
    REQUIRE((options.channels >= 1U && options.channels <= 8U));
    REQUIRE((options.packetCount >= 1U && options.packetCount <= 4U));
    REQUIRE((options.precisionBits == 16U || options.precisionBits == 20U || options.precisionBits == 24U ||
             options.precisionBits == 32U));
    auto markers = std::to_array<std::int32_t>({0x5152, 0x6364, 0x7576, -0x1718, 0x292A, -0x3B3C, 0x4D4E, -0x5F60});

    switch (options.precisionBits)
    {
      case 20:
        markers =
          std::to_array<std::int32_t>({0x51521, 0x63642, 0x75763, -0x17184, 0x292A5, -0x3B3C6, 0x4D4E7, -0x5F608});
        break;
      case 24:
        markers = std::to_array<std::int32_t>(
          {0x515201, 0x636402, 0x757603, -0x171804, 0x292A05, -0x3B3C06, 0x4D4E07, -0x5F6008});
        break;
      case 32:
        markers = std::to_array<std::int32_t>(
          {0x51520001, 0x63640002, 0x75760003, -0x17180004, 0x292A0005, -0x3B3C0006, 0x4D4E0007, -0x5F600008});
        break;
      default: break;
    }

    auto writer = AlacPacketBitWriter{};

    // Sequential CPEs followed by an SCE for an odd final slot; mono is SCE.
    // These are arrival-order slot markers, not speaker-role declarations.
    for (std::uint8_t slot = 0; slot < options.channels; slot += 2)
    {
      bool const pair = slot + 1U < options.channels;
      // Distinct instance tags within the CPE kind; the final SCE has tag zero.
      // Tags identify elements, not speaker roles.
      writeElementHeader(writer, pair, pair ? static_cast<std::uint8_t>(slot / 2U) : std::uint8_t{0});

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        writer.write(static_cast<std::uint32_t>(markers[slot]), options.precisionBits);

        if (pair)
        {
          writer.write(static_cast<std::uint32_t>(markers[slot + 1U]), options.precisionBits);
        }
      }
    }

    writer.write(7, 3);
    auto const& packet = writer.bytes();

    if (options.channels == 2U && options.precisionBits == 16U)
    {
      REQUIRE(packet.size() == 72U);
    }

    auto cookie = std::vector<std::uint8_t>{0,  0,  0,  36, 'a', 'l', 'a', 'c', 0, 0,  0, 0, 0, 0, 0, 16, 0,    16,
                                            40, 10, 14, 2,  0,   255, 0,   0,   0, 72, 0, 0, 0, 0, 0, 0,  0xBB, 0x80};
    cookie[17] = options.precisionBits;
    cookie[21] = options.channels;
    cookie.insert(cookie.end(), options.cookieSuffix.begin(), options.cookieSuffix.end());
    REQUIRE(cookie.size() <= std::numeric_limits<std::uint32_t>::max());
    auto const cookieSize = static_cast<std::uint32_t>(cookie.size());
    auto const packetSize = static_cast<std::uint32_t>(packet.size());

    for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex)
    {
      auto const shift = static_cast<std::uint32_t>((3U - byteIndex) * 8U);
      cookie[byteIndex] = static_cast<std::uint8_t>(cookieSize >> shift);
      cookie[24U + byteIndex] = static_cast<std::uint8_t>(packetSize >> shift);
    }

    auto extensions = options.sampleEntryPrefix;
    extensions.insert(extensions.end(), cookie.begin(), cookie.end());
    extensions.insert(extensions.end(), options.sampleEntrySuffix.begin(), options.sampleEntrySuffix.end());
    auto payload = std::vector<std::uint8_t>{};

    for (std::uint32_t packetIndex = 0; packetIndex < options.packetCount; ++packetIndex)
    {
      payload.insert(payload.end(), packet.begin(), packet.end());
    }

    auto const stsd = ao::test::mp4::makeStsdAtom("alac", extensions, 48000, options.channels, options.precisionBits);
    auto const stsz = ao::test::mp4::makeStszAtom(packetSize, options.packetCount);
    auto const stts = ao::test::mp4::makeSttsAtom(options.packetCount, 16);
    auto const stsc = ao::test::mp4::makeStscAtom(options.packetCount);
    auto const stco = ao::test::mp4::makeStcoAtom(8);
    auto tableBody = std::vector<std::uint8_t>{};
    tableBody.insert(tableBody.end(), stsd.begin(), stsd.end());
    tableBody.insert(tableBody.end(), stsz.begin(), stsz.end());
    tableBody.insert(tableBody.end(), stts.begin(), stts.end());
    tableBody.insert(tableBody.end(), stsc.begin(), stsc.end());
    tableBody.insert(tableBody.end(), stco.begin(), stco.end());
    auto const table = ao::test::mp4::makeAtom("stbl", tableBody);
    auto const track = ao::test::mp4::makeTrackAtom("soun", table, 48000, 16U * options.packetCount);
    auto bytes = std::vector<std::uint8_t>{};
    ao::test::mp4::addAtom(bytes, "mdat", payload);
    ao::test::mp4::addAtom(bytes, "moov", track);
    return bytes;
  }
} // namespace ao::audio::test
