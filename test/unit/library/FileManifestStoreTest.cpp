// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/library/FileManifestStore.h"

#include "ao/Exception.h"
#include "ao/library/FileManifestBuilder.h"
#include "ao/library/FileManifestLayout.h"
#include "ao/lmdb/Database.h"
#include "ao/lmdb/Environment.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <span>
#include <string>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("FileManifestStore - Invalid URI length throws", "[library][unit][manifest]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "manifests"};
    auto store = FileManifestStore{db};
    auto writer = store.writer(wtxn);

    // 4097 bytes, which is greater than kMaxUriLength (4096)
    auto const longUri = std::string(4097, 'a');

    REQUIRE_THROWS_AS(writer.put(longUri, std::span<std::byte const>{}), ao::Exception);
  }

  TEST_CASE("FileManifestStore - Write and read back manifest", "[library][unit][manifest]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "manifests"};
    auto store = FileManifestStore{db};

    auto builder = FileManifestBuilder::createNew();
    builder.trackId(TrackId{42}).fileSize(12345).mtime(67890).status(FileStatus::Available);
    auto const payload = builder.serialize();

    store.writer(wtxn).put("song.flac", payload);
    wtxn.commit();

    auto rtxn = ReadTransaction{env};
    auto const optView = store.reader(rtxn).get("song.flac");
    REQUIRE(optView.has_value());
    CHECK(optView->trackId() == TrackId{42});
    CHECK(optView->fileSize() == 12345);
    CHECK(optView->mtime() == 67890);
    CHECK(optView->status() == FileStatus::Available);
  }

  TEST_CASE("FileManifestStore - Get non-existent URI returns nullopt", "[library][unit][manifest]")
  {
    auto temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = WriteTransaction{env};
    auto db = Database{wtxn, "manifests"};
    auto store = FileManifestStore{db};
    wtxn.commit();

    auto rtxn = ReadTransaction{env};
    auto const optView = store.reader(rtxn).get("nonexistent.flac");
    CHECK_FALSE(optView.has_value());
  }

} // namespace ao::library::test
