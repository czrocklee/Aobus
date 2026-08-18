// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ResourceLayout.h>

#include <ao/CoreIds.h>
#include <ao/library/TrackLayout.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace ao::library::test
{
  namespace
  {
    utility::Sha256Digest digestWithLeadingBytes(std::array<std::byte, 4> const& leading)
    {
      auto digest = utility::Sha256Digest{};

      for (std::size_t index = 0; index < leading.size(); ++index)
      {
        digest.bytes[index] = leading[index];
      }

      return digest;
    }
  } // namespace

  TEST_CASE("ResourceDescriptor - is a 36-byte row with no padding", "[library][unit][resource]")
  {
    STATIC_REQUIRE(sizeof(ResourceDescriptor) == kResourceDescriptorSize);
    STATIC_REQUIRE(alignof(ResourceDescriptor) == kResourceDescriptorAlignment);
    STATIC_REQUIRE(kResourceDescriptorSize == utility::Sha256Digest::kByteCount + sizeof(std::uint32_t));
  }

  TEST_CASE("CoverArtEntry - the track record is untouched by externalization", "[library][unit][resource]")
  {
    STATIC_REQUIRE(sizeof(CoverArtEntry) == 8);
    STATIC_REQUIRE(alignof(CoverArtEntry) == 4);
  }

  TEST_CASE("deriveResourceId - reads the digest's first four bytes big-endian", "[library][unit][resource]")
  {
    auto const digest = digestWithLeadingBytes({std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}});

    // Pinned spelling: the same stored row must resolve to the same handle on
    // every platform and for every future reader.
    CHECK(deriveResourceId(digest).raw() == 0x12345678U);
  }

  TEST_CASE("deriveResourceId - normalizes a zero result to one", "[library][unit][resource]")
  {
    auto const zeroLeading =
      digestWithLeadingBytes({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});

    CHECK(deriveResourceId(zeroLeading) == ResourceId{1});
    CHECK(deriveResourceId(zeroLeading) != kInvalidResourceId);

    // Only an all-zero prefix is normalized; a digest that genuinely begins with
    // one keeps it.
    auto const oneLeading =
      digestWithLeadingBytes({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}});
    CHECK(deriveResourceId(oneLeading) == ResourceId{1});
  }

  TEST_CASE("ResourceDescriptor - the persisted row is the record itself, digest first", "[library][unit][resource]")
  {
    auto const descriptor = ResourceDescriptor{
      .digest = digestWithLeadingBytes({std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}, std::byte{0x01}}),
      .byteLength = 86'956,
    };
    auto const row = utility::bytes::view(descriptor);

    // The digest is placed verbatim, in the order SHA-256 produced it. That is
    // worth checking on its own: `deriveResourceId` reads the first four bytes
    // of the digest, so a row that reversed or relocated them would still round
    // trip here while handing every existing database a different id.
    REQUIRE(row.size() == kResourceDescriptorSize);
    CHECK(row[0] == std::byte{0xAB});
    CHECK(row[3] == std::byte{0x01});

    // The length follows the digest rather than sharing its space or trailing
    // padding. Its bytes are the machine's own representation, so this reads
    // them back the way the parse does instead of naming an order the code no
    // longer chooses.
    std::uint32_t lengthField = 0;
    std::memcpy(&lengthField, row.data() + utility::Sha256Digest::kByteCount, sizeof(lengthField));
    CHECK(lengthField == descriptor.byteLength);

    auto const optParsed = parseResourceDescriptor(row);
    REQUIRE(optParsed);
    CHECK(optParsed->digest == descriptor.digest);
    CHECK(optParsed->byteLength == descriptor.byteLength);
  }

  TEST_CASE("ResourceDescriptor - the row carries the whole 32-bit length range", "[library][unit][resource]")
  {
    auto const descriptor = ResourceDescriptor{.digest = {}, .byteLength = std::numeric_limits<std::uint32_t>::max()};
    auto const optParsed = parseResourceDescriptor(utility::bytes::view(descriptor));

    REQUIRE(optParsed);
    CHECK(optParsed->byteLength == std::numeric_limits<std::uint32_t>::max());
  }

  TEST_CASE("resourceByteLengthFits - refuses content the length field cannot describe", "[library][unit][resource]")
  {
    constexpr std::size_t kMaximum = std::numeric_limits<std::uint32_t>::max();

    STATIC_REQUIRE(resourceByteLengthFits(0));
    STATIC_REQUIRE(resourceByteLengthFits(kMaximum));

    // The store refuses this rather than truncating it, which is why the check is
    // a predicate over a length: the size is unreachable through the byte-taking
    // entry point, since a span for it would claim a range that does not exist.
    STATIC_REQUIRE_FALSE(resourceByteLengthFits(kMaximum + 1U));
  }

  TEST_CASE("parseResourceDescriptor - refuses anything that is not a descriptor's width", "[library][unit][resource]")
  {
    auto const descriptor = ResourceDescriptor{};
    auto const row = utility::bytes::view(descriptor);

    CHECK_FALSE(parseResourceDescriptor(row.first(kResourceDescriptorSize - 1)));
    CHECK_FALSE(parseResourceDescriptor({}));

    auto wide = std::array<std::byte, kResourceDescriptorSize + 1>{};
    CHECK_FALSE(parseResourceDescriptor(wide));
  }
} // namespace ao::library::test
