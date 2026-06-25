// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
//
// Baseline measurement for the ColumnView bind hot path — synthetic data, no
// fixed pass/fail thresholds. Mirrors the Phase 0 core baselines but targets the
// per-cell text work that runs on every visible cell as GTK recycles rows during
// scrolling: TrackRowCache::trackRow() (lazy row materialization) and
// TrackRowObject::displayText() (the stored/computed text the bind handler feeds
// into each label). It deliberately does NOT realize widgets — GTK's internal
// layout is version-dependent and flaky headless — so the numbers isolate the
// app-owned bind cost we can actually optimize, not the toolkit's.

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    // A representative wide table: a mix of text-backed columns (served straight
    // from the stored slot) and computed columns (formatted once, then memoized).
    // The split is what the bind path actually exercises per visible cell.
    constexpr auto kBenchFields = std::array{
      rt::TrackField::Title,
      rt::TrackField::Artist,
      rt::TrackField::Album,
      rt::TrackField::AlbumArtist,
      rt::TrackField::Genre, // text-backed
      rt::TrackField::Year,
      rt::TrackField::Duration,
      rt::TrackField::TrackNumber,
      rt::TrackField::Bitrate,
      rt::TrackField::SampleRate, // computed
    };

    std::vector<TrackId> seedLibrary(library::MusicLibrary& library, std::size_t count)
    {
      auto ids = std::vector<TrackId>{};
      ids.reserve(count);

      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      for (std::size_t i = 0; i < count; ++i)
      {
        auto builder = library::TrackBuilder::createNew();
        // Spread metadata across many distinct dictionary strings so resolution
        // and the sort/text caches see realistic cardinality rather than one value.
        builder.metadata()
          .title(std::format("Track {}", i))
          .artist(std::format("Artist {}", i % 500))
          .album(std::format("Album {}", i % 1000))
          .albumArtist(std::format("AlbumArtist {}", i % 400))
          .genre(std::format("Genre {}", i % 40))
          .year(static_cast<std::uint16_t>(1950 + (i % 70)))
          .trackNumber(static_cast<std::uint16_t>(1 + (i % 30)));
        builder.property()
          .uri(std::format("/music/track_{}.flac", i))
          .duration(std::chrono::milliseconds{120000 + static_cast<std::ptrdiff_t>(i % 200000)})
          .bitrate(Bitrate{320000})
          .sampleRate(SampleRate{44100})
          .channels(Channels{2})
          .bitDepth(BitDepth{16});

        auto serializeResult = builder.serialize(txn, library.dictionary(), library.resources());
        REQUIRE(serializeResult);
        auto const [hot, cold] = *serializeResult;
        ids.push_back(ao::test::requireValue(writer.createHotCold(hot, cold)).first);
      }

      REQUIRE(txn.commit());
      return ids;
    }
  } // namespace

  TEST_CASE("Track scroll bind-path baseline", "[baseline][gtk][track]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& library = fixture.runtime().musicLibrary();

    constexpr std::size_t kRowCount = 5000;
    constexpr std::size_t kReScrollPasses = 20;

    auto const ids = seedLibrary(library, kRowCount);
    CHECK(ids.size() == kRowCount);

    auto cache = TrackRowCache{fixture.runtime().library()};

    // Cold scroll: walk every row once, top to bottom. Each row is materialized
    // lazily (the first trackRow() opens a read txn, resolves dictionary strings,
    // populates) and every computed column is formatted for the first time. This
    // is the cost of scrolling through a library that has never been viewed.
    std::size_t coldSink = 0;
    auto const coldStart = std::chrono::steady_clock::now();

    for (auto const id : ids)
    {
      auto const rowPtr = cache.trackRow(id);

      for (auto const field : kBenchFields)
      {
        coldSink += rowPtr->displayText(field)->size();
      }
    }

    auto const coldEnd = std::chrono::steady_clock::now();
    auto const coldDuration = std::chrono::duration_cast<std::chrono::milliseconds>(coldEnd - coldStart);

    // Warm re-scroll: rows are cached and computed strings memoized, so this is the
    // steady-state cost of recycling cells back over already-seen rows — a cache
    // lookup plus a displayText() pointer fetch, no formatting, no allocation.
    std::size_t warmSink = 0;
    auto const warmStart = std::chrono::steady_clock::now();

    for (std::size_t pass = 0; pass < kReScrollPasses; ++pass)
    {
      for (auto const id : ids)
      {
        auto const rowPtr = cache.trackRow(id);

        for (auto const field : kBenchFields)
        {
          warmSink += rowPtr->displayText(field)->size();
        }
      }
    }

    auto const warmEnd = std::chrono::steady_clock::now();
    auto const warmDuration = std::chrono::duration_cast<std::chrono::milliseconds>(warmEnd - warmStart);

    auto const coldCells = kRowCount * kBenchFields.size();
    auto const warmCells = coldCells * kReScrollPasses;
    auto const coldPerCellNs =
      coldCells > 0 ? (static_cast<double>(coldDuration.count()) * 1'000'000.0) / static_cast<double>(coldCells) : 0.0;
    auto const warmPerCellNs =
      warmCells > 0 ? (static_cast<double>(warmDuration.count()) * 1'000'000.0) / static_cast<double>(warmCells) : 0.0;

    APP_LOG_INFO("=== Scroll bind-path baseline: {} rows x {} columns ===", kRowCount, kBenchFields.size());
    APP_LOG_INFO(
      "  Cold scroll (materialize + first format): {} ms ({:.1f} ns/cell)", coldDuration.count(), coldPerCellNs);
    APP_LOG_INFO("  Warm re-scroll x{} (cache hits): {} ms ({:.1f} ns/cell)",
                 kReScrollPasses,
                 warmDuration.count(),
                 warmPerCellNs);

    // Catastrophic-regression guards only; the logged numbers are the artifact.
    CHECK(coldSink > 0);
    CHECK(warmSink > 0);
    CHECK(coldDuration < std::chrono::minutes{1});
    CHECK(warmDuration < std::chrono::minutes{2});
  }
} // namespace ao::gtk::test
