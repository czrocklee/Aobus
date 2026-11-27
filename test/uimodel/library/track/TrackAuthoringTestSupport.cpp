// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/uimodel/library/track/TrackAuthoringTestSupport.h"

#include "test/TestFixtureSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/LibraryEditing.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    std::vector<TrackId> seedTracks(library::MusicLibrary& musicLibrary, std::size_t const trackCount)
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(trackCount);

      for (std::size_t index = 0; index < trackCount; ++index)
      {
        trackIds.push_back(library::test::addTrackWithUniqueFixtureUri(
          musicLibrary,
          library::test::TrackSpec{
            .title = index == 0 ? "Old Title" : "Other Title", .uri = "track-" + std::to_string(index) + ".flac"}));
      }

      return trackIds;
    }
  } // namespace

  struct TrackAuthoringFixture::Impl final
  {
    explicit Impl(std::size_t const trackCount)
      : trackIds{seedTracks(musicLibrary, trackCount)}
    {
    }

    ao::test::TempDir temp;
    library::MusicLibrary musicLibrary{library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db")};
    std::vector<TrackId> trackIds;
    rt::test::ManualExecutor executor;
    rt::LibraryChanges changes{rt::test::makeLibraryChanges(executor, musicLibrary)};
    rt::test::LibraryCommandsFixture commandsFixture{musicLibrary, changes, executor};
  };

  TrackAuthoringFixture::TrackAuthoringFixture(std::size_t const trackCount)
    : _implPtr{std::make_unique<Impl>(trackCount)}
  {
  }

  TrackAuthoringFixture::~TrackAuthoringFixture() = default;

  rt::Library const& TrackAuthoringFixture::library() const
  {
    return _implPtr->commandsFixture.library();
  }

  rt::LibraryCommands& TrackAuthoringFixture::libraryCommands() const
  {
    return _implPtr->commandsFixture.commands();
  }

  LibraryEditing TrackAuthoringFixture::editing() const
  {
    return LibraryEditing{libraryCommands()};
  }

  void TrackAuthoringFixture::beginClosing()
  {
    _implPtr->commandsFixture.beginClosing();
  }

  async::Runtime& TrackAuthoringFixture::runtime() const
  {
    return _implPtr->commandsFixture.runtime();
  }

  rt::test::ManualExecutor& TrackAuthoringFixture::executor() const
  {
    return _implPtr->executor;
  }

  std::span<TrackId const> TrackAuthoringFixture::trackIds() const noexcept
  {
    return _implPtr->trackIds;
  }

  std::string TrackAuthoringFixture::title(TrackId const trackId) const
  {
    auto transaction = _implPtr->musicLibrary.readTransaction();
    auto const optView =
      _implPtr->musicLibrary.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    return std::string{optView->metadata().title()};
  }

  std::vector<std::string> TrackAuthoringFixture::tags(TrackId const trackId) const
  {
    auto transaction = _implPtr->musicLibrary.readTransaction();
    auto const optView = _implPtr->musicLibrary.tracks().reader(transaction).get(trackId);
    REQUIRE(optView);

    auto names = std::vector<std::string>{};

    for (auto const tagId : optView->tags())
    {
      names.emplace_back(_implPtr->musicLibrary.dictionary().getOrDefault(tagId));
    }

    std::ranges::sort(names);
    return names;
  }
} // namespace ao::uimodel::test
