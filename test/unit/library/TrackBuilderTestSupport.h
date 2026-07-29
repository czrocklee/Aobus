// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::library::test
{
  class TrackSerializationFixture final
  {
  public:
    TrackSerializationFixture();

    std::pair<std::vector<std::byte>, std::vector<std::byte>> serialize(TrackBuilder& builder);
    Result<std::vector<std::byte>> trySerializeHot(TrackBuilder& builder);
    Result<std::vector<std::byte>> trySerializeCold(TrackBuilder& builder);
    std::vector<std::byte> serializeCold(TrackBuilder& builder);

    WriteTransaction& transaction();
    DictionaryStore const& dictionary();
    ResourceStore const& resources();

  private:
    void commitAndRenew();

    ao::test::TempDir _temp;
    MusicLibrary _library;
    WriteTransaction _transaction;
  };

  std::pair<std::vector<std::byte>, std::vector<std::byte>> serializeTestTrack(TrackBuilder& builder);
} // namespace ao::library::test
