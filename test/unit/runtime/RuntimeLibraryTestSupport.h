// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class Library;
}

namespace ao::async
{
  class Executor;
  class Runtime;
}

namespace ao::rt::test
{
  MetadataPatch metadataPatch(library::test::TrackSpec const& spec);

  TrackId addRuntimeTrack(AppRuntime& runtime,
                          library::test::TrackSpec const& spec,
                          compat::MoveOnlyFunction<void()> settlePublication = {});

  std::vector<TrackId> runtimeTrackIds(AppRuntime& runtime);
  library::test::TrackSpec runtimeTrackSpec(AppRuntime& runtime, TrackId trackId);

  void updateRuntimeTrack(AppRuntime& runtime,
                          TrackId trackId,
                          compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater,
                          compat::MoveOnlyFunction<void()> settlePublication = {});

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
    void updateTrack(TrackId id, compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater);
    TrackId addTrack(std::string_view title);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  // State-only helper. Tests whose publication path can cross an executor
  // boundary must use makeLibraryChanges with an explicitly driven executor.
  LibraryChanges makeStateOnlyLibraryChanges(std::uint64_t lastPublishedRevision = 0);
  LibraryChanges makeStateOnlyLibraryChanges(library::MusicLibrary const& storage);
  async::Executor& stateOnlyLibraryExecutor();
  LibraryChanges makeLibraryChanges(async::Executor& executor, std::uint64_t lastPublishedRevision = 0);
  LibraryChanges makeLibraryChanges(async::Executor& executor, library::MusicLibrary const& storage);

  TrackId addTrackAndPublish(library::MusicLibrary& storage,
                             LibraryChanges& changes,
                             library::test::TrackSpec const& spec,
                             async::Executor& executor = stateOnlyLibraryExecutor());
  TrackId addTrackAndPublishReset(library::MusicLibrary& storage,
                                  LibraryChanges& changes,
                                  library::test::TrackSpec const& spec,
                                  async::Executor& executor = stateOnlyLibraryExecutor());

  class LibraryCommandsFixture final
  {
  public:
    LibraryCommandsFixture(library::MusicLibrary& storage,
                           LibraryChanges& changes,
                           async::Executor& executor = stateOnlyLibraryExecutor());
    ~LibraryCommandsFixture();

    LibraryCommandsFixture(LibraryCommandsFixture const&) = delete;
    LibraryCommandsFixture& operator=(LibraryCommandsFixture const&) = delete;
    LibraryCommandsFixture(LibraryCommandsFixture&&) = delete;
    LibraryCommandsFixture& operator=(LibraryCommandsFixture&&) = delete;

    Library& library();
    LibraryCommands& commands();
    async::Runtime& runtime();

    template<typename T>
    T runTask(async::Task<T> task)
    {
      return runTestTask(runtime(), runtime().callbackExecutor(), std::move(task));
    }

    void releaseLibrary();
    TrackId addTrack(library::test::TrackSpec const& spec);
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
