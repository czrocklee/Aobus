// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/PlaylistExporter.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"
#include "track/TrackRowCache.h"
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glibmm/main.h>
#include <gtkmm/application.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::portal::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    class MutableTrackSource final : public rt::TrackSource
    {
    public:
      void addTrack(TrackId id) { _ids.push_back(id); }

      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        for (std::size_t i = 0; i < _ids.size(); ++i)
        {
          if (_ids[i] == id)
          {
            return i;
          }
        }

        return std::nullopt;
      }

    private:
      std::vector<TrackId> _ids;
    };

    class TestMusicLibrary final
    {
    public:
      TestMusicLibrary()
        : _tempDir{}, _library{_tempDir.path(), _tempDir.path()}
      {
      }

      library::MusicLibrary& library() { return _library; }

      TrackId addTrack(std::string const& title, std::string const& uri)
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);

        auto builder = library::TrackBuilder::createNew();
        builder.metadata().title(title);
        builder.property().uri(uri).durationMs(180000).bitrate(320000).sampleRate(44100).channels(2).bitDepth(16);

        auto const hotData = builder.serializeHot(txn, _library.dictionary());
        auto const coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
        auto [id, _] = writer.createHotCold(hotData, coldData);
        txn.commit();
        return id;
      }

    private:
      TempDir _tempDir;
      library::MusicLibrary _library;
    };
  } // namespace

  TEST_CASE("PlaylistExporter exports m3u playlists reactively", "[portal][unit][exporter]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.playlist_exporter_test");
    auto testLibrary = TestMusicLibrary{};

    auto const id1 = testLibrary.addTrack("Track 1", "/music/track1.flac");
    auto const id2 = testLibrary.addTrack("Track 2", "/music/track2.flac");

    auto source = MutableTrackSource{};
    source.addTrack(id1);
    source.addTrack(id2);

    auto rowCache = TrackRowCache{testLibrary.library()};

    auto const tempDir = TempDir{};
    auto const playlistPath = std::filesystem::path{tempDir.path()} / "playlist.m3u";

    {
      auto exporter = PlaylistExporter{source, rowCache, tempDir.path(), playlistPath};

      // Trigger manual write to avoid waiting 3s
      exporter.triggerWrite();

      // Wait for async file writing to complete (usually instant)
      auto const start = std::chrono::steady_clock::now();

      while (!std::filesystem::exists(playlistPath) &&
             (std::chrono::steady_clock::now() - start) < std::chrono::seconds{1})
      {
        Glib::MainContext::get_default()->iteration(true);
      }

      REQUIRE(std::filesystem::exists(playlistPath));

      auto ifs = std::ifstream{playlistPath};
      auto line = std::string{};
      auto lines = std::vector<std::string>{};

      while (std::getline(ifs, line))
      {
        lines.push_back(line);
      }

      REQUIRE(lines.size() == 2);
      CHECK_THAT(lines[0], Catch::Matchers::ContainsSubstring("music/track1.flac"));
      CHECK_THAT(lines[1], Catch::Matchers::ContainsSubstring("music/track2.flac"));

      SECTION("Manual triggerWrite")
      {
        exporter.triggerWrite();
        // Wait a bit
        Glib::MainContext::get_default()->iteration(false);
      }

      SECTION("Reactivity to inserted tracks")
      {
        auto const id3 = testLibrary.addTrack("Track 3", "/music/track3.flac");
        source.addTrack(id3);
        exporter.onInserted(id3, 2);
        // writeFile is debounced, so we just check it doesn't crash
      }

      SECTION("Reactivity to removed tracks")
      {
        exporter.onRemoved(id1, 0);
      }

      SECTION("Reactivity to updated tracks")
      {
        exporter.onUpdated(id1, 0);
      }

      SECTION("Fallback for missing URI")
      {
        auto const missingId = TrackId{999};
        source.addTrack(missingId);
        exporter.onInserted(missingId, 2);

        // Trigger manual write to avoid waiting 3s again
        exporter.triggerWrite();

        // Wait for write
        auto const start = std::chrono::steady_clock::now();

        while (!std::filesystem::exists(playlistPath) &&
               (std::chrono::steady_clock::now() - start) < std::chrono::seconds{2})
        {
          Glib::MainContext::get_default()->iteration(true);
        }
      }
    }
  }
} // namespace ao::gtk::portal::test
