// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

namespace ao::library::test
{
  class TrackSerializationFixture final
  {
  public:
    TrackSerializationFixture()
      : _library{_temp.path(), _temp.path() / "db"}, _transaction{writeTransaction(_library)}
    {
    }

    std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(TrackBuilder& builder)
    {
      auto result = builder.serialize(_transaction, _library.resources());
      REQUIRE(result);
      commitAndRenew();
      return *result;
    }

    Result<std::vector<std::byte>> trySerializeHot(TrackBuilder& builder)
    {
      auto result = builder.serializeHot(_transaction);

      if (result)
      {
        commitAndRenew();
      }

      return result;
    }

    Result<std::vector<std::byte>> trySerializeCold(TrackBuilder& builder)
    {
      auto result = builder.serializeCold(_transaction, _library.resources());

      if (result)
      {
        commitAndRenew();
      }

      return result;
    }

    std::vector<std::byte> serializeCold(TrackBuilder& builder)
    {
      auto result = trySerializeCold(builder);
      REQUIRE(result);
      return *result;
    }

    WriteTransaction& transaction() { return _transaction; }

    DictionaryStore const& dictionary() { return _library.dictionary(); }

    ResourceStore const& resources() { return _library.resources(); }

  private:
    void commitAndRenew()
    {
      REQUIRE(_transaction.commit());
      _transaction = writeTransaction(_library);
    }

    ao::test::TempDir _temp;
    MusicLibrary _library;
    WriteTransaction _transaction;
  };

  inline std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder)
  {
    auto context = TrackSerializationFixture{};
    return context.serialize(builder);
  }
} // namespace ao::library::test
