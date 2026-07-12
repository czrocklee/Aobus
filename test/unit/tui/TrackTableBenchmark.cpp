// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// Allocation-bound micro-benchmarks for the TUI track-table render path. Built
// as the standalone ao_tui_bench target (EXCLUDE_FROM_ALL, linked against
// mimalloc to mirror the shipping aobus-tui allocator), so the timings reflect
// production rather than the test binary's system allocator, and mimalloc never
// meets the ASan/TSan interceptors that the in-gate suites use.
//
// The A/B is the full build (viewportRows = 0, retained as the comparison path)
// versus the windowed build
// (viewportRows = a viewport). It isolates how per-frame element-tree
// construction -- and FTXUI's layout pass over that tree -- scales with library
// size when full and is bounded by the viewport when windowed. The rows-built
// counts reported alongside are allocator-independent, so they carry directly to
// the Windows debug build where each allocation simply costs far more.

#include "tui/TrackListEntry.h"
#include "tui/TrackSection.h"
#include "tui/TrackTable.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/benchmark/catch_chronometer.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

// Route operator new/delete through mimalloc so this opt-in benchmark measures
// the intended shipping allocation path. The target stays outside sanitizer
// gates because this explicit override competes with sanitizer interceptors.
#include <mimalloc-new-delete.h> // NOLINT(misc-include-cleaner) -- installs global allocation operators

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ao::tui::bench
{
  namespace
  {
    constexpr std::int32_t kColumns = 120;
    constexpr std::int32_t kViewportRows = 40;

    std::vector<TrackListEntry> manyTracks(std::size_t const count)
    {
      auto tracks = std::vector<TrackListEntry>{};
      tracks.reserve(count);

      for (std::size_t index = 0; index < count; ++index)
      {
        tracks.push_back(makeTrackListEntry(rt::TrackRow{.id = TrackId{static_cast<std::uint32_t>(index + 1)},
                                                         .title = std::format("Track {:05}", index),
                                                         .artist = "Artist",
                                                         .album = "Album",
                                                         .duration = std::chrono::seconds{60},
                                                         .trackNumber = static_cast<std::uint16_t>((index % 99) + 1)}));
      }

      return tracks;
    }

    // Equal-sized contiguous groups matching the projection's row-range convention.
    std::vector<TrackSection> equalSections(std::size_t const trackCount, std::size_t const perSection)
    {
      auto sections = std::vector<TrackSection>{};

      for (std::size_t begin = 0; begin < trackCount; begin += perSection)
      {
        sections.push_back(TrackSection{.rowBegin = begin,
                                        .rowCount = std::min(perSection, trackCount - begin),
                                        .primaryText = std::format("Album {:04}", begin / perSection)});
      }

      return sections;
    }

    TrackTableViewOptions fullOptions() noexcept
    {
      return TrackTableViewOptions{.availableColumns = kColumns - 2, .viewportRows = 0};
    }

    TrackTableViewOptions windowedOptions() noexcept
    {
      return TrackTableViewOptions{.availableColumns = kColumns - 2, .viewportRows = kViewportRows};
    }
  } // namespace

  TEST_CASE("bench: flat track-table build - full scales with N, windowed is bounded", "[benchmark][tui][track-table]")
  {
    auto const presentation = rt::defaultTrackPresentationSpec();

    for (std::size_t const trackCount : {std::size_t{1000}, std::size_t{5000}, std::size_t{20000}})
    {
      auto const tracks = manyTracks(trackCount);
      auto const selected = static_cast<std::int32_t>(trackCount / 2);
      auto const window = computeTrackTableWindow(
        selected, static_cast<std::int32_t>(trackCount), kViewportRows, kTrackTableOverscanRows);
      auto const windowedRows = window.endVisualRow - window.startVisualRow;

      WARN("N=" << trackCount << "  rows built:  full=" << trackCount << "  windowed(vp=" << kViewportRows
                << ")=" << windowedRows);

      BENCHMARK("build full   N=" + std::to_string(trackCount))
      {
        return trackTableView(tracks, selected, kInvalidTrackId, presentation, fullOptions());
      };

      BENCHMARK("build window N=" + std::to_string(trackCount))
      {
        return trackTableView(tracks, selected, kInvalidTrackId, presentation, windowedOptions());
      };
    }
  }

  TEST_CASE("bench: flat track-table render - full lays out all N, windowed lays out the window",
            "[benchmark][tui][track-table]")
  {
    auto const presentation = rt::defaultTrackPresentationSpec();
    auto const tracks = manyTracks(5000);
    auto const selected = static_cast<std::int32_t>(tracks.size() / 2);

    BENCHMARK_ADVANCED("render full   N=5000")(Catch::Benchmark::Chronometer meter)
    {
      auto elementPtr = trackTableView(tracks, selected, kInvalidTrackId, presentation, fullOptions());
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kColumns), ftxui::Dimension::Fixed(kViewportRows));
      meter.measure(
        [&]
        {
          ftxui::Render(screen, elementPtr);
          return screen.dimx();
        });
    };

    BENCHMARK_ADVANCED("render window N=5000")(Catch::Benchmark::Chronometer meter)
    {
      auto elementPtr = trackTableView(tracks, selected, kInvalidTrackId, presentation, windowedOptions());
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kColumns), ftxui::Dimension::Fixed(kViewportRows));
      meter.measure(
        [&]
        {
          ftxui::Render(screen, elementPtr);
          return screen.dimx();
        });
    };
  }

  TEST_CASE("bench: grouped track-table build - sections do not change the scaling story",
            "[benchmark][tui][track-table]")
  {
    auto const presentation = rt::defaultTrackPresentationSpec();
    auto const tracks = manyTracks(5000);
    auto const sections = equalSections(tracks.size(), 12);
    auto const selected = static_cast<std::int32_t>(tracks.size() / 2);

    BENCHMARK("build full   grouped N=5000")
    {
      return trackTableView(tracks, sections, selected, kInvalidTrackId, presentation, fullOptions());
    };

    BENCHMARK("build window grouped N=5000")
    {
      return trackTableView(tracks, sections, selected, kInvalidTrackId, presentation, windowedOptions());
    };
  }
} // namespace ao::tui::bench
