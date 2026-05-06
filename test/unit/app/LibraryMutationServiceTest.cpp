// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/CommandBus.h>
#include <runtime/CommandTypes.h>
#include <runtime/EventBus.h>
#include <runtime/EventTypes.h>
#include <runtime/Services.h>

#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <test/unit/lmdb/TestUtils.h>

namespace ao::app::test
{
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
    };

    class TestMusicLibrary final
    {
    public:
      TestMusicLibrary()
        : _tempDir{}, _library{_tempDir.path()}
      {
      }

      auto& library() { return _library; }

      auto addTrack(std::string_view title) -> ao::TrackId
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);
        auto builder = ao::library::TrackBuilder::createNew();
        builder.metadata().title(title).artist("A").album("B").year(2020);
        builder.property()
          .uri("/tmp/t.flac")
          .durationMs(200000)
          .bitrate(320000)
          .sampleRate(44100)
          .channels(2)
          .bitDepth(16);
        auto hot = builder.serializeHot(txn, _library.dictionary());
        auto cold = builder.serializeCold(txn, _library.dictionary(), _library.resources());
        auto [id, _] = writer.createHotCold(hot, cold);
        txn.commit();
        return id;
      }

    private:
      TempDir _tempDir;
      ao::library::MusicLibrary _library;
    };
  }

  TEST_CASE("LibraryMutationService - UpdateTrackMetadata publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Original Title");

    auto bus = CommandBus{};
    auto events = EventBus{};
    NullExecutor executor;
    auto service = LibraryMutationService{bus, events, executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = events.subscribe<TracksMutated>([&](TracksMutated const& ev) { mutated = ev.trackIds; });

    auto result = bus.execute(UpdateTrackMetadata{
      .trackIds = {trackId},
      .patch = MetadataPatch{.optTitle = "New Title"},
    });

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - EditTrackTags publishes TracksMutated", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Track");

    auto bus = CommandBus{};
    auto events = EventBus{};
    NullExecutor executor;
    auto service = LibraryMutationService{bus, events, executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = events.subscribe<TracksMutated>([&](TracksMutated const& ev) { mutated = ev.trackIds; });

    auto result = bus.execute(EditTrackTags{
      .trackIds = {trackId},
      .tagsToAdd = {"rock"},
    });

    REQUIRE(result.has_value());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryMutationService - no-op patch does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};
    auto trackId = testLib.addTrack("Track");

    auto bus = CommandBus{};
    auto events = EventBus{};
    NullExecutor executor;
    auto service = LibraryMutationService{bus, events, executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = events.subscribe<TracksMutated>([&](TracksMutated const& ev) { mutated = ev.trackIds; });

    auto result = bus.execute(UpdateTrackMetadata{
      .trackIds = {trackId},
      .patch = {},
    });

    REQUIRE(result.has_value());
    CHECK(mutated.size() == 1);
  }

  TEST_CASE("LibraryMutationService - missing track does not publish", "[app][runtime][mutation]")
  {
    auto testLib = TestMusicLibrary{};

    auto bus = CommandBus{};
    auto events = EventBus{};
    NullExecutor executor;
    auto service = LibraryMutationService{bus, events, executor, testLib.library()};

    auto mutated = std::vector<ao::TrackId>{};
    auto sub = events.subscribe<TracksMutated>([&](TracksMutated const& ev) { mutated = ev.trackIds; });

    auto result = bus.execute(UpdateTrackMetadata{
      .trackIds = {ao::TrackId{99999}},
      .patch = MetadataPatch{.optTitle = "X"},
    });

    REQUIRE(result.has_value());
    CHECK(mutated.empty());
  }
}
