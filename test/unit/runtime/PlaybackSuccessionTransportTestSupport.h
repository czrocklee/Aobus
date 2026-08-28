// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackSuccession.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTransportTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/audio/Engine.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>

namespace ao::audio::test
{
  struct BlockingPreparationGate;
}

namespace ao::rt::test::playback_succession
{
  class DecoderActivationProbe final
  {
  public:
    DecoderActivationProbe();
    ~DecoderActivationProbe();

    DecoderActivationProbe(DecoderActivationProbe const&) = delete;
    DecoderActivationProbe& operator=(DecoderActivationProbe const&) = delete;
    DecoderActivationProbe(DecoderActivationProbe&&) = delete;
    DecoderActivationProbe& operator=(DecoderActivationProbe&&) = delete;

    void registerPath(std::filesystem::path const& path);
    void notify(std::filesystem::path const& path);
    std::size_t count(std::filesystem::path const& path) const;
    std::map<std::filesystem::path, std::size_t> snapshot() const;

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

  audio::DecoderFactoryFn makeActivationProbedDecoderFactory(
    std::shared_ptr<DecoderActivationProbe> probePtr,
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr = {},
    std::filesystem::path blockedFileName = {},
    bool failBlockedPreparation = false,
    bool blockEveryLookahead = false,
    std::filesystem::path finalOpenFailureFileName = {});

  struct PlaybackSuccessionTransportFixtureConfig final
  {
    std::shared_ptr<audio::test::BlockingPreparationGate> blockingGatePtr{};
    std::filesystem::path blockedFileName{};
    bool failBlockedPreparation = false;
    bool blockEveryLookahead = false;
    std::filesystem::path finalOpenFailureFileName{};
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
    TrackId addPlayableTrack(std::string title);
    void openManualView(std::span<TrackId const> trackIds);
    void buildThreeTrackManualView();
    void buildFourTrackManualView();
    void buildTwoTrackManualView();
    void queueNaturalAdvance();
    Result<> playAndWait(TrackId trackId);
    std::size_t lookaheadActivationCount(TrackId trackId) const;
    bool waitForLookaheadAfter(TrackId trackId, std::size_t previousCount);

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
