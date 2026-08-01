// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/RuntimeLibraryTestSupport.h"

#include "runtime/library/LibraryMutationService.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ao::rt::test
{
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
                          std::move_only_function<void()> settlePublication)
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

    auto& writer = runtime.library().writer();
    auto createResult = writer.createTrackFromFile(destinationPath);
    REQUIRE(createResult);
    auto const trackId = createResult->trackId;

    if (settlePublication)
    {
      settlePublication();
    }

    auto bindingResult = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingResult);
    auto targets = std::move(*bindingResult);
    auto patchResult = writer.updateMetadata(targets, metadataPatch(spec));
    REQUIRE(patchResult);
    REQUIRE(
      (patchResult->status == TrackAuthoringStatus::Applied || patchResult->status == TrackAuthoringStatus::NoOp));

    if (settlePublication)
    {
      settlePublication();
    }

    if (!spec.tags.empty())
    {
      if (patchResult->optNextTargets)
      {
        targets = *patchResult->optNextTargets;
      }

      auto tagResult = writer.editTags(targets, spec.tags, std::span<std::string const>{});
      REQUIRE(tagResult);
      REQUIRE((tagResult->status == TrackAuthoringStatus::Applied || tagResult->status == TrackAuthoringStatus::NoOp));

      if (settlePublication)
      {
        settlePublication();
      }
    }

    REQUIRE(spec.coverArtId == kInvalidResourceId);
    return trackId;
  }

  void updateRuntimeTrack(AppRuntime& runtime,
                          TrackId const trackId,
                          std::move_only_function<void(library::test::TrackSpec&)> updater)
  {
    auto spec = library::test::TrackSpec{};
    {
      auto transaction = runtime.musicLibrary().readTransaction();
      auto optView =
        runtime.musicLibrary().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      spec = library::test::trackSpecFromView(runtime.musicLibrary(), *optView);
    }

    updater(spec);
    REQUIRE(spec.coverArtId == kInvalidResourceId);
    auto bindingResult = runtime.library().bindTrackTargets(std::span{&trackId, std::size_t{1}});
    REQUIRE(bindingResult);
    auto result = runtime.library().writer().updateMetadata(*bindingResult, metadataPatch(spec));
    REQUIRE(result);
    REQUIRE((result->status == TrackAuthoringStatus::Applied || result->status == TrackAuthoringStatus::NoOp));
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
    return library::test::addTrack(_implPtr->library, spec);
  }

  void MusicLibraryFixture::updateTrack(TrackId const id,
                                        std::move_only_function<void(library::test::TrackSpec&)> updater)
  {
    library::test::updateTrackSpec(_implPtr->library, id, std::move(updater));
  }

  TrackId MusicLibraryFixture::addTrack(std::string_view const title)
  {
    return addTrack(library::test::TrackSpec{.title = std::string{title}});
  }

  LibraryChanges makeStateOnlyLibraryChanges(std::uint64_t const lastPublishedRevision)
  {
    static thread_local auto executor = InlineExecutor{};
    return LibraryChanges{executor, lastPublishedRevision};
  }

  LibraryChanges makeStateOnlyLibraryChanges(library::MusicLibrary const& storage)
  {
    auto const transaction = storage.readTransaction();
    return makeStateOnlyLibraryChanges(storage.libraryRevision(transaction));
  }

  LibraryChanges makeLibraryChanges(async::Executor& executor, std::uint64_t const lastPublishedRevision)
  {
    return LibraryChanges{executor, lastPublishedRevision};
  }

  LibraryChanges makeLibraryChanges(async::Executor& executor, library::MusicLibrary const& storage)
  {
    auto const transaction = storage.readTransaction();
    return makeLibraryChanges(executor, storage.libraryRevision(transaction));
  }

  TrackId addTrackAndPublish(library::MusicLibrary& storage,
                             LibraryChanges& changes,
                             library::test::TrackSpec const& spec,
                             bool const libraryReset)
  {
    auto executor = InlineExecutor{};
    auto mutationService = LibraryMutationService{executor, library::test::requireWritableLibrary(storage), changes};
    auto mutation = ao::test::requireValue(mutationService.beginInteractiveMutation());
    auto const trackId = library::test::addTrack(storage, mutation.transaction(), spec);
    REQUIRE(mutation.commit(LibraryChangeSet{
      .libraryReset = libraryReset,
      .tracksInserted = {trackId},
    }));
    return trackId;
  }

  struct LibraryWriterFixture::Impl final
  {
    Impl(library::MusicLibrary& storageValue, LibraryChanges& changesValue)
      : asyncRuntime{executor}, storage{storageValue}, changes{changesValue}
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

    InlineExecutor executor;
    async::Runtime asyncRuntime;
    library::MusicLibrary& storage;
    LibraryChanges& changes;
    std::unique_ptr<Library> libraryPtr;
  };

  LibraryWriterFixture::LibraryWriterFixture(library::MusicLibrary& storage, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(storage, changes)}
  {
  }

  LibraryWriterFixture::~LibraryWriterFixture() = default;

  Library& LibraryWriterFixture::library()
  {
    return _implPtr->ensureLibrary();
  }

  LibraryWriter& LibraryWriterFixture::writer()
  {
    return _implPtr->ensureLibrary().writer();
  }

  void LibraryWriterFixture::releaseLibrary()
  {
    _implPtr->libraryPtr.reset();
  }

  BoundTrackTargets LibraryWriterFixture::bind(std::span<TrackId const> const trackIds)
  {
    return ao::test::requireValue(_implPtr->ensureLibrary().bindTrackTargets(trackIds));
  }

  Result<UpdateTrackMetadataReply> LibraryWriterFixture::updateMetadata(std::span<TrackId const> const trackIds,
                                                                        MetadataPatch const& patch)
  {
    auto bindingResult = _implPtr->ensureLibrary().bindTrackTargets(trackIds);

    if (!bindingResult)
    {
      return std::unexpected{bindingResult.error()};
    }

    auto outcomeResult = _implPtr->ensureLibrary().writer().updateMetadata(*bindingResult, patch);

    if (!outcomeResult)
    {
      return std::unexpected{outcomeResult.error()};
    }

    switch (outcomeResult->status)
    {
      case TrackAuthoringStatus::Applied:
      case TrackAuthoringStatus::NoOp: return std::move(outcomeResult->reply);
      case TrackAuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
      case TrackAuthoringStatus::Unavailable:
        return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
    }

    return makeError(Error::Code::InvalidState, "Unknown track authoring status");
  }

  Result<EditTrackTagsReply> LibraryWriterFixture::editTags(std::span<TrackId const> const trackIds,
                                                            std::span<std::string const> const tagsToAdd,
                                                            std::span<std::string const> const tagsToRemove)
  {
    auto bindingResult = _implPtr->ensureLibrary().bindTrackTargets(trackIds);

    if (!bindingResult)
    {
      return std::unexpected{bindingResult.error()};
    }

    auto outcomeResult = _implPtr->ensureLibrary().writer().editTags(*bindingResult, tagsToAdd, tagsToRemove);

    if (!outcomeResult)
    {
      return std::unexpected{outcomeResult.error()};
    }

    switch (outcomeResult->status)
    {
      case TrackAuthoringStatus::Applied:
      case TrackAuthoringStatus::NoOp: return std::move(outcomeResult->reply);
      case TrackAuthoringStatus::Stale: return makeError(Error::Code::Conflict, "Track authoring binding is stale");
      case TrackAuthoringStatus::Unavailable:
        return makeError(Error::Code::InvalidState, "Track authoring is unavailable");
    }

    return makeError(Error::Code::InvalidState, "Unknown track authoring status");
  }
} // namespace ao::rt::test
