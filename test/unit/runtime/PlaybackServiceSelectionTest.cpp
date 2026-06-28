// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/runtime/PlaybackServiceTestSupport.h"
#include <ao/Type.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/flow/Graph.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("PlaybackService selection - reveal track requests", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    bool revealFired = false;
    auto sub = fixture.playbackService.onRevealTrackRequested(
      [&](auto const& ev)
      {
        revealFired = true;
        CHECK(ev.trackId == TrackId{42});
      });
    fixture.playbackService.revealTrack(TrackId{42});
    CHECK(revealFired);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView fails with empty selection",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView fails when track does not exist",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    fixture.viewService.setSelection(result.viewId, {TrackId{99999}});

    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView fails with invalid view or selection",
            "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    TrackId const tid = fixture.playbackService.playSelectionInView(ViewId{999});
    CHECK(tid == kInvalidTrackId);
  }

  TEST_CASE("PlaybackService selection - playSelectionInView succeeds", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    fixture.onDevicesChangedCb(fixture.status.devices);
    fixture.onDevicesChangedCb(fixture.status.devices);
    auto emptyStatus = fixture.status;
    emptyStatus.devices.clear();
    fixture.onDevicesChangedCb(emptyStatus.devices);

    auto const trackId = fixture.testLib.addTrack("A Track");
    auto const result = fixture.viewService.createView({.listId = kInvalidListId}, true);
    fixture.viewService.setSelection(result.viewId, {trackId});

    if (fixture.onGraphChangedCb)
    {
      fixture.onGraphChangedCb(audio::flow::Graph{});
    }

    TrackId const tid = fixture.playbackService.playSelectionInView(result.viewId);
    CHECK(tid == trackId);
    CHECK(fixture.playbackService.state().trackId == trackId);

    if (fixture.renderTarget != nullptr)
    {
      fixture.renderTarget->onRouteReady("mock_anchor");
      fixture.renderTarget->onDrainComplete();
      fixture.renderTarget->onPropertyChanged(audio::PropertyId::Volume);
    }
  }

  TEST_CASE("PlaybackService selection - revealPlayingTrack works", "[runtime][unit][playback][selection]")
  {
    auto fixture = PlaybackFixture<MockExecutor>{};

    bool revealFired = false;
    auto sub = fixture.playbackService.onRevealTrackRequested([&](PlaybackService::RevealTrackRequested const& /*ev*/)
                                                              { revealFired = true; });
    fixture.playbackService.revealPlayingTrack();
    CHECK(revealFired);
  }
} // namespace ao::rt::test
