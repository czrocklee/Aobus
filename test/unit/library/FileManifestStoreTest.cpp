// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/library/FileManifestStore.h"

#include "ao/Exception.h"
#include "ao/lmdb/Database.h"
#include "ao/lmdb/Environment.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <span>
#include <string>

using namespace ao::library;
using namespace ao::lmdb;
using namespace ao::lmdb::test;

TEST_CASE("FileManifestStore - Invalid URI length throws", "[library][manifest]")
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
