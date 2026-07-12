// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::library::test
{
  class TrackSerializationFixture final
  {
  public:
    TrackSerializationFixture()
      : _env{lmdb::test::openEnvironment(_temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20})}
      , _transaction{lmdb::test::beginWriteTransaction(_env)}
      , _dictionary{lmdb::test::openDatabase(_transaction, "dictionary"), _transaction}
      , _resources{lmdb::test::openDatabase(_transaction, "resources")}
    {
    }

    std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(TrackBuilder& builder)
    {
      auto result = builder.serialize(_transaction, _dictionary, _resources);
      REQUIRE(result);
      return *result;
    }

    Result<std::vector<std::byte>> trySerializeHot(TrackBuilder& builder)
    {
      return builder.serializeHot(_transaction, _dictionary);
    }

    Result<std::vector<std::byte>> trySerializeCold(TrackBuilder& builder)
    {
      return builder.serializeCold(_transaction, _dictionary, _resources);
    }

    std::vector<std::byte> serializeCold(TrackBuilder& builder)
    {
      auto result = trySerializeCold(builder);
      REQUIRE(result);
      return *result;
    }

    lmdb::WriteTransaction& transaction() { return _transaction; }

    DictionaryStore& dictionary() { return _dictionary; }

    ResourceStore& resources() { return _resources; }

  private:
    ao::test::TempDir _temp;
    lmdb::Environment _env;
    lmdb::WriteTransaction _transaction;
    DictionaryStore _dictionary;
    ResourceStore _resources;
  };

  inline std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder)
  {
    auto context = TrackSerializationFixture{};
    return context.serialize(builder);
  }
} // namespace ao::library::test
