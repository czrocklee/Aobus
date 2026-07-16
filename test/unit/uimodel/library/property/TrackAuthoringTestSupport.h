// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::test
{
  class TrackAuthoringFixture final
  {
  public:
    explicit TrackAuthoringFixture(std::size_t trackCount = 2)
      : _musicLibrary{library::test::makeTestMusicLibrary(_temp.path(), _temp.path() / "db")}, _asyncRuntime{_executor}
    {
      _trackIds.reserve(trackCount);

      for (std::size_t index = 0; index < trackCount; ++index)
      {
        _trackIds.push_back(
          library::test::addTrack(_musicLibrary,
                                  library::test::TrackSpec{.title = index == 0 ? "Old Title" : "Other Title",
                                                           .uri = "track-" + std::to_string(index) + ".flac"}));
      }

      auto readTransaction = _musicLibrary.readTransaction();
      auto const revision = _musicLibrary.libraryRevision(readTransaction);
      _changesPtr = std::make_unique<rt::LibraryChanges>(_executor, revision);
      _libraryPtr = std::make_unique<rt::Library>(_asyncRuntime, _musicLibrary, *_changesPtr);
    }

    ~TrackAuthoringFixture()
    {
      _libraryPtr.reset();
      _changesPtr.reset();
      _asyncRuntime.requestStop();
      _asyncRuntime.join();
    }

    TrackAuthoringFixture(TrackAuthoringFixture const&) = delete;
    TrackAuthoringFixture& operator=(TrackAuthoringFixture const&) = delete;
    TrackAuthoringFixture(TrackAuthoringFixture&&) = delete;
    TrackAuthoringFixture& operator=(TrackAuthoringFixture&&) = delete;

    rt::Library& library() const { return *_libraryPtr; }
    rt::LibraryChanges& changes() const { return *_changesPtr; }
    std::span<TrackId const> trackIds() const noexcept { return _trackIds; }

    std::string title(TrackId trackId) const
    {
      auto transaction = _musicLibrary.readTransaction();
      auto const optView =
        _musicLibrary.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      return std::string{optView->metadata().title()};
    }

    std::vector<std::string> tags(TrackId trackId) const
    {
      auto transaction = _musicLibrary.readTransaction();
      auto const optView = _musicLibrary.tracks().reader(transaction).get(trackId);
      REQUIRE(optView);

      auto names = std::vector<std::string>{};

      for (auto const tagId : optView->tags())
      {
        names.emplace_back(_musicLibrary.dictionary().getOrDefault(tagId));
      }

      std::ranges::sort(names);
      return names;
    }

  private:
    ao::test::TempDir _temp;
    library::MusicLibrary _musicLibrary;
    std::vector<TrackId> _trackIds;
    rt::test::InlineExecutor _executor;
    async::Runtime _asyncRuntime;
    std::unique_ptr<rt::LibraryChanges> _changesPtr;
    std::unique_ptr<rt::Library> _libraryPtr;
  };
} // namespace ao::uimodel::test
