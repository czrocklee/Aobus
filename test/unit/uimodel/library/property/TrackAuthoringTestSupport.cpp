// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/uimodel/library/property/TrackAuthoringTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  struct TrackAuthoringFixture::Impl final
  {
    explicit Impl(std::size_t const trackCount)
      : musicLibrary{library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db")}, asyncRuntime{executor}
    {
      trackIds.reserve(trackCount);

      for (std::size_t index = 0; index < trackCount; ++index)
      {
        trackIds.push_back(
          library::test::addTrack(musicLibrary,
                                  library::test::TrackSpec{.title = index == 0 ? "Old Title" : "Other Title",
                                                           .uri = "track-" + std::to_string(index) + ".flac"}));
      }

      auto readTransaction = musicLibrary.readTransaction();
      auto const revision = musicLibrary.libraryRevision(readTransaction);
      changesPtr = std::make_unique<rt::LibraryChanges>(executor, revision);
      libraryPtr = ao::test::requireValue(rt::Library::create(asyncRuntime, musicLibrary, *changesPtr));
    }

    ~Impl()
    {
      libraryPtr.reset();
      changesPtr.reset();
      asyncRuntime.requestStop();
      asyncRuntime.join();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ao::test::TempDir temp;
    library::MusicLibrary musicLibrary;
    std::vector<TrackId> trackIds;
    rt::test::InlineExecutor executor;
    async::Runtime asyncRuntime;
    std::unique_ptr<rt::LibraryChanges> changesPtr;
    std::unique_ptr<rt::Library> libraryPtr;
  };

  TrackAuthoringFixture::TrackAuthoringFixture(std::size_t const trackCount)
    : _implPtr{std::make_unique<Impl>(trackCount)}
  {
  }

  TrackAuthoringFixture::~TrackAuthoringFixture() = default;

  rt::Library& TrackAuthoringFixture::library() const
  {
    return *_implPtr->libraryPtr;
  }

  rt::LibraryChanges& TrackAuthoringFixture::changes() const
  {
    return *_implPtr->changesPtr;
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
