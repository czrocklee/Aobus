// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestView.h>

#include <ao/CoreIds.h>
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
    header.mtime(987654321098765ULL);

    auto const buffer = std::as_bytes(std::span{&header, std::size_t{1}});
    auto const view = FileManifestView{buffer};

    REQUIRE(view.isValid());
    CHECK(view.fileSize() == 123456789012345ULL);
    CHECK(view.mtime() == 987654321098765ULL);

    auto const zeroHeader = FileManifestHeader{};
    auto const zeroBuffer = std::as_bytes(std::span{&zeroHeader, std::size_t{1}});
    auto const zeroView = FileManifestView{zeroBuffer};

    REQUIRE(zeroView.isValid());
    CHECK(zeroView.fileSize() == 0);
    CHECK(zeroView.mtime() == 0);
  }

  TEST_CASE("FileManifestView - poisons a small buffer", "[library][unit][manifest]")
  {
    auto buffer = std::array<std::byte, kFileManifestHeaderSize - 1>{};

    auto const view = FileManifestView{buffer};
    CHECK_FALSE(view.isValid());
    CHECK(view.trackId() == kInvalidTrackId);
    CHECK(view.fileSize() == 0);
    CHECK(view.status() == FileStatus::Available);
  }
} // namespace ao::library::test
