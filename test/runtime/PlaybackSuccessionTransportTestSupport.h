// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSuccession.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTransportTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::audio::test
{
  struct BlockingPreparationGate;
  class StagedFailureGate;
}

namespace ao::rt::test::playback_succession
{
  class DecoderActivationProbe final
  {
  public:
    struct Snapshot final
    {
      std::map<std::filesystem::path, std::size_t> activations;
      std::map<std::filesystem::path, std::size_t> outputDecoderConstructions;

      bool operator==(Snapshot const&) const = default;
    };

    DecoderActivationProbe();
    ~DecoderActivationProbe();

    DecoderActivationProbe(DecoderActivationProbe const&) = delete;
    DecoderActivationProbe& operator=(DecoderActivationProbe const&) = delete;
    DecoderActivationProbe(DecoderActivationProbe&&) = delete;
    DecoderActivationProbe& operator=(DecoderActivationProbe&&) = delete;

    void registerPath(std::filesystem::path const& path);
    void notify(std::filesystem::path const& path);
    void notifyOutputDecoderConstruction(std::filesystem::path const& path);
    std::size_t count(std::filesystem::path const& path) const;
    Snapshot snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  class DecoderActivationGate final
  {
  public:
    explicit DecoderActivationGate(std::chrono::milliseconds timeout = std::chrono::seconds{10});
    ~DecoderActivationGate();

    DecoderActivationGate(DecoderActivationGate const&) = delete;
    DecoderActivationGate& operator=(DecoderActivationGate const&) = delete;
    DecoderActivationGate(DecoderActivationGate&&) = delete;
    DecoderActivationGate& operator=(DecoderActivationGate&&) = delete;

    void blockPath(std::filesystem::path const& path);
    void enterAndWait(std::filesystem::path const& path);
    bool tryWaitForEntry(std::chrono::milliseconds timeout = std::chrono::seconds{10});
    bool isHeld() const;
    bool hasTimedOut() const;
    void release();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  class [[nodiscard]] PreparationReleaseGuard final
  {
  public:
    explicit PreparationReleaseGuard(std::shared_ptr<audio::test::BlockingPreparationGate> gatePtr);
    ~PreparationReleaseGuard();

    PreparationReleaseGuard(PreparationReleaseGuard const&) = delete;
    PreparationReleaseGuard& operator=(PreparationReleaseGuard const&) = delete;
    PreparationReleaseGuard(PreparationReleaseGuard&&) = delete;
    PreparationReleaseGuard& operator=(PreparationReleaseGuard&&) = delete;

    void release();

  private:
    std::shared_ptr<audio::test::BlockingPreparationGate> _gatePtr;
  };

  struct PlaybackSuccessionTransportFixtureConfig final
  {
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr{};
    std::filesystem::path blockedFileName{};
    std::shared_ptr<DecoderActivationGate> activationGatePtr{};
    bool failBlockedPreparation = false;
    bool blockEveryLookahead = false;
    std::vector<std::filesystem::path> finalOpenFailureFileNames{};
    std::filesystem::path stagedFailureFileName{};
    audio::test::StagedFailureGate* stagedFailureGate = nullptr;
    std::filesystem::path lossyFileName{};
  };

  struct PlaybackSuccessionTransportFixture final
  {
    explicit PlaybackSuccessionTransportFixture(PlaybackSuccessionTransportFixtureConfig config = {});
    ~PlaybackSuccessionTransportFixture();

    PlaybackSuccessionTransportFixture(PlaybackSuccessionTransportFixture const&) = delete;
    PlaybackSuccessionTransportFixture& operator=(PlaybackSuccessionTransportFixture const&) = delete;
    PlaybackSuccessionTransportFixture(PlaybackSuccessionTransportFixture&&) = delete;
    PlaybackSuccessionTransportFixture& operator=(PlaybackSuccessionTransportFixture&&) = delete;

    LibraryCommands& commands();
    PlaybackService createPlayback();
    TrackId addPlayableTrack(std::string title);
    void openManualView(std::span<TrackId const> trackIds);
    void buildThreeTrackManualView();
    void buildFourTrackManualView();
    void buildTwoTrackManualView();
    void queueNaturalAdvance();
    // Settles explicit start and its successor, not the current decoder's activation.
    Result<> playAndWait(TrackId trackId);
    std::size_t decoderActivationCount(TrackId trackId) const;
    bool tryWaitForDecoderActivationAfter(TrackId trackId, std::size_t previousCount);

    std::shared_ptr<DecoderActivationProbe> decoderProbePtr;
    PlaybackTransportFixture<QueuedExecutor> transport;
    ControlledSleeper sleeper;
    async::Runtime asyncRuntime;
    LibraryChanges changes;
    LibraryCommandsFixture commandsFixture;
    TrackSourceCache sources;
    ViewService views;
    WorkspaceService workspace;
    std::unique_ptr<PlaybackSuccession> successionPtr;
    TrackId firstTrackId = kInvalidTrackId;
    TrackId secondTrackId = kInvalidTrackId;
    TrackId thirdTrackId = kInvalidTrackId;
    TrackId fourthTrackId = kInvalidTrackId;
    ListId listId = kInvalidListId;
    ViewId viewId = kInvalidViewId;
    std::map<TrackId, std::filesystem::path> trackPaths;
    std::uint32_t nextPlayableFile = 0;
  };
} // namespace ao::rt::test::playback_succession
