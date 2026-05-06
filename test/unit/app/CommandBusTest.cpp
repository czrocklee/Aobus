// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <runtime/CommandBus.h>
#include <runtime/CommandTypes.h>

#include <memory>
#include <string>
#include <vector>

namespace ao::app::test
{
  TEST_CASE("CommandBus - execute returns handler result", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("void reply command")
    {
      auto called = false;
      bus.registerHandler<StopPlayback>(
        [&](StopPlayback const&) -> ao::Result<void>
        {
          called = true;
          return {};
        });

      auto result = bus.execute(StopPlayback{});
      REQUIRE(result.has_value());
      CHECK(called);
    }

    SECTION("typed reply command")
    {
      bus.registerHandler<CreateTrackListView>(
        [](CreateTrackListView const& /*cmd*/) -> ao::Result<CreateTrackListViewReply>
        { return CreateTrackListViewReply{.viewId = ViewId{42}}; });

      auto result = bus.execute(CreateTrackListView{});
      REQUIRE(result.has_value());
      CHECK(result->viewId == ViewId{42});
    }

    SECTION("command receives its fields")
    {
      bus.registerHandler<SeekPlayback>(
        [](SeekPlayback const& cmd) -> ao::Result<void>
        {
          CHECK(cmd.positionMs == 5000);
          return {};
        });

      auto result = bus.execute(SeekPlayback{.positionMs = 5000});
      REQUIRE(result.has_value());
    }

    SECTION("handler returning error")
    {
      bus.registerHandler<UpdateTrackMetadata>(
        [](UpdateTrackMetadata const&) -> ao::Result<UpdateTrackMetadataReply>
        { return std::unexpected(ao::Error{.code = ao::Error::Code::IoError, .message = "disk full"}); });

      auto result = bus.execute(UpdateTrackMetadata{
        .trackIds = {ao::TrackId{1}},
        .patch = {},
      });

      REQUIRE(!result.has_value());
      CHECK(result.error().code == ao::Error::Code::IoError);
      CHECK(result.error().message == "disk full");
    }
  }

  TEST_CASE("CommandBus - handler registration", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("register handler for a command type")
    {
      REQUIRE_NOTHROW(bus.registerHandler<PausePlayback>([](PausePlayback const&) -> ao::Result<void> { return {}; }));
    }

    SECTION("replace handler throws")
    {
      bus.registerHandler<PausePlayback>([](PausePlayback const&) -> ao::Result<void> { return {}; });

      REQUIRE_THROWS(bus.registerHandler<PausePlayback>([](PausePlayback const&) -> ao::Result<void> { return {}; }));
    }
  }

  TEST_CASE("CommandBus - execute without handler throws", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("unregistered command throws")
    {
      REQUIRE_THROWS(bus.execute(ResumePlayback{}));
    }
  }

  TEST_CASE("CommandBus - multiple command types coexist", "[app][runtime][command]")
  {
    auto bus = CommandBus{};
    auto pauseCalled = false;
    auto resumeCalled = false;

    bus.registerHandler<PausePlayback>(
      [&](PausePlayback const&) -> ao::Result<void>
      {
        pauseCalled = true;
        return {};
      });

    bus.registerHandler<ResumePlayback>(
      [&](ResumePlayback const&) -> ao::Result<void>
      {
        resumeCalled = true;
        return {};
      });

    SECTION("executing PausePlayback only calls pause handler")
    {
      bus.execute(PausePlayback{});
      CHECK(pauseCalled);
      CHECK(!resumeCalled);
    }

    SECTION("executing ResumePlayback only calls resume handler")
    {
      bus.execute(ResumePlayback{});
      CHECK(!pauseCalled);
      CHECK(resumeCalled);
    }

    SECTION("executing both calls correct handlers")
    {
      bus.execute(PausePlayback{});
      bus.execute(ResumePlayback{});
      CHECK(pauseCalled);
      CHECK(resumeCalled);
    }
  }

  TEST_CASE("CommandBus - handler captures state by move", "[app][runtime][command]")
  {
    auto bus = CommandBus{};
    auto ptr = std::make_unique<int>(42);

    bus.registerHandler<StopPlayback>(
      [p = std::move(ptr)](StopPlayback const&) -> ao::Result<void>
      {
        CHECK(*p == 42);
        return {};
      });

    bus.execute(StopPlayback{});
  }

  TEST_CASE("CommandBus - all playback command types", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("PlayTrack")
    {
      auto trackId = ao::TrackId{};
      bus.registerHandler<PlayTrack>(
        [&](PlayTrack const& cmd) -> ao::Result<void>
        {
          trackId = cmd.descriptor.trackId;
          return {};
        });

      bus.execute(PlayTrack{.descriptor = ao::audio::TrackPlaybackDescriptor{.trackId = ao::TrackId{99}}});
      CHECK(trackId == ao::TrackId{99});
    }

    SECTION("PlaySelectionInView")
    {
      bus.registerHandler<PlaySelectionInView>(
        [](PlaySelectionInView const& cmd) -> ao::Result<ao::TrackId>
        {
          CHECK(cmd.viewId == ViewId{10});
          return ao::TrackId{5};
        });

      auto result = bus.execute(PlaySelectionInView{.viewId = ViewId{10}});
      REQUIRE(result.has_value());
      CHECK(*result == ao::TrackId{5});
    }

    SECTION("SetPlaybackOutput")
    {
      bus.registerHandler<SetPlaybackOutput>(
        [](SetPlaybackOutput const& cmd) -> ao::Result<void>
        {
          CHECK(cmd.backendId == ao::audio::kBackendPipeWire);
          return {};
        });

      bus.execute(SetPlaybackOutput{
        .backendId = ao::audio::kBackendPipeWire,
      });
    }

    SECTION("SetPlaybackVolume")
    {
      auto vol = 0.0F;
      bus.registerHandler<SetPlaybackVolume>(
        [&](SetPlaybackVolume const& cmd) -> ao::Result<void>
        {
          vol = cmd.volume;
          return {};
        });

      bus.execute(SetPlaybackVolume{.volume = 0.75F});
      CHECK(vol == Catch::Approx(0.75F));
    }

    SECTION("SetPlaybackMuted")
    {
      auto muted = false;
      bus.registerHandler<SetPlaybackMuted>(
        [&](SetPlaybackMuted const& cmd) -> ao::Result<void>
        {
          muted = cmd.muted;
          return {};
        });

      bus.execute(SetPlaybackMuted{.muted = true});
      CHECK(muted);
    }
  }

  TEST_CASE("CommandBus - view commands", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("CreateTrackListView")
    {
      bus.registerHandler<CreateTrackListView>(
        [](CreateTrackListView const& cmd) -> ao::Result<CreateTrackListViewReply>
        {
          CHECK(cmd.attached);
          return CreateTrackListViewReply{.viewId = ViewId{1}};
        });

      auto result = bus.execute(CreateTrackListView{
        .initial = {},
        .attached = true,
      });
      REQUIRE(result.has_value());
      CHECK(result->viewId == ViewId{1});
    }

    SECTION("DestroyView")
    {
      auto destroyed = ViewId{};
      bus.registerHandler<DestroyView>(
        [&](DestroyView const& cmd) -> ao::Result<void>
        {
          destroyed = cmd.viewId;
          return {};
        });

      bus.execute(DestroyView{.viewId = ViewId{7}});
      CHECK(destroyed == ViewId{7});
    }

    SECTION("SetViewFilter")
    {
      auto filter = std::string{};
      bus.registerHandler<SetViewFilter>(
        [&](SetViewFilter const& cmd) -> ao::Result<void>
        {
          filter = cmd.filterExpression;
          return {};
        });

      bus.execute(SetViewFilter{.viewId = ViewId{1}, .filterExpression = "$year > 2000"});
      CHECK(filter == "$year > 2000");
    }

    SECTION("SetViewSort")
    {
      auto sortBy = std::vector<TrackSortTerm>{};
      bus.registerHandler<SetViewSort>(
        [&](SetViewSort const& cmd) -> ao::Result<void>
        {
          sortBy = cmd.sortBy;
          return {};
        });

      bus.execute(SetViewSort{
        .viewId = ViewId{1},
        .sortBy = {TrackSortTerm{.field = TrackSortField::Year, .ascending = false}},
      });
      REQUIRE(sortBy.size() == 1);
      CHECK(sortBy[0].field == TrackSortField::Year);
      CHECK(!sortBy[0].ascending);
    }

    SECTION("SetViewSelection")
    {
      auto selection = std::vector<ao::TrackId>{};
      bus.registerHandler<SetViewSelection>(
        [&](SetViewSelection const& cmd) -> ao::Result<void>
        {
          selection = cmd.selection;
          return {};
        });

      auto ids = std::vector<ao::TrackId>{ao::TrackId{1}, ao::TrackId{2}, ao::TrackId{3}};
      bus.execute(SetViewSelection{.viewId = ViewId{1}, .selection = ids});
      CHECK(selection == ids);
    }

    SECTION("SetFocusedView with view")
    {
      auto focused = ViewId{};
      bus.registerHandler<SetFocusedView>(
        [&](SetFocusedView const& cmd) -> ao::Result<void>
        {
          focused = cmd.viewId;
          return {};
        });

      bus.execute(SetFocusedView{.viewId = ViewId{5}});
      CHECK(focused == ViewId{5});
    }

    SECTION("SetFocusedView clears focus")
    {
      auto focused = ViewId{1};
      bus.registerHandler<SetFocusedView>(
        [&](SetFocusedView const& cmd) -> ao::Result<void>
        {
          focused = cmd.viewId;
          return {};
        });

      bus.execute(SetFocusedView{.viewId = ViewId{}});
      CHECK(focused == ViewId{});
    }
  }

  TEST_CASE("CommandBus - library mutation commands", "[app][runtime][command]")
  {
    auto bus = CommandBus{};

    SECTION("UpdateTrackMetadata")
    {
      bus.registerHandler<UpdateTrackMetadata>(
        [](UpdateTrackMetadata const& cmd) -> ao::Result<UpdateTrackMetadataReply>
        {
          CHECK(cmd.trackIds.size() == 2);
          CHECK(cmd.patch.optTitle == "new title");
          return UpdateTrackMetadataReply{.mutatedIds = {ao::TrackId{1}, ao::TrackId{2}}};
        });

      auto result = bus.execute(UpdateTrackMetadata{
        .trackIds = {ao::TrackId{1}, ao::TrackId{2}},
        .patch = {.optTitle = "new title"},
      });
      REQUIRE(result.has_value());
      CHECK(result->mutatedIds.size() == 2);
    }

    SECTION("EditTrackTags")
    {
      bus.registerHandler<EditTrackTags>(
        [](EditTrackTags const& cmd) -> ao::Result<EditTrackTagsReply>
        {
          CHECK(cmd.tagsToAdd.size() == 1);
          CHECK(cmd.tagsToAdd[0] == "rock");
          CHECK(cmd.tagsToRemove.size() == 1);
          CHECK(cmd.tagsToRemove[0] == "jazz");
          return EditTrackTagsReply{.mutatedIds = {ao::TrackId{1}}};
        });

      auto result = bus.execute(EditTrackTags{
        .trackIds = {ao::TrackId{1}},
        .tagsToAdd = {"rock"},
        .tagsToRemove = {"jazz"},
      });
      REQUIRE(result.has_value());
    }

    SECTION("ImportFiles")
    {
      bus.registerHandler<ImportFiles>(
        [](ImportFiles const& cmd) -> ao::Result<ImportFilesReply>
        {
          CHECK(cmd.paths.size() == 1);
          return ImportFilesReply{.importedTrackCount = 42};
        });

      auto result = bus.execute(ImportFiles{
        .paths = {"/tmp/test.flac"},
      });
      REQUIRE(result.has_value());
      CHECK(result->importedTrackCount == 42);
    }
  }
}
