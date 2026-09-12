// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibrarySession.h"

#include "LibraryEditorModel.h"
#include "MainRunLoopExecutor.h"
#include "PlaybackSeekTarget.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/audio/BackendProvider.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/resource/ResourceByteMemoryCache.h>
#include <ao/uimodel/FrameClock.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/library/track/IndexedTrackRowCache.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/uimodel/library/track/TrackFilterView.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::appkit
{
  namespace
  {
    uimodel::FrameClock::TimePoint playbackFrameTime()
    {
      auto const elapsed = std::chrono::steady_clock::now().time_since_epoch();
      return uimodel::FrameClock::fromMicros(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
  } // namespace

  struct LibrarySession::Storage final
  {
    Storage(i18n::MessageCatalog catalogValue, std::function<void(DesktopInvalidation)> onInvalidatedValue)
      : catalog{std::move(catalogValue)}, onInvalidated{std::move(onInvalidatedValue)}
    {
    }
    DesktopViewState state{};
    i18n::MessageCatalog catalog;
    std::function<void(DesktopInvalidation)> onInvalidated;
    std::unique_ptr<rt::TextOrderingPolicy> textOrderingPolicyPtr;
    std::optional<rt::AppRuntime> optRuntime;
    MainRunLoopExecutor* executor = nullptr;
    rt::ViewId viewId = rt::kInvalidViewId;
    std::shared_ptr<rt::TrackListProjection const> projectionPtr;
    uimodel::TrackDisplayIndex displayIndex;
    uimodel::IndexedTrackRowCache rows;
    async::Subscription librarySub;
    async::Subscription workspaceSub;
    async::Subscription projectionSub;
    async::Subscription deltaSub;
    async::Subscription selectionSub;
    std::vector<TrackId> selectedTrackIds;
    std::unique_ptr<LibraryEditorModel> editorPtr;
    std::unique_ptr<uimodel::PlaybackActions> actionsPtr;
    std::array<std::unique_ptr<uimodel::TransportViewModel>, kPlaybackCommandCapacity> transportPtrs;
    std::unique_ptr<uimodel::NowPlayingViewModel> nowPlayingPtr;
    std::unique_ptr<uimodel::AobusSoulViewModel> soulPtr;
    std::unique_ptr<uimodel::OutputDeviceViewModel> outputPtr;
    std::unique_ptr<uimodel::VolumeViewModel> volumePtr;
    std::unique_ptr<uimodel::PlaybackPositionViewModel> positionPtr;
    uimodel::PlaybackPositionInterpolator positionInterpolator;
    std::unique_ptr<uimodel::TrackFilterViewModel> filterPtr;
    std::unique_ptr<uimodel::ActivityStatusViewModel> activityPtr;
    rt::ResourceByteMemoryCache::Request playingCoverRequest;
    rt::ResourceByteMemoryCache::Request selectedCoverRequest;
    ResourceId playingCoverId = kInvalidResourceId;
    ResourceId selectedCoverId = kInvalidResourceId;
    std::stop_source scanStop;
    bool closing = false;

    void invalidate(DesktopInvalidation invalidation) const
    {
      if (!closing)
      {
        onInvalidated(invalidation);
      }
    }

    // Storage stays alive through runtime shutdown and the final executor drain.
    static async::Task<void> scanAsync(Storage* storage, std::stop_token stopToken)
    {
      AO_INVARIANT(storage->optRuntime);
      auto& runtime = *storage->optRuntime;
      auto const outcome =
        co_await uimodel::runLibraryScanAsync(&runtime.library().jobs(), uimodel::LibraryScanMode::Eager, stopToken);

      co_await runtime.async().resumeOnCallbackExecutorAsync();

      if (!storage->closing)
      {
        storage->state.scanning = false;
        storage->invalidate(DesktopInvalidation::Library);

        runtime.notifications().post(uimodel::libraryScanSeverity(outcome.verdict),
                                     uimodel::formatLibraryScanMessage(storage->catalog, outcome),
                                     uimodel::libraryScanLifetime(outcome.verdict));
      }
    }
  };

  Result<std::unique_ptr<LibrarySession>> LibrarySession::create(std::filesystem::path const& root,
                                                                 std::filesystem::path const& stateRoot,
                                                                 bool restorePlayback,
                                                                 i18n::MessageCatalog catalog,
                                                                 std::function<void(DesktopInvalidation)> onInvalidated)
  {
    auto sessionPtr = std::unique_ptr<LibrarySession>{new LibrarySession{std::move(catalog), std::move(onInvalidated)}};

    if (auto res = sessionPtr->initialize(root, stateRoot, restorePlayback); !res)
    {
      return std::unexpected{res.error()};
    }

    return sessionPtr;
  }

  LibrarySession::LibrarySession(i18n::MessageCatalog catalog, std::function<void(DesktopInvalidation)> onInvalidated)
    : _storagePtr{std::make_unique<Storage>(std::move(catalog), std::move(onInvalidated))}
  {
  }

  Result<> LibrarySession::initialize(std::filesystem::path const& root,
                                      std::filesystem::path const& stateRoot,
                                      bool restorePlayback)
  {
    auto& storage = *_storagePtr;
    auto executorPtr = std::make_unique<MainRunLoopExecutor>();
    storage.executor = executorPtr.get();
    auto const paths = rt::LibraryPaths{root};
    auto textOrderingPolicyRes = i18n::createIcuTextOrderingPolicy(storage.catalog.requestedLocale());

    if (!textOrderingPolicyRes)
    {
      storage.executor = nullptr;
      return std::unexpected{textOrderingPolicyRes.error()};
    }

    storage.textOrderingPolicyPtr = std::move(*textOrderingPolicyRes);
    auto runtimeRes = rt::AppRuntime::create({
      .executorPtr = std::move(executorPtr),
      .musicRoot = root,
      .databasePath = paths.databasePath(),
      .cacheDirectory = stateRoot / "cache",
      .workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(paths.databasePath() / "appkit-workspace.yaml"),
      .textOrderingPolicy = storage.textOrderingPolicyPtr.get(),
    });

    if (!runtimeRes)
    {
      storage.executor = nullptr;
      return std::unexpected{runtimeRes.error()};
    }

    storage.optRuntime.emplace(std::move(*runtimeRes));
    auto& runtime = *storage.optRuntime;
    storage.librarySub =
      runtime.library().changes().onChanged([this](auto const& changeSet) { onLibraryChanged(changeSet); });

    for (auto& providerPtr : audio::createPlatformBackendProviders())
    {
      runtime.addAudioProvider(std::move(providerPtr));
    }

    if (auto res = runtime.workspace().restoreSession(runtime.workspaceConfigStore()); !res)
    {
      APP_LOG_WARN("AppKit workspace restore: {}", res.error().message);
    }

    if (runtime.workspace().snapshot().activeViewId == rt::kInvalidViewId)
    {
      if (auto res = runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}); !res)
      {
        return std::unexpected{res.error()};
      }
    }

    storage.editorPtr = std::make_unique<LibraryEditorModel>(
      runtime, storage.catalog, [&storage] { storage.invalidate(DesktopInvalidation::Editor); });

    storage.actionsPtr = std::make_unique<uimodel::PlaybackActions>(
      runtime.playback(),
      [this]
      {
        if (auto const ids = selection(); !ids.empty())
        {
          play(ids.front());
        }
        else if (auto const& projectionPtr = _storagePtr->projectionPtr; projectionPtr && projectionPtr->size() > 0)
        {
          play(projectionPtr->trackIdAt(0));
        }
      });

    auto const commands = uimodel::playbackCommands();
    AO_INVARIANT(commands.size() == kPlaybackCommandCapacity);

    for (auto const command : commands)
    {
      AO_INVARIANT(static_cast<std::size_t>(command) < kPlaybackCommandCapacity);
    }

    for (auto const command : commands)
    {
      auto const index = static_cast<std::size_t>(command);
      storage.transportPtrs[index] =
        std::make_unique<uimodel::TransportViewModel>(runtime.playback(),
                                                      *storage.actionsPtr,
                                                      storage.catalog,
                                                      command,
                                                      false,
                                                      [&storage, index](auto const& state)
                                                      {
                                                        storage.state.transport[index] = state;
                                                        storage.invalidate(DesktopInvalidation::Playback);
                                                      });
    }

    storage.nowPlayingPtr = std::make_unique<uimodel::NowPlayingViewModel>(
      runtime.playback(),
      storage.catalog,
      [&storage](auto const& state)
      {
        storage.state.nowPlaying = state;
        storage.invalidate(DesktopInvalidation::Playback);

        if (storage.playingCoverId != state.coverArtId)
        {
          storage.playingCoverRequest.reset();
          storage.playingCoverId = state.coverArtId;
          storage.state.playingCover = {};

          if (state.coverArtId != kInvalidResourceId)
          {
            storage.playingCoverRequest =
              storage.optRuntime->resourceBytes().request(state.coverArtId,
                                                          [&storage](rt::ResourceBytes bytes)
                                                          {
                                                            storage.state.playingCover = std::move(bytes);
                                                            storage.invalidate(DesktopInvalidation::Playback);
                                                          });
          }
        }
      });
    storage.soulPtr = std::make_unique<uimodel::AobusSoulViewModel>(runtime.playback(),
                                                                    [&storage](auto const& state)
                                                                    {
                                                                      storage.state.soul = state;
                                                                      storage.invalidate(DesktopInvalidation::Playback);
                                                                    });
    storage.outputPtr = std::make_unique<uimodel::OutputDeviceViewModel>(
      runtime.playback(),
      storage.catalog,
      [&storage](auto const& state)
      {
        storage.state.output = state;
        storage.invalidate(DesktopInvalidation::Playback);
      },
      uimodel::OutputDeviceIntent::discarded());
    storage.volumePtr = std::make_unique<uimodel::VolumeViewModel>(runtime.playback(),
                                                                   storage.catalog,
                                                                   [&storage](auto const& state)
                                                                   {
                                                                     storage.state.volume = state;
                                                                     storage.invalidate(DesktopInvalidation::Playback);
                                                                   });
    storage.positionPtr = std::make_unique<uimodel::PlaybackPositionViewModel>(
      runtime.playback(),
      [&storage, &runtime](auto const& state)
      {
        storage.state.position = state;
        storage.state.positionRevision = runtime.playback().snapshot().transport.positionRevision;
        storage.positionInterpolator.updateState(state.elapsed, state.duration, state.isPlaying && !state.isPreviewing);
        // Anchor at publication, including while the window is hidden.
        std::ignore = storage.positionInterpolator.interpolateElapsed(playbackFrameTime());
        storage.invalidate(DesktopInvalidation::Playback);
      });
    storage.filterPtr =
      std::make_unique<uimodel::TrackFilterViewModel>(runtime.views(),
                                                      runtime.workspace(),
                                                      storage.catalog,
                                                      [&storage](auto const& state)
                                                      {
                                                        storage.state.filter = state;
                                                        storage.invalidate(DesktopInvalidation::Library);
                                                      });
    storage.activityPtr = std::make_unique<uimodel::ActivityStatusViewModel>(
      runtime.notifications(),
      storage.catalog,
      [&storage](auto const& state)
      {
        storage.state.activity = state;
        storage.invalidate(DesktopInvalidation::Activity);
      },
      uimodel::ActivityStatusViewModelOptions{.libraryJobs = &runtime.library().jobs()});
    storage.workspaceSub = runtime.workspace().onChanged([this](auto const&) { bindProjection(); });
    storage.projectionSub = runtime.views().onProjectionChanged([this](auto const&) { bindProjection(); });
    storage.selectionSub = runtime.views().onSelectionChanged(
      [this](auto const& change)
      {
        if (change.viewId == _storagePtr->viewId)
        {
          updateSelection(change.selection);
        }
      });
    bindProjection();

    if (restorePlayback)
    {
      runtime.startPlaybackSessionPersistence();

      if (auto res = runtime.restorePlaybackSession(); !res)
      {
        APP_LOG_WARN("AppKit playback restore: {}", res.error().message);
      }
    }

    return {};
  }

  LibrarySession::~LibrarySession()
  {
    close();
  }
  DesktopViewState const& LibrarySession::state() const noexcept
  {
    return _storagePtr->state;
  }

  std::chrono::milliseconds LibrarySession::playbackElapsed() const
  {
    return _storagePtr->positionInterpolator.interpolateElapsed(playbackFrameTime());
  }

  rt::AppRuntime& LibrarySession::runtime() const noexcept
  {
    AO_INVARIANT(_storagePtr->optRuntime);
    return *_storagePtr->optRuntime;
  }
  LibraryEditorModel& LibrarySession::editor() const noexcept
  {
    return *_storagePtr->editorPtr;
  }

  i18n::MessageCatalog const& LibrarySession::catalog() const noexcept
  {
    return _storagePtr->catalog;
  }
  uimodel::TrackDisplayIndex const& LibrarySession::displayIndex() const noexcept
  {
    return _storagePtr->displayIndex;
  }

  void LibrarySession::onLibraryChanged(rt::LibraryChangeSet const& changeSet)
  {
    auto& storage = *_storagePtr;

    if (changeSet.libraryReset || !changeSet.listsUpserted.empty() || !changeSet.listsDeleted.empty())
    {
      ++storage.state.listRevision;
      storage.invalidate(DesktopInvalidation::Library);
    }

    auto const selectedId = storage.selectedTrackIds.size() == 1 ? storage.selectedTrackIds.front() : kInvalidTrackId;

    if (changeSet.libraryReset || std::ranges::contains(changeSet.tracksMutated, selectedId) ||
        std::ranges::contains(changeSet.tracksInserted, selectedId) ||
        std::ranges::contains(changeSet.tracksDeleted, selectedId))
    {
      refreshSelectedCover();
    }
  }

  void LibrarySession::bindProjection()
  {
    auto& storage = *_storagePtr;
    auto const viewId = runtime().workspace().snapshot().activeViewId;
    auto projectionRes = runtime().views().findTrackListProjection(viewId);

    if (!projectionRes || (*projectionRes == storage.projectionPtr && viewId == storage.viewId))
    {
      return;
    }

    storage.deltaSub.reset();
    storage.viewId = viewId;
    storage.projectionPtr = std::move(*projectionRes);
    // subscribe() synchronously publishes its reset before returning the handle.
    storage.deltaSub = storage.projectionPtr->subscribe([this](auto const&) { rebuildRows(); });
    refreshSelection();
  }

  void LibrarySession::rebuildRows()
  {
    auto& storage = *_storagePtr;
    auto const& projectionPtr = storage.projectionPtr;
    auto sections = std::vector<uimodel::TrackDisplaySection>{};

    for (std::size_t index = 0; index < projectionPtr->groupCount(); ++index)
    {
      auto const group = projectionPtr->groupAt(index);
      sections.push_back({.start = group.rows.start, .count = group.rows.count});
    }

    auto const displayReset = storage.displayIndex.tryReset(projectionPtr->size(), sections);
    AO_INVARIANT(displayReset);
    storage.rows.reset(
      projectionPtr->size(),
      [this](std::size_t index)
      { return runtime().library().snapshot().trackRow(_storagePtr->projectionPtr->trackIdAt(index)); });
    ++storage.state.tableRevision;
    storage.invalidate(DesktopInvalidation::Library);
  }

  rt::TrackRow const* LibrarySession::rowAt(std::size_t index)
  {
    auto const optItem = _storagePtr->displayIndex.itemAt(index);
    return optItem && optItem->kind == uimodel::TrackDisplayItemKind::TrackRow
             ? _storagePtr->rows.rowAt(optItem->sourceIndex)
             : nullptr;
  }

  uimodel::TrackGroupHeadingPresentation LibrarySession::groupHeading(std::size_t index) const
  {
    auto const group = _storagePtr->projectionPtr->groupAt(index);
    return uimodel::formatTrackGroupHeading(catalog(), group.heading);
  }

  std::vector<TrackId> LibrarySession::selection() const
  {
    return _storagePtr->selectedTrackIds;
  }

  std::optional<std::size_t> LibrarySession::displayIndexOf(TrackId trackId) const
  {
    auto const optIndex = _storagePtr->projectionPtr->indexOf(trackId);
    return optIndex ? displayIndex().displayIndexOfSourceRow(*optIndex) : std::nullopt;
  }

  void LibrarySession::select(std::vector<TrackId> ids)
  {
    auto res = runtime().views().setSelection(_storagePtr->viewId, std::move(ids));
    AO_INVARIANT(res);
  }

  void LibrarySession::refreshSelection()
  {
    auto stateRes = runtime().views().findTrackListState(_storagePtr->viewId);
    updateSelection(stateRes ? std::move(stateRes->selection) : std::vector<TrackId>{});
  }

  void LibrarySession::updateSelection(std::vector<TrackId> ids)
  {
    auto& storage = *_storagePtr;
    storage.selectedTrackIds = std::move(ids);
    refreshSelectedCover();
    storage.invalidate(DesktopInvalidation::Library);
  }

  void LibrarySession::refreshSelectedCover()
  {
    auto& storage = *_storagePtr;
    auto const& selectedTrackIds = storage.selectedTrackIds;
    auto const coverId = selectedTrackIds.size() == 1
                           ? runtime().library().snapshot().trackCoverArtId(selectedTrackIds.front())
                           : kInvalidResourceId;

    if (coverId != storage.selectedCoverId)
    {
      storage.selectedCoverRequest.reset();
      storage.selectedCoverId = coverId;
      storage.state.selectedCover = {};

      if (coverId != kInvalidResourceId)
      {
        storage.selectedCoverRequest =
          runtime().resourceBytes().request(coverId,
                                            [&storage](rt::ResourceBytes bytes)
                                            {
                                              storage.state.selectedCover = std::move(bytes);
                                              storage.invalidate(DesktopInvalidation::Library);
                                            });
      }

      storage.invalidate(DesktopInvalidation::Library);
    }
  }

  void LibrarySession::navigate(ListId listId) const
  {
    auto res = runtime().workspace().navigate({.target = listId});
    AO_INVARIANT(res);
  }

  void LibrarySession::setPresentation(std::string const& id) const
  {
    auto res = runtime().workspace().setActivePresentation(id);
    AO_INVARIANT(res);
  }

  Result<> LibrarySession::sort(rt::TrackSortField const field, bool const ascending) const
  {
    auto const viewId = runtime().workspace().snapshot().activeViewId;
    auto stateRes = runtime().views().findTrackListState(viewId);

    if (!stateRes)
    {
      return std::unexpected{stateRes.error()};
    }

    auto presentation = std::move(stateRes->presentation);
    presentation.id = "appkit-column-sort";

    if (field == rt::TrackSortField::TrackNumber)
    {
      presentation.sortBy = {{.field = rt::TrackSortField::DiscNumber, .ascending = ascending},
                             {.field = rt::TrackSortField::TrackNumber, .ascending = ascending}};
    }
    else
    {
      presentation.sortBy = {{.field = field, .ascending = ascending}};
    }

    return runtime().views().setPresentation(viewId, presentation);
  }

  void LibrarySession::filter(std::string const& text)
  {
    _storagePtr->filterPtr->updateFilter(text);
  }

  void LibrarySession::play(TrackId trackId)
  {
    if (auto res = runtime().playback().commands().startFromView(_storagePtr->viewId, trackId); !res)
    {
      runtime().notifications().post(
        rt::NotificationSeverity::Error, res.error().message, rt::NotificationLifetime::history());
    }
  }

  void LibrarySession::execute(uimodel::PlaybackCommand command)
  {
    auto& transportPtrs = _storagePtr->transportPtrs;
    auto const index = static_cast<std::size_t>(command);
    AO_INVARIANT(index < transportPtrs.size());
    AO_INVARIANT(transportPtrs[index]);
    transportPtrs[index]->handleClick();
  }

  void LibrarySession::seek(double fraction, PlaybackSeekTarget const& target)
  {
    auto const& current = runtime().playback().snapshot().transport;

    if (target.duration <= std::chrono::milliseconds{0} || target.revision != current.positionRevision ||
        current.duration <= std::chrono::milliseconds{0})
    {
      return;
    }

    _storagePtr->positionPtr->seekFinal(std::chrono::milliseconds{
      static_cast<std::int64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(target.duration.count()))});
  }

  void LibrarySession::setVolume(float volume)
  {
    _storagePtr->volumePtr->handleVolumeChanged(volume);
  }

  void LibrarySession::selectOutput(audio::OutputDeviceSelection const& selection)
  {
    _storagePtr->outputPtr->selectOutputDevice(selection.backendId, selection.deviceId, selection.profileId);
  }

  void LibrarySession::rescan()
  {
    if (auto& storage = *_storagePtr; !storage.state.scanning && !storage.closing)
    {
      storage.state.scanning = true;
      storage.invalidate(DesktopInvalidation::Library);
      storage.scanStop = std::stop_source{};
      runtime().async().spawnLogged(Storage::scanAsync(&storage, storage.scanStop.get_token()), "AppKit library scan");
    }
  }

  std::optional<std::chrono::steady_clock::duration> LibrarySession::refreshActivity()
  {
    _storagePtr->activityPtr->tryAutoDismissCompactIfDue();
    return _storagePtr->activityPtr->compactAutoDismissRemaining();
  }

  void LibrarySession::dismissActivity()
  {
    _storagePtr->activityPtr->dismissCompact();
  }

  void LibrarySession::hideActivityNotification(rt::NotificationId id)
  {
    _storagePtr->activityPtr->hideDetailNotification(id);
  }

  void LibrarySession::checkpoint() const
  {
    runtime().workspace().saveSession(runtime().workspaceConfigStore());
  }

  bool LibrarySession::canClose() const noexcept
  {
    return _storagePtr->executor == nullptr || !_storagePtr->executor->isPerforming();
  }

  void LibrarySession::close() noexcept
  {
    auto& storage = *_storagePtr;

    if (!storage.optRuntime)
    {
      return;
    }

    AO_EXPECTS(canClose());
    storage.closing = true;
    storage.scanStop.request_stop();

    if (storage.editorPtr)
    {
      storage.editorPtr->shutdown();
    }

    storage.librarySub.reset();
    storage.workspaceSub.reset();
    storage.projectionSub.reset();
    storage.deltaSub.reset();
    storage.selectionSub.reset();
    storage.playingCoverRequest.reset();
    storage.selectedCoverRequest.reset();
    storage.filterPtr.reset();
    storage.activityPtr.reset();
    storage.positionPtr.reset();
    storage.volumePtr.reset();
    storage.outputPtr.reset();
    storage.soulPtr.reset();
    storage.nowPlayingPtr.reset();

    for (auto& transportPtr : storage.transportPtrs)
    {
      transportPtr.reset();
    }

    storage.actionsPtr.reset();
    storage.rows.reset(0, {});
    storage.projectionPtr.reset();
    storage.optRuntime->shutdown();
    storage.executor->finishClosing();
    storage.editorPtr.reset();
    storage.optRuntime.reset();
    storage.textOrderingPolicyPtr.reset();
    storage.executor = nullptr;
    APP_LOG_INFO("AppKit session closed after producer join and final callback drain");
  }
} // namespace ao::appkit
