// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestView.h>

#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace ao::library::test
{
  TEST_CASE("FileManifestView - properties", "[library][unit][manifest]")
  {
    auto const signature = utility::xxh3Hash128("view payload");
    auto header = FileManifestHeader{};

    header.trackId = TrackId{42};
    header.status = FileStatus::Missing;
    header.audioPayloadLength(1122334455667788ULL);
    header.audioSignature(signature);

    auto const buffer = std::as_bytes(std::span{&header, std::size_t{1}});
    auto view = FileManifestView{buffer};

    CHECK(view.trackId() == ao::TrackId{42});
    CHECK(view.status() == FileStatus::Missing);
    CHECK(view.audioPayloadLength() == 1122334455667788ULL);
    CHECK(view.audioSignature() == signature);
  }

  TEST_CASE("FileManifestView - returns file size and modification time", "[library][unit][manifest]")
  {
    auto header = FileManifestHeader{};
    header.fileSize(123456789012345ULL);
    header.mtime(FileTimestamp{.seconds = 987654321098765, .nanoseconds = 999999999});

    auto const buffer = std::as_bytes(std::span{&header, std::size_t{1}});
    auto const view = FileManifestView{buffer};

    REQUIRE(view.isValid());
    CHECK(view.fileSize() == 123456789012345ULL);
    auto const optMtime = view.mtime();
    REQUIRE(optMtime);
    CHECK(optMtime->seconds == 987654321098765);
    CHECK(optMtime->nanoseconds == 999999999);

    auto const zeroHeader = FileManifestHeader{};
    auto const zeroBuffer = std::as_bytes(std::span{&zeroHeader, std::size_t{1}});
    auto const zeroView = FileManifestView{zeroBuffer};

    REQUIRE(zeroView.isValid());
    CHECK(zeroView.fileSize() == 0);
    // A zeroed header is canonical absence, not a stored epoch zero.
    CHECK_FALSE(zeroView.mtime());
  }

  TEST_CASE("FileManifestView - reads epoch zero and negative instants with a fraction", "[library][unit][manifest]")
  {
    auto epochZeroHeader = FileManifestHeader{};
    epochZeroHeader.mtime(FileTimestamp{});
    auto const epochZeroBuffer = std::as_bytes(std::span{&epochZeroHeader, std::size_t{1}});
    auto const epochZeroView = FileManifestView{epochZeroBuffer};

    REQUIRE(epochZeroView.isValid());
    auto const optEpochZero = epochZeroView.mtime();
    REQUIRE(optEpochZero);
    CHECK(*optEpochZero == FileTimestamp{.seconds = 0, .nanoseconds = 0});

    auto negativeHeader = FileManifestHeader{};
    negativeHeader.mtime(FileTimestamp{.seconds = -11644473600, .nanoseconds = 123456789});
    auto const negativeBuffer = std::as_bytes(std::span{&negativeHeader, std::size_t{1}});
    auto const negativeView = FileManifestView{negativeBuffer};

    REQUIRE(negativeView.isValid());
    auto const optNegative = negativeView.mtime();
    REQUIRE(optNegative);
    CHECK(optNegative->seconds == -11644473600);
    CHECK(optNegative->nanoseconds == 123456789);
  }

  TEST_CASE("FileManifestView - poisons a small buffer", "[library][unit][manifest]")
  {
    auto buffer = std::array<std::byte, kFileManifestHeaderSize - 1>{};

    auto const view = FileManifestView{buffer};
    CHECK_FALSE(view.isValid());
    CHECK(view.trackId() == kInvalidTrackId);
    CHECK(view.fileSize() == 0);
    CHECK_FALSE(view.mtime());
    CHECK(view.status() == FileStatus::Available);
  }
} // namespace ao::library::test
