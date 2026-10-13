// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ao::lmdb
{
  class WriteTransaction;
}

namespace ao::library
{
  class TrackBuilder;
  class WriteTransaction;
}

namespace ao::library::test
{
  constexpr std::size_t alignToWord(std::size_t size) noexcept
  {
    return ((size + 3U) / 4U) * 4U;
  }

  struct TrackStoreFixture final
  {
    ao::test::TempDir temp;
    MusicLibrary library;
    TrackStore const& store;

    TrackStoreFixture();
  };

  /**
   * Canonical raw record bytes for direct LMDB seeding.
   *
   * TrackStore no longer accepts caller-supplied record bytes, so these exist
   * only for tests that plant rows in "tracks_hot"/"tracks_cold" behind the
   * store, which is how persisted-corruption cases reach MusicLibrary::open().
   * Tests that just need a track to exist go through TrackBuilder instead.
   */
  std::vector<std::byte> makeHotData(TrackHotHeader header = {}, std::string_view title = {});
  std::vector<std::byte> makeColdData(TrackColdHeader header = {}, std::string_view uri = "track.flac");

  /** Creates an empty library so seeded rows are swept against a real metadata header. */
  void initializeLibraryStorage(std::filesystem::path const& path);

  /**
   * Plants raw Track rows straight into "tracks_hot"/"tracks_cold", behind the
   * store, on a library that is not open. An empty span omits that side, which
   * is how orphan pairs are produced. Both databases are always created so an
   * omitted side reads as empty rather than missing. Each hot row also gets a
   * canonical manifest binding at uri, which must match the cold record's URI
   * when that side is present. This keeps Track/manifest cardinality valid.
   *
   * Call initializeLibraryStorage first: without a metadata header the sweep
   * rejects the library before it ever validates a Track record.
   */
  void seedRawTrackRow(std::filesystem::path const& path,
                       std::uint32_t rawTrackId,
                       std::string_view uri,
                       std::span<std::byte const> hotData,
                       std::span<std::byte const> coldData);

  /**
   * Raises the raw id space record so it covers Track and List ids up to the
   * given values, as a supported writer's commit does for every row it stores.
   * Raw seeding calls it so a planted row is admitted or rejected on its own
   * merits rather than for lying above the id space.
   */
  void coverRawIdSpace(lmdb::WriteTransaction& transaction, std::uint32_t lastTrackId, std::uint32_t lastListId = 0);

  /** Reads the committed id space record; every environment for the path must be closed. */
  IdSpace readRawIdSpace(std::filesystem::path const& path);

  /** Requires the intended rejection, not an earlier fail-closed admission guard. */
  void requireCorruptOpen(std::filesystem::path const& path, std::string_view expectedMessage);

  TrackId requireCreate(MusicLibrary& library, WriteTransaction& transaction, TrackBuilder const& builder);

  TrackId createCommittedTrack(MusicLibrary& library, TrackBuilder const& builder);
} // namespace ao::library::test
