// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/FileManifestBuilder.h"

#include "ao/library/FileManifestLayout.h"
#include "ao/library/FileManifestView.h"

#include <catch2/catch_test_macros.hpp>

using namespace ao::library;

TEST_CASE("FileManifestBuilder constructs valid payload", "[library][manifest]")
{
  auto builder = FileManifestBuilder::createNew();
  builder.trackId(ao::TrackId{100}).fileSize(999999).mtime(888888).status(FileStatus::Error);

  auto payload = builder.serialize();

  // Validate via view
  auto view = FileManifestView{payload};

  REQUIRE(view.trackId() == ao::TrackId{100});
  REQUIRE(view.fileSize() == 999999);
  REQUIRE(view.mtime() == 888888);
  REQUIRE(view.status() == FileStatus::Error);
}

TEST_CASE("FileManifestBuilder constructs from view", "[library][manifest]")
{
  auto builder1 = FileManifestBuilder::createNew();
  builder1.trackId(ao::TrackId{123}).fileSize(111).mtime(222).status(FileStatus::Missing);

  auto payload1 = builder1.serialize();
  auto view = FileManifestView{payload1};

  auto builder2 = FileManifestBuilder::fromView(view);
  builder2.fileSize(333); // Modify one field

  auto payload2 = builder2.serialize();
  auto view2 = FileManifestView{payload2};

  REQUIRE(view2.trackId() == ao::TrackId{123});
  REQUIRE(view2.fileSize() == 333);
  REQUIRE(view2.mtime() == 222);
  REQUIRE(view2.status() == FileStatus::Missing);
}
