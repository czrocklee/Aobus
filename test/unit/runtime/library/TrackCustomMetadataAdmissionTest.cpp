// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackCustomMetadataAdmission - reserved writes reject the entire command before interning",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const originalCredits =
      std::vector<library::Credit>{{.name = "Original soloist", .kind = library::CreditKind::Soloist, .role = "Piano"}};
    auto const date = library::RecordingDate{.year = 1999, .month = 2, .day = 3};
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "Before",
                                                                 .work = "Original work",
                                                                 .recordingDate = date,
                                                                 .credits = originalCredits,
                                                                 .uri = "first.flac",
                                                                 .tags = {"Existing tag"},
                                                                 .customMetadata = {{"mood", "Calm"}}});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second", .uri = "second.flac"});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const ids = std::array{first, second};
    auto const targets = fixture.bind(ids);
    auto const revision = targets.revision();
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    std::size_t publications = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publications](LibraryChangeSet const&) noexcept { ++publications; });
    auto reservedKeys = std::vector{std::string{kCreditsMetadataKey}};

    for (auto const& field : trackFieldDefinitions())
    {
      reservedKeys.emplace_back(field.id);
    }

    auto exercise = [&](auto apply)
    {
      for (auto const& key : reservedKeys)
      {
        CAPTURE(key);
        auto const patch = MetadataPatch{
          .optTitle = "Must not commit",
          .optArtist = "Unseen artist",
          .optWork = "Must not replace work",
          .optRecordingDate = library::RecordingDate{.year = 2001},
          .optCredits = CreditReplacement{.kinds = std::bitset<library::kCreditKindCount>{0b1111},
                                          .entries = {{.name = "Unseen credit", .role = "Unseen role"}}},
          .customUpdates = {{key, "Unseen value"}, {"Unseen valid key", "Unseen valid value"}, {"mood", std::nullopt}},
        };
        auto const res = apply(patch);
        REQUIRE_FALSE(res);
        CHECK(res.error().code == Error::Code::InvalidInput);
        CHECK(res.error().message.contains(key));
        CHECK(res.error().message.contains("reserved"));
        CHECK(storage.library().dictionary().size() == dictionarySize);
        CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
        CHECK_FALSE(storage.library().dictionary().findId("Unseen artist"));
        CHECK_FALSE(storage.library().dictionary().findId("Unseen credit"));
        CHECK_FALSE(storage.library().dictionary().findId("Unseen role"));
        CHECK_FALSE(storage.library().dictionary().findId("Unseen valid key"));
        CHECK_FALSE(storage.library().dictionary().findId("Unseen tag"));
        CHECK(publications == 0);
        CHECK(targets.matches(fixture.library().authoringAvailability()));
        auto snapshot = fixture.library().snapshot();
        CHECK(snapshot.revision() == revision);
        CHECK(snapshot.trackField(first, TrackField::Title) == TrackFieldRawValue{std::string{"Before"}});
        CHECK(snapshot.trackField(second, TrackField::Title) == TrackFieldRawValue{std::string{"Second"}});
        CHECK(snapshot.trackField(first, TrackField::Work) == TrackFieldRawValue{std::string{"Original work"}});
        CHECK(snapshot.trackField(first, TrackField::RecordingDate) == TrackFieldRawValue{date});
        CHECK(snapshot.trackCredits(first) == std::optional{originalCredits});
        CHECK(snapshot.trackCustomMetadataValue(first, "mood") == "Calm");
        CHECK_FALSE(snapshot.trackCustomMetadataValue(first, key));
        CHECK_FALSE(snapshot.trackCustomMetadataValue(second, key));
        CHECK(snapshot.selectionTags(std::array{first}) == std::vector<std::string>{"Existing tag"});
        CHECK(snapshot.selectionTags(std::array{second}).empty());
      }
    };

    SECTION("metadata")
    {
      exercise([&](MetadataPatch const& patch)
               { return fixture.runTask(fixture.commands().updateMetadataAsync(targets, patch)); });
    }

    SECTION("combined Properties")
    {
      exercise(
        [&](MetadataPatch const& patch)
        {
          return fixture.runTask(fixture.commands().updatePropertiesAsync(
            targets, TrackPropertiesPatch{.metadata = patch, .tagsToAdd = {"Unseen tag"}}));
        });
    }

    SECTION("metadata preview")
    {
      exercise([&](MetadataPatch const& patch)
               { return fixture.runTask(fixture.commands().previewUpdateMetadataAsync({first, second}, patch)); });
    }

    SECTION("preview with no targets still validates the patch")
    {
      exercise([&](MetadataPatch const& patch)
               { return fixture.runTask(fixture.commands().previewUpdateMetadataAsync({}, patch)); });
    }
  }

  TEST_CASE("TrackCustomMetadataAdmission - reserved key deletion remains available for cleanup",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const originalCredits = std::vector<library::Credit>{{.name = "Original performer", .role = "Cello"}};
    // Core custom storage has no runtime field vocabulary. Seed through its normal builder,
    // not by bypassing runtime admission or introducing a legacy-format reader.
    auto const id = storage.addTrack(library::test::TrackSpec{
      .title = "Before",
      .credits = originalCredits,
      .customMetadata =
        {{"credits", "Prior custom text"}, {"title", ""}, {"codec", "Prior custom codec"}, {"mood", "Calm"}},
    });
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const targets = fixture.bind(std::array{id});
    auto const revision = targets.revision();
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    std::size_t publications = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publications](LibraryChangeSet const&) noexcept { ++publications; });
    auto const patch = MetadataPatch{
      .customUpdates = {
        {"credits", std::nullopt}, {"title", std::nullopt}, {"codec", std::nullopt}, {"file-size", std::nullopt}}};
    bool preview = false;

    SECTION("metadata")
    {
      auto const res = fixture.runTask(fixture.commands().updateMetadataAsync(targets, patch));
      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      CHECK(res->reply.changes.front().fields.size() == 3);
    }

    SECTION("Properties")
    {
      auto const res =
        fixture.runTask(fixture.commands().updatePropertiesAsync(targets, TrackPropertiesPatch{.metadata = patch}));
      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.metadata.changes.size() == 1);
      CHECK(res->reply.metadata.changes.front().fields.size() == 3);
      CHECK(res->reply.tags.changes.empty());
    }

    SECTION("preview")
    {
      preview = true;
      auto const res = fixture.runTask(fixture.commands().previewUpdateMetadataAsync({id}, patch));
      REQUIRE(res);
      REQUIRE(res->changes.size() == 1);
      CHECK(res->changes.front().fields.size() == 3);
    }

    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
    CHECK(publications == (preview ? 0U : 1U));
    auto snapshot = fixture.library().snapshot();
    CHECK(snapshot.revision() == revision + (preview ? 0U : 1U));
    CHECK(snapshot.trackField(id, TrackField::Title) == TrackFieldRawValue{std::string{"Before"}});
    CHECK(snapshot.trackCredits(id) == std::optional{originalCredits});
    CHECK(snapshot.trackCustomMetadataValue(id, "mood") == "Calm");
    CHECK_FALSE(snapshot.trackCustomMetadataValue(id, "file-size"));

    if (preview)
    {
      CHECK(snapshot.trackCustomMetadataValue(id, "credits") == "Prior custom text");
      CHECK(snapshot.trackCustomMetadataValue(id, "title") == "");
      CHECK(snapshot.trackCustomMetadataValue(id, "codec") == "Prior custom codec");
    }
    else
    {
      CHECK_FALSE(snapshot.trackCustomMetadataValue(id, "credits"));
      CHECK_FALSE(snapshot.trackCustomMetadataValue(id, "title"));
      CHECK_FALSE(snapshot.trackCustomMetadataValue(id, "codec"));
    }
  }

  TEST_CASE("TrackCustomMetadataAdmission - case variants and query aliases remain ordinary NFC custom keys",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const id = storage.addTrack("Before");
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const patch = MetadataPatch{.customUpdates = {{"musicians", "Legacy word"},
                                                       {"Credits", "Capitalized"},
                                                       {"credit", "Query alias"},
                                                       {"albumArtist", "Camel case"},
                                                       {"filePath", "Path alias"},
                                                       {"Re\u0301sume\u0301", "Cafe\u0301"}}};
    auto const res = fixture.updateMetadata(std::array{id}, patch);
    REQUIRE(res);
    REQUIRE(res->changes.size() == 1);
    CHECK(res->changes.front().fields.size() == 6);
    auto snapshot = fixture.library().snapshot();
    CHECK(snapshot.trackCustomMetadataValue(id, "musicians") == "Legacy word");
    CHECK(snapshot.trackCustomMetadataValue(id, "Credits") == "Capitalized");
    CHECK(snapshot.trackCustomMetadataValue(id, "credit") == "Query alias");
    CHECK(snapshot.trackCustomMetadataValue(id, "albumArtist") == "Camel case");
    CHECK(snapshot.trackCustomMetadataValue(id, "filePath") == "Path alias");
    CHECK(snapshot.trackCustomMetadataValue(id, "R\u00e9sum\u00e9") == "Caf\u00e9");
    CHECK_FALSE(snapshot.trackCustomMetadataValue(id, "credits"));
  }
} // namespace ao::rt::test
