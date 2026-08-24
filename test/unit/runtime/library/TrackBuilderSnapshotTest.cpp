// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <runtime/library/TrackBuilderSnapshot.h>

#include <ao/PictureType.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ao::rt::test
{
  TEST_CASE("TrackBuilderSnapshot - borrowed cover bytes become an observed descriptor",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto const coverBytes = std::array{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    auto builder = library::TrackBuilder::makeEmpty();
    builder.coverArt().add(PictureType::FrontCover, std::span<std::byte const>{coverBytes});

    auto snapshotRes = TrackBuilderSnapshot::make(builder);
    REQUIRE(snapshotRes);
    auto rebuilt = snapshotRes->makeBuilder();
    REQUIRE(rebuilt.coverArt().entries().size() == 1);
    auto const* const observed =
      std::get_if<library::ObservedResourceDescriptor>(&rebuilt.coverArt().entries().front().source);

    REQUIRE(observed);
    CHECK(observed->descriptor.digest == utility::computeSha256(coverBytes));
    CHECK(observed->descriptor.byteLength == coverBytes.size());
  }

  TEST_CASE("TrackBuilderSnapshot - observed cover evidence survives the owning snapshot",
            "[runtime][unit][track-builder-snapshot]")
  {
    auto const coverBytes = std::array{std::byte{0x9A}, std::byte{0xBC}};
    auto const expected = library::ObservedResourceDescriptor{
      .descriptor =
        library::ResourceDescriptor{
          .digest = utility::computeSha256(coverBytes),
          .byteLength = static_cast<std::uint32_t>(coverBytes.size()),
        },
    };
    auto builder = library::TrackBuilder::makeEmpty();
    builder.coverArt().add(PictureType::BackCover, expected);

    auto snapshotRes = TrackBuilderSnapshot::make(builder);
    REQUIRE(snapshotRes);
    auto rebuilt = snapshotRes->makeBuilder();
    REQUIRE(rebuilt.coverArt().entries().size() == 1);
    auto const& rebuiltCover = rebuilt.coverArt().entries().front();
    auto const* const observed = std::get_if<library::ObservedResourceDescriptor>(&rebuiltCover.source);

    REQUIRE(observed);
    CHECK(rebuiltCover.type == PictureType::BackCover);
    CHECK(observed->descriptor.digest == expected.descriptor.digest);
    CHECK(observed->descriptor.byteLength == expected.descriptor.byteLength);
  }
} // namespace ao::rt::test
