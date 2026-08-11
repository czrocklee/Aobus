// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
  class Library;
}

namespace ao::async
{
  class Executor;
}

namespace ao::rt::test
{
  MetadataPatch metadataPatch(library::test::TrackSpec const& spec);

  TrackId addRuntimeTrack(AppRuntime& runtime,
                          library::test::TrackSpec const& spec,
                          std::move_only_function<void()> settlePublication = {});

  void updateRuntimeTrack(AppRuntime& runtime,
                          TrackId trackId,
                          std::move_only_function<void(library::test::TrackSpec&)> updater);

  class MusicLibraryFixture final
  {
  public:
    MusicLibraryFixture();
    ~MusicLibraryFixture();

    MusicLibraryFixture(MusicLibraryFixture const&) = delete;
    MusicLibraryFixture& operator=(MusicLibraryFixture const&) = delete;
    MusicLibraryFixture(MusicLibraryFixture&&) = delete;
    MusicLibraryFixture& operator=(MusicLibraryFixture&&) = delete;

    library::MusicLibrary& library();
    library::MusicLibrary const& library() const;
    std::filesystem::path const& root() const;
    TrackId addTrack(library::test::TrackSpec const& spec);
    void updateTrack(TrackId id, std::move_only_function<void(library::test::TrackSpec&)> updater);
    TrackId addTrack(std::string_view title);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  // State-only helper. Tests whose publication path can cross an executor
  // boundary must use makeLibraryChanges with an explicitly driven executor.
  LibraryChanges makeStateOnlyLibraryChanges(std::uint64_t lastPublishedRevision = 0);
  LibraryChanges makeStateOnlyLibraryChanges(library::MusicLibrary const& storage);
  LibraryChanges makeLibraryChanges(async::Executor& executor, std::uint64_t lastPublishedRevision = 0);
  LibraryChanges makeLibraryChanges(async::Executor& executor, library::MusicLibrary const& storage);

  TrackId addTrackAndPublish(library::MusicLibrary& storage,
                             LibraryChanges& changes,
                             library::test::TrackSpec const& spec);
  TrackId addTrackAndPublishReset(library::MusicLibrary& storage,
                                  LibraryChanges& changes,
                                  library::test::TrackSpec const& spec);

  class LibraryWriterFixture final
  {
  public:
    LibraryWriterFixture(library::MusicLibrary& storage, LibraryChanges& changes);
    ~LibraryWriterFixture();

    LibraryWriterFixture(LibraryWriterFixture const&) = delete;
    LibraryWriterFixture& operator=(LibraryWriterFixture const&) = delete;
    LibraryWriterFixture(LibraryWriterFixture&&) = delete;
    LibraryWriterFixture& operator=(LibraryWriterFixture&&) = delete;

    Library& library();
    LibraryWriter& writer();
    void releaseLibrary();
    BoundTrackTargets bind(std::span<TrackId const> trackIds);
    Result<UpdateTrackMetadataReply> updateMetadata(std::span<TrackId const> trackIds, MetadataPatch const& patch);
    Result<EditTrackTagsReply> editTags(std::span<TrackId const> trackIds,
                                        std::span<std::string const> tagsToAdd,
                                        std::span<std::string const> tagsToRemove);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt::test
