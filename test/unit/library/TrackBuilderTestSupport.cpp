// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackBuilderTestSupport.h"

#include "MusicLibraryTestSupport.h"
#include "WritableLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/WriteTransaction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::library::test
{
  TrackSerializationFixture::TrackSerializationFixture()
    : _library{makeTestMusicLibrary(_temp.path(), _temp.path() / "db")}, _transaction{writeTransaction(_library)}
  {
  }

  std::pair<std::vector<std::byte>, std::vector<std::byte>> TrackSerializationFixture::serialize(TrackBuilder& builder)
  {
    auto result = physicalSerializeTrack(builder, _transaction, _library.resources());
    REQUIRE(result);
    commitAndRenew();
    return *result;
  }

  Result<std::vector<std::byte>> TrackSerializationFixture::trySerializeHot(TrackBuilder& builder)
  {
    auto result = physicalSerializeHotTrack(builder, _transaction);

    if (result)
    {
      commitAndRenew();
    }

    return result;
  }

  Result<std::vector<std::byte>> TrackSerializationFixture::trySerializeCold(TrackBuilder& builder)
  {
    auto result = physicalSerializeColdTrack(builder, _transaction, _library.resources());

    if (result)
    {
      commitAndRenew();
    }

    return result;
  }

  std::vector<std::byte> TrackSerializationFixture::serializeCold(TrackBuilder& builder)
  {
    auto result = trySerializeCold(builder);
    REQUIRE(result);
    return *result;
  }

  WriteTransaction& TrackSerializationFixture::transaction()
  {
    return _transaction;
  }

  DictionaryStore const& TrackSerializationFixture::dictionary()
  {
    return _library.dictionary();
  }

  ResourceStore const& TrackSerializationFixture::resources()
  {
    return _library.resources();
  }

  void TrackSerializationFixture::commitAndRenew()
  {
    REQUIRE(_transaction.commit());
    _transaction = writeTransaction(_library);
  }

  std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder)
  {
    auto context = TrackSerializationFixture{};
    return context.serialize(builder);
  }
} // namespace ao::library::test
