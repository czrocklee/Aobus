// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/FileManifestView.h"

#include "ao/library/FileManifestLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace ao::library;

TEST_CASE("FileManifestView properties", "[library][manifest]")
{
  // 24 bytes buffer
  auto buffer = std::array<std::byte, 24>{};
  auto* header = reinterpret_cast<FileManifestHeader*>(buffer.data());

  header->trackId = ao::TrackId{42};
  header->status = FileStatus::Missing;
  header->fileSize(123456789012345ULL);
  header->mtime(987654321098765ULL);

  auto view = FileManifestView{buffer};

  REQUIRE(view.trackId() == ao::TrackId{42});
  REQUIRE(view.status() == FileStatus::Missing);
  REQUIRE(view.fileSize() == 123456789012345ULL);
  REQUIRE(view.mtime() == 987654321098765ULL);
}

TEST_CASE("FileManifestView throws on small buffer", "[library][manifest]")
{
  auto buffer = std::array<std::byte, 10>{};

  REQUIRE_THROWS(FileManifestView{buffer});
}
