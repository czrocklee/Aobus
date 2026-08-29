// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"

#include "runtime/library/LibraryWriteLane.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/library/LibraryWriteLaneTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    async::LoopExecutor& stateOnlyLibraryExecutorInstance()
    {
      static thread_local auto executor = async::LoopExecutor{};
      return executor;
    }
  } // namespace

  MetadataPatch metadataPatch(library::test::TrackSpec const& spec)
  {
    auto patch = MetadataPatch{
      .optTitle = spec.title,
      .optArtist = spec.artist,
      .optAlbum = spec.album,
      .optAlbumArtist = spec.albumArtist,
      .optGenre = spec.genre,
      .optComposer = spec.composer,
      .optConductor = spec.conductor,
      .optEnsemble = spec.ensemble,
      .optWork = spec.work,
      .optMovement = spec.movement,
      .optSoloist = spec.soloist,
      .optYear = spec.year,
      .optTrackNumber = spec.trackNumber,
      .optTrackTotal = spec.trackTotal,
      .optDiscNumber = spec.discNumber,
      .optDiscTotal = spec.discTotal,
      .optMovementNumber = spec.movementNumber,
      .optMovementTotal = spec.movementTotal,
    };

    for (auto const& [key, value] : spec.customMetadata)
    {
      patch.customUpdates.emplace(key, value);
    }

    return patch;
  }

  TrackId addRuntimeTrack(AppRuntime& runtime,
                          library::test::TrackSpec const& spec,
                          compat::MoveOnlyFunction<void()> settlePublication)
  {
    static auto nextFixtureTrack = std::atomic<std::uint64_t>{0};
    auto sourcePath = std::filesystem::path{spec.uri};

    if (sourcePath.is_relative())
    {
      sourcePath = runtime.musicRoot() / sourcePath;
    }

    if (!std::filesystem::is_regular_file(sourcePath))
    {
      sourcePath = audio::test::requireAudioFixture("basic_metadata.flac");
    }

    auto const sequence = nextFixtureTrack.fetch_add(1, std::memory_order_relaxed);
    auto const relativePath =
      std::filesystem::path{".aobus-test"} / std::format("track-{}{}", sequence, sourcePath.extension().string());
    auto const destinationPath = runtime.musicRoot() / relativePath;
    std::filesystem::create_directories(destinationPath.parent_path());
    std::filesystem::copy_file(sourcePath, destinationPath, std::filesystem::copy_options::overwrite_existing);

    auto& commands = runtime.library().commands();
    auto createRes = settlePublication
                       ? runRuntimeTask(runtime, commands.createTrackFromFile(destinationPath), settlePublication)
                       : runRuntimeTask(runtime, commands.createTrackFromFile(destinationPath));
    REQUIRE(createRes);
    auto const trackId = createRes->trackId;

    auto bindingRes = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingRes);
    auto targets = std::move(*bindingRes);
    auto patchRes =
      settlePublication
        ? runRuntimeTask(runtime, commands.updateMetadata(targets, metadataPatch(spec)), settlePublication)
        : runRuntimeTask(runtime, commands.updateMetadata(targets, metadataPatch(spec)));
    REQUIRE(patchRes);
    REQUIRE((patchRes->status == AuthoringStatus::Applied || patchRes->status == AuthoringStatus::NoOp));

    if (!spec.tags.empty())
    {
      if (patchRes->optNextTargets)
      {
        targets = *patchRes->optNextTargets;
      }

      auto tagRes = settlePublication
                      ? runRuntimeTask(runtime, commands.editTags(targets, spec.tags, {}), settlePublication)
                      : runRuntimeTask(runtime, commands.editTags(targets, spec.tags, {}));
      REQUIRE(tagRes);
      REQUIRE((tagRes->status == AuthoringStatus::Applied || tagRes->status == AuthoringStatus::NoOp));
    }

    REQUIRE(spec.coverArtId == kInvalidResourceId);
    return trackId;
  }

  std::vector<TrackId> runtimeTrackIds(AppRuntime& runtime)
  {
    auto sourceRes = runtime.sources().acquire(kAllTracksListId);
    REQUIRE(sourceRes);
    auto const& source = sourceRes->source();
    auto trackIds = std::vector<TrackId>{};
    trackIds.reserve(source.size());

    for (std::size_t index = 0; index < source.size(); ++index)
    {
      trackIds.push_back(source.trackIdAt(index));
    }

    return trackIds;
  }

  library::test::TrackSpec runtimeTrackSpec(AppRuntime& runtime, TrackId const trackId)
  {
    auto projectionPtr = runtime.workspace().detailProjection(ExplicitSelectionTarget{.trackIds = {trackId}});
    auto const detail = projectionPtr->snapshot();
    REQUIRE(detail.selectionKind == SelectionKind::Single);

    auto stringField = [&detail](TrackField const field)
    {
      auto const& aggregate = trackFieldArrayAt(detail.fields, field);
      auto const* value = aggregate.optValue ? std::get_if<std::string>(&*aggregate.optValue) : nullptr;
      return value != nullptr ? *value : std::string{};
    };
    auto numberField = [&detail](TrackField const field)
    {
      auto const& aggregate = trackFieldArrayAt(detail.fields, field);
      auto const* value = aggregate.optValue ? std::get_if<std::uint16_t>(&*aggregate.optValue) : nullptr;
      return value != nullptr ? *value : std::uint16_t{0};
    };
    auto spec = library::test::TrackSpec{
      .title = stringField(TrackField::Title),
      .artist = stringField(TrackField::Artist),
      .album = stringField(TrackField::Album),
      .albumArtist = stringField(TrackField::AlbumArtist),
      .genre = stringField(TrackField::Genre),
      .composer = stringField(TrackField::Composer),
      .conductor = stringField(TrackField::Conductor),
      .ensemble = stringField(TrackField::Ensemble),
      .work = stringField(TrackField::Work),
      .movement = stringField(TrackField::Movement),
      .soloist = stringField(TrackField::Soloist),
      .coverArtId = detail.singleCoverArtId,
      .year = numberField(TrackField::Year),
      .discNumber = numberField(TrackField::DiscNumber),
      .discTotal = numberField(TrackField::DiscTotal),
      .trackNumber = numberField(TrackField::TrackNumber),
      .trackTotal = numberField(TrackField::TrackTotal),
      .movementNumber = numberField(TrackField::MovementNumber),
      .movementTotal = numberField(TrackField::MovementTotal),
    };
    auto snapshot = runtime.library().snapshot();

    for (auto const tagId : detail.commonTagIds)
    {
      spec.tags.push_back(snapshot.resolve(tagId));
    }

    for (auto const& item : detail.customMetadata)
    {
      if (item.value.optValue)
      {
        spec.customMetadata.emplace_back(item.key, *item.value.optValue);
      }
    }

    return spec;
  }

  void updateRuntimeTrack(AppRuntime& runtime,
                          TrackId const trackId,
                          compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater,
                          compat::MoveOnlyFunction<void()> settlePublication)
  {
    auto spec = runtimeTrackSpec(runtime, trackId);

    updater(spec);
    REQUIRE(spec.coverArtId == kInvalidResourceId);
    auto bindingRes = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingRes);
    auto result =
      settlePublication
        ? runRuntimeTask(
            runtime, runtime.library().commands().updateMetadata(*bindingRes, metadataPatch(spec)), settlePublication)
        : runRuntimeTask(runtime, runtime.library().commands().updateMetadata(*bindingRes, metadataPatch(spec)));
    REQUIRE(result);
    REQUIRE((result->status == AuthoringStatus::Applied || result->status == AuthoringStatus::NoOp));
  }

  struct MusicLibraryFixture::Impl final
  {
    ao::test::TempDir tempDir;
    library::MusicLibrary library{library::test::makeTestMusicLibrary(tempDir.path(), tempDir.path())};
  };

  MusicLibraryFixture::MusicLibraryFixture()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  MusicLibraryFixture::~MusicLibraryFixture() = default;

  library::MusicLibrary& MusicLibraryFixture::library()
  {
    return _implPtr->library;
  }

  library::MusicLibrary const& MusicLibraryFixture::library() const
  {
    return _implPtr->library;
  }

  std::filesystem::path const& MusicLibraryFixture::root() const
  {
    return _implPtr->tempDir.path();
  }

  TrackId MusicLibraryFixture::addTrack(library::test::TrackSpec const& spec)
  {
    return library::test::addTrackWithUniqueFixtureUri(_implPtr->library, spec);
  }

  void MusicLibraryFixture::updateTrack(TrackId const id,
                                        compat::MoveOnlyFunction<void(library::test::TrackSpec&)> updater)
  {
    library::test::updateTrackSpec(_implPtr->library, id, std::move(updater));
  }

  TrackId MusicLibraryFixture::addTrack(std::string_view const title)
  {
    return addTrack(library::test::TrackSpec{.title = std::string{title}});
  }

  LibraryChanges makeStateOnlyLibraryChanges(std::uint64_t const lastPublishedRevision)
  {
    return LibraryChanges{stateOnlyLibraryExecutorInstance(), lastPublishedRevision, "state-only-test-library"};
  }

  LibraryChanges makeStateOnlyLibraryChanges(library::MusicLibrary const& storage)
  {
    auto const transaction = storage.readTransaction();
    return LibraryChanges{stateOnlyLibraryExecutorInstance(),
                          storage.libraryRevision(transaction),
                          utility::pathToUtf8(storage.databasePath())};
  }

  async::Executor& stateOnlyLibraryExecutor()
  {
    return stateOnlyLibraryExecutorInstance();
  }

  LibraryChanges makeLibraryChanges(async::Executor& executor, std::uint64_t const lastPublishedRevision)
  {
    return LibraryChanges{executor, lastPublishedRevision, "test-library"};
  }

  LibraryChanges makeLibraryChanges(async::Executor& executor, library::MusicLibrary const& storage)
  {
    auto const transaction = storage.readTransaction();
    return LibraryChanges{executor, storage.libraryRevision(transaction), utility::pathToUtf8(storage.databasePath())};
  }

  namespace
  {
    TrackId addTrackAndPublishImpl(library::MusicLibrary& storage,
                                   LibraryChanges& changes,
                                   library::test::TrackSpec const& spec,
                                   bool const libraryReset,
                                   async::Executor& executor)
    {
      auto asyncRuntime = async::Runtime{executor};
      auto writeLane =
        LibraryWriteLane{asyncRuntime.callbackExecutor(), library::test::requireWritableLibrary(storage), changes};
      auto task = executeInteractiveMutation(
        writeLane.captureSubmission(),
        [&storage, &spec, libraryReset](library::LibraryWrite& write) -> Result<OperationOutcome<TrackId>>
        {
          auto const trackId = library::test::addTrackWithUniqueFixtureUri(storage, write, spec);
          return Changed<TrackId>{
            .value = trackId,
            .changeSet = LibraryChangeSet{.libraryReset = libraryReset, .tracksInserted = {trackId}},
          };
        });
      auto executionRes = runTestTask(asyncRuntime, executor, std::move(task));
      REQUIRE(executionRes);
      REQUIRE(executionRes->optCommittedRevision);
      return executionRes->value;
    }
  } // namespace

  TrackId addTrackAndPublish(library::MusicLibrary& storage,
                             LibraryChanges& changes,
                             library::test::TrackSpec const& spec,
                             async::Executor& executor)
  {
    return addTrackAndPublishImpl(storage, changes, spec, false, executor);
  }

  TrackId addTrackAndPublishReset(library::MusicLibrary& storage,
                                  LibraryChanges& changes,
                                  library::test::TrackSpec const& spec,
                                  async::Executor& executor)
  {
    return addTrackAndPublishImpl(storage, changes, spec, true, executor);
  }

  struct LibraryCommandsFixture::Impl final
  {
    Impl(library::MusicLibrary& storageValue, LibraryChanges& changesValue, async::Executor& executorValue)
      : executor{executorValue}, asyncRuntime{executor}, storage{storageValue}, changes{changesValue}
    {
    }

    ~Impl()
    {
      libraryPtr.reset();
      asyncRuntime.requestStop();
      asyncRuntime.join();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Library& ensureLibrary()
    {
      if (!libraryPtr)
      {
        libraryPtr = ao::test::requireValue(Library::create(asyncRuntime, storage, changes));
      }

      return *libraryPtr;
    }

    async::Executor& executor;
    async::Runtime asyncRuntime;
    library::MusicLibrary& storage;
    LibraryChanges& changes;
    std::unique_ptr<Library> libraryPtr;
    std::uint64_t nextFixtureTrack = 0;
  };

  LibraryCommandsFixture::LibraryCommandsFixture(library::MusicLibrary& storage,
                                                 LibraryChanges& changes,
                                                 async::Executor& executor)
    : _implPtr{std::make_unique<Impl>(storage, changes, executor)}
  {
  }

  LibraryCommandsFixture::~LibraryCommandsFixture() = default;

  Library& LibraryCommandsFixture::library()
  {
    return _implPtr->ensureLibrary();
  }

  LibraryCommands& LibraryCommandsFixture::commands()
  {
    return _implPtr->ensureLibrary().commands();
  }

  async::Runtime& LibraryCommandsFixture::runtime()
  {
    return _implPtr->asyncRuntime;
  }

  void LibraryCommandsFixture::releaseLibrary()
  {
    _implPtr->libraryPtr.reset();
  }

  TrackId LibraryCommandsFixture::addTrack(library::test::TrackSpec const& spec)
  {
    auto sourcePath = std::filesystem::path{spec.uri};

    if (sourcePath.is_relative())
    {
      sourcePath = _implPtr->storage.rootPath() / sourcePath;
    }

    if (!std::filesystem::is_regular_file(sourcePath))
    {
      sourcePath = audio::test::requireAudioFixture("basic_metadata.flac");
    }

    auto const relativePath =
      std::filesystem::path{".aobus-test"} /
      std::format("fixture-track-{}{}", _implPtr->nextFixtureTrack++, sourcePath.extension().string());
    auto const destinationPath = _implPtr->storage.rootPath() / relativePath;
    std::filesystem::create_directories(destinationPath.parent_path());
    std::filesystem::copy_file(sourcePath, destinationPath, std::filesystem::copy_options::overwrite_existing);

    auto createRes = runTask(commands().createTrackFromFile(destinationPath));
    REQUIRE(createRes);
    auto const trackId = createRes->trackId;
    REQUIRE(updateMetadata(std::array{trackId}, metadataPatch(spec)));

    if (!spec.tags.empty())
    {
      REQUIRE(editTags(std::array{trackId}, spec.tags, {}));
    }

    REQUIRE(spec.coverArtId == kInvalidResourceId);
    return trackId;
  }

  BoundTrackTargets LibraryCommandsFixture::bind(std::span<TrackId const> const trackIds)
  {
    return ao::test::requireValue(_implPtr->ensureLibrary().bindTrackTargets(trackIds));
  }

  Result<UpdateTrackMetadataReply> LibraryCommandsFixture::updateMetadata(std::span<TrackId const> const trackIds,
                                                                          MetadataPatch const& patch)
  {
    auto bindingRes = _implPtr->ensureLibrary().bindTrackTargets(trackIds);

    if (!bindingRes)
    {
      return std::unexpected{bindingRes.error()};
    }

    auto outcomeRes = runTask(_implPtr->ensureLibrary().commands().updateMetadata(*bindingRes, patch));

    if (!outcomeRes)
    {
      return std::unexpected{outcomeRes.error()};
    }

    switch (outcomeRes->status)
    {
      case AuthoringStatus::Applied:
      case AuthoringStatus::NoOp: return std::move(outcomeRes->reply);
      case AuthoringStatus::Busy: return makeError(Error::Code::ResourceBusy, "Track authoring is busy");
      case AuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
      case AuthoringStatus::Unavailable: return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
    }

    return makeError(Error::Code::InvalidState, "Unknown track authoring status");
  }

  Result<EditTrackTagsReply> LibraryCommandsFixture::editTags(std::span<TrackId const> const trackIds,
                                                              std::span<std::string const> const tagsToAdd,
                                                              std::span<std::string const> const tagsToRemove)
  {
    auto bindingRes = _implPtr->ensureLibrary().bindTrackTargets(trackIds);

    if (!bindingRes)
    {
      return std::unexpected{bindingRes.error()};
    }

    auto outcomeRes = runTask(_implPtr->ensureLibrary().commands().editTags(
      *bindingRes,
      std::vector<std::string>{tagsToAdd.begin(), tagsToAdd.end()},
      std::vector<std::string>{tagsToRemove.begin(), tagsToRemove.end()}));

    if (!outcomeRes)
    {
      return std::unexpected{outcomeRes.error()};
    }

    switch (outcomeRes->status)
    {
      case AuthoringStatus::Applied:
      case AuthoringStatus::NoOp: return std::move(outcomeRes->reply);
      case AuthoringStatus::Busy: return makeError(Error::Code::ResourceBusy, "Track authoring is busy");
      case AuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
      case AuthoringStatus::Unavailable: return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
    }

    return makeError(Error::Code::InvalidState, "Unknown track authoring status");
  }
} // namespace ao::rt::test
