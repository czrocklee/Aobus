// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestView.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("FileManifestView - properties", "[library][unit][manifest]")
  {
    // 24 bytes buffer
    auto buffer = std::array<std::byte, 24>{};
    auto* header = reinterpret_cast<FileManifestHeader*>(buffer.data());

    header->trackId = TrackId{42};
    header->status = FileStatus::Missing;
    header->fileSize(123456789012345ULL);
    header->mtime(987654321098765ULL);

    auto view = FileManifestView{buffer};

    CHECK(view.trackId() == ao::TrackId{42});
    CHECK(view.status() == FileStatus::Missing);
    CHECK(view.fileSize() == 123456789012345ULL);
    CHECK(view.mtime() == 987654321098765ULL);
  }

  TEST_CASE("FileManifestView - throws on small buffer", "[library][unit][manifest]")
  {
    auto buffer = std::array<std::byte, 10>{};

    REQUIRE_THROWS(FileManifestView{buffer});
  }
} // namespace ao::library::test
