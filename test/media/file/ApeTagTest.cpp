// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/media/file/mpeg/ApeTag.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::media::file::mpeg::test
{
  namespace
  {
    constexpr std::uint32_t kContainsHeaderFlag = 0x80000000U;
    constexpr std::size_t kItemBytes = 40;

    void appendU32le(std::vector<std::byte>& out, std::uint32_t value)
    {
      for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
      {
        out.push_back(static_cast<std::byte>(value >> shift));
      }
    }

    void appendBytes(std::vector<std::byte>& out, std::size_t count, std::byte value)
    {
      out.insert(out.end(), count, value);
    }

    // Appends one 32-byte APE header/footer block with the given descriptor
    // fields and zeroed reserved bytes.
    void appendBlock(std::vector<std::byte>& out,
                     std::uint32_t version,
                     std::uint32_t tagSize,
                     std::uint32_t itemCount,
                     std::uint32_t flags)
    {
      auto const magic = std::string_view{"APETAGEX"};

      for (auto const ch : magic)
      {
        out.push_back(static_cast<std::byte>(ch));
      }

      appendU32le(out, version);
      appendU32le(out, tagSize);
      appendU32le(out, itemCount);
      appendU32le(out, flags);
      appendBytes(out, 8, std::byte{0});
    }

    // Footer-only tag: `prefix` bytes of candidate audio, item data, footer.
    std::vector<std::byte> footerOnlyTag(std::size_t prefixSize, std::uint32_t tagSize, std::uint32_t flags = 0)
    {
      auto bytes = std::vector<std::byte>{};
      appendBytes(bytes, prefixSize, std::byte{0x5A});
      appendBytes(bytes, kItemBytes, std::byte{0x11});
      appendBlock(bytes, 2000, tagSize, 1, flags);
      return bytes;
    }
  } // namespace

  TEST_CASE("readApeTagStart - locates the tag start from a footer at the end of the span")
  {
    SECTION("Footer-only tag keeps the prefix outside the tag")
    {
      auto const bytes = footerOnlyTag(16, kItemBytes + 32);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE(startRes);
      CHECK(*startRes == 16);
    }

    SECTION("Header and footer tag keeps the prefix outside the tag")
    {
      auto bytes = std::vector<std::byte>{};
      appendBytes(bytes, 12, std::byte{0x5A});
      appendBlock(bytes, 2000, kItemBytes + 32, 1, 0);
      appendBytes(bytes, kItemBytes, std::byte{0x11});
      appendBlock(bytes, 2000, kItemBytes + 32, 1, kContainsHeaderFlag);

      auto const startRes = readApeTagStart(bytes);
      REQUIRE(startRes);
      CHECK(*startRes == 12);
    }

    SECTION("Tag size exactly covering the span starts at zero")
    {
      auto const bytes = footerOnlyTag(0, kItemBytes + 32);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE(startRes);
      CHECK(*startRes == 0);
    }

    SECTION("Minimum footer-only tag size is accepted")
    {
      auto bytes = std::vector<std::byte>{};
      appendBlock(bytes, 2000, 32, 0, 0);

      auto const startRes = readApeTagStart(bytes);
      REQUIRE(startRes);
      CHECK(*startRes == 0);
    }
  }

  TEST_CASE("readApeTagStart - rejects footer geometry outside the span")
  {
    SECTION("Tag size below the footer size")
    {
      auto const bytes = footerOnlyTag(16, 31);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "invalid apev2 trailing tag size");
    }

    SECTION("Tag size beyond the span")
    {
      auto const bytes = footerOnlyTag(16, 88 + 1);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "invalid apev2 trailing tag size");
    }

    SECTION("Span shorter than a footer")
    {
      auto const bytes = std::vector<std::byte>(16, std::byte{0x00});
      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "missing ape trailing tag footer");
    }

    SECTION("Missing footer magic")
    {
      auto bytes = footerOnlyTag(16, kItemBytes + 32);
      bytes[bytes.size() - 32] = std::byte{0x58};

      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "missing ape trailing tag footer");
    }

    SECTION("Header flag set but no room for a header")
    {
      auto const bytes = footerOnlyTag(0, kItemBytes + 32, kContainsHeaderFlag);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "invalid apev2 trailing tag header size");
    }

    SECTION("Header flag set but header magic missing")
    {
      // tagSize 32 leaves the expected header inside the item data, which does
      // not start with the magic.
      auto const bytes = footerOnlyTag(0, 32, kContainsHeaderFlag);
      auto const startRes = readApeTagStart(bytes);
      REQUIRE_FALSE(startRes);
      CHECK(startRes.error().code == Error::Code::CorruptData);
      CHECK(startRes.error().message == "invalid apev2 trailing tag header");
    }
  }

  TEST_CASE("readApeTagStart - accepts non-geometric descriptor fields when the geometry is valid")
  {
    // Every field outside the geometry contract is deliberately invalid or
    // mismatched: raw version 1000, differing header/footer versions, tag
    // sizes, item counts and flags, unknown flag bits, and nonzero reserved
    // bytes. A strict APE decoder owns refusing these.
    auto bytes = std::vector<std::byte>{};
    appendBytes(bytes, 4, std::byte{0x5A});
    appendBlock(bytes, 2000, 999, 7, 0x0000FFFF);
    appendBytes(bytes, 24, std::byte{0x11});
    appendBlock(bytes, 1000, 24 + 32, 3, kContainsHeaderFlag | 0x0000FFFF);
    bytes[4 + 24] = std::byte{0x7F};  // header reserved bytes
    bytes[60 + 24] = std::byte{0x7F}; // footer reserved bytes

    auto const startRes = readApeTagStart(bytes);
    REQUIRE(startRes);
    CHECK(*startRes == 4);
  }
} // namespace ao::media::file::mpeg::test
