// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("TrackTargetAdmission - duplicate targets author unseen metadata once in first-seen order",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const original = std::vector<library::Credit>{{.name = "Original performer", .role = "Piano"}};
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "First", .credits = original});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second", .credits = original});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto& runtimeLibrary = fixture.library();
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto const orderedIds = std::array{second, first, second, first};
    auto const uniqueIds = std::array{second, first};
    auto const targetsRes = runtimeLibrary.bindTrackTargets(orderedIds);
    REQUIRE(targetsRes);
    CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
    auto const revision = targetsRes->revision();
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    auto const replacement = std::vector<library::Credit>{{.name = "Unseen René", .role = "Unseen Flûte"}};
    auto const patch = MetadataPatch{
      .optTitle = "Unseen résumé title",
      .optCredits = CreditReplacement{.kinds = std::bitset<library::kCreditKindCount>{0b1000}, .entries = replacement},
      .customUpdates = {{"Unseen clé", "Unseen café"}},
    };
    auto const newTexts = std::array{"Unseen résumé title", "Unseen René", "Unseen Flûte", "Unseen clé", "Unseen café"};

    for (auto const* const text : newTexts)
    {
      REQUIRE_FALSE(storage.library().dictionary().findId(text));
    }

    auto exercise = [&](auto runPatch, auto metadataChanges)
    {
      auto const res = runPatch(*targetsRes, patch);
      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      auto const& records = std::invoke(metadataChanges, res->reply);
      REQUIRE(records.size() == uniqueIds.size());

      for (std::size_t index = 0; index < uniqueIds.size(); ++index)
      {
        CHECK(records[index].trackId == uniqueIds[index]);
        CHECK(records[index].fields.size() == 3);
      }

      CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
      REQUIRE(res->optNextTargets);
      CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
      CHECK(res->optNextTargets->revision() == revision + 1);
      CHECK(res->optNextTargets->matches(runtimeLibrary.authoringAvailability()));
      REQUIRE(publications.size() == 1);
      CHECK(publications[0].libraryRevision == revision + 1);
      CHECK(std::ranges::is_permutation(publications[0].tracksMutated, uniqueIds));
      CHECK(storage.library().dictionary().size() > dictionarySize);
      CHECK(storage.library().dictionary().generation() > dictionaryGeneration);

      for (auto const* const text : std::array{"Unseen René", "Unseen Flûte", "Unseen clé"})
      {
        CHECK(storage.library().dictionary().findId(text).has_value());
      }

      // Titles and custom values stay inline rather than entering the dictionary.
      CHECK_FALSE(storage.library().dictionary().findId("Unseen résumé title"));
      CHECK_FALSE(storage.library().dictionary().findId("Unseen café"));

      {
        auto snapshot = runtimeLibrary.snapshot();
        CHECK(snapshot.revision() == revision + 1);

        for (auto const trackId : uniqueIds)
        {
          auto const optCredits = snapshot.trackCredits(trackId);
          REQUIRE(optCredits);
          CHECK(*optCredits == replacement);
          CHECK(snapshot.trackField(trackId, TrackField::Title) ==
                TrackFieldRawValue{std::string{"Unseen résumé title"}});
          CHECK(snapshot.trackCustomMetadataValue(trackId, "Unseen clé") == "Unseen café");
        }
      }

      auto const committedSize = storage.library().dictionary().size();
      auto const committedGeneration = storage.library().dictionary().generation();
      auto const equivalentPatch = MetadataPatch{
        .optTitle = "Unseen re\u0301sume\u0301 title",
        .optCredits =
          CreditReplacement{.kinds = std::bitset<library::kCreditKindCount>{0b1000},
                            .entries = {{.name = " \tUnseen Rene\u0301 \r\n", .role = " \tUnseen Flu\u0302te \v\f"}}},
        .customUpdates = {{"Unseen cle\u0301", "Unseen cafe\u0301"}},
      };
      auto const noOpRes = runPatch(*res->optNextTargets, equivalentPatch);
      REQUIRE(noOpRes);
      CHECK(noOpRes->status == AuthoringStatus::NoOp);
      CHECK(std::invoke(metadataChanges, noOpRes->reply).empty());
      CHECK_FALSE(noOpRes->optNextTargets);
      CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
      CHECK(runtimeLibrary.snapshot().revision() == revision + 1);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision + 1);
      CHECK(storage.library().dictionary().size() == committedSize);
      CHECK(storage.library().dictionary().generation() == committedGeneration);
      CHECK(publications.size() == 1);
    };

    SECTION("metadata command")
    {
      exercise([&](BoundTrackTargets const& targets, MetadataPatch const& metadata)
               { return fixture.runTask(fixture.commands().updateMetadataAsync(targets, metadata)); },
               &UpdateTrackMetadataReply::changes);
    }

    SECTION("combined metadata and tags command")
    {
      REQUIRE_FALSE(storage.library().dictionary().findId("Previously unseen tag"));
      exercise(
        [&](BoundTrackTargets const& targets, MetadataPatch const& metadata)
        {
          auto res = fixture.runTask(fixture.commands().updatePropertiesAsync(
            targets, TrackPropertiesPatch{.metadata = metadata, .tagsToAdd = {"Previously unseen tag"}}));
          REQUIRE(res);

          if (res->status == AuthoringStatus::Applied)
          {
            REQUIRE(res->reply.tags.changes.size() == uniqueIds.size());

            for (std::size_t index = 0; index < uniqueIds.size(); ++index)
            {
              CHECK(res->reply.tags.changes[index].trackId == uniqueIds[index]);
              CHECK(res->reply.tags.changes[index].addedTags == std::vector<std::string>{"Previously unseen tag"});
              CHECK(res->reply.tags.changes[index].removedTags.empty());
            }
          }
          else
          {
            CHECK(res->reply.tags.changes.empty());
          }

          CHECK(runtimeLibrary.snapshot().selectionTags(uniqueIds) ==
                std::vector<std::string>{"Previously unseen tag"});
          return res;
        },
        [](UpdateTrackPropertiesReply const& reply) -> std::vector<TrackChangeRecord> const&
        { return reply.metadata.changes; });
    }
  }

  TEST_CASE("TrackTargetAdmission - duplicate targets author an unseen tag once in first-seen order",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "First"});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second"});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto& runtimeLibrary = fixture.library();
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto const composed = std::string{"Unseen r\u00e9sum\u00e9 tag"};
    auto const decomposed = std::string{"Unseen re\u0301sume\u0301 tag"};
    REQUIRE(composed != decomposed);

    auto storedTagNames = [&storage](TrackId const trackId)
    {
      auto transaction = storage.library().readTransaction();
      auto const optView =
        storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      auto const builder = library::TrackBuilder::fromHotView(*optView, storage.library().dictionary());
      auto const& tagNames = builder.tags().names();
      return std::vector<std::string>{tagNames.begin(), tagNames.end()};
    };

    auto exercise = [&](std::span<TrackId const> const orderedIds, std::span<TrackId const> const uniqueIds)
    {
      REQUIRE(orderedIds.size() > uniqueIds.size());
      auto const targetsRes = runtimeLibrary.bindTrackTargets(orderedIds);
      REQUIRE(targetsRes);
      CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
      auto const revision = targetsRes->revision();
      auto const dictionarySize = storage.library().dictionary().size();
      auto const dictionaryGeneration = storage.library().dictionary().generation();
      REQUIRE(dictionarySize > 0);
      // Absent from the committed dictionary, not merely missing on these tracks.
      REQUIRE_FALSE(storage.library().dictionary().findId(composed));
      REQUIRE_FALSE(storage.library().dictionary().findId(decomposed));
      CHECK(publications.empty());

      for (auto const trackId : uniqueIds)
      {
        CHECK(storedTagNames(trackId).empty());
      }

      auto const absentRemoveRes = fixture.runTask(fixture.commands().editTagsAsync(*targetsRes, {}, {decomposed}));
      REQUIRE(absentRemoveRes);
      CHECK(absentRemoveRes->status == AuthoringStatus::NoOp);
      CHECK(absentRemoveRes->reply.changes.empty());
      CHECK_FALSE(absentRemoveRes->optNextTargets);
      CHECK(targetsRes->revision() == revision);
      CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
      CHECK(runtimeLibrary.snapshot().revision() == revision);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision);
      CHECK(storage.library().dictionary().size() == dictionarySize);
      CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
      CHECK_FALSE(storage.library().dictionary().findId(composed));
      CHECK_FALSE(storage.library().dictionary().findId(decomposed));
      CHECK(publications.empty());

      auto const res = fixture.runTask(fixture.commands().editTagsAsync(*targetsRes, {composed}, {}));
      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == uniqueIds.size());

      for (std::size_t index = 0; index < uniqueIds.size(); ++index)
      {
        CHECK(res->reply.changes[index].trackId == uniqueIds[index]);
        CHECK(res->reply.changes[index].addedTags == std::vector<std::string>{composed});
        CHECK(res->reply.changes[index].removedTags.empty());
        CHECK(storedTagNames(uniqueIds[index]) == std::vector<std::string>{composed});
      }

      CHECK(targetsRes->revision() == revision);
      CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
      REQUIRE(res->optNextTargets);
      CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
      CHECK(res->optNextTargets->revision() == revision + 1);
      CHECK(res->optNextTargets->matches(runtimeLibrary.authoringAvailability()));
      CHECK(runtimeLibrary.snapshot().revision() == revision + 1);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision + 1);
      CHECK(runtimeLibrary.snapshot().selectionTags(uniqueIds) == std::vector<std::string>{composed});
      REQUIRE(publications.size() == 1);
      CHECK(publications[0] == LibraryChangeSet{
                                 .libraryRevision = revision + 1,
                                 .tracksMutated = std::vector<TrackId>{uniqueIds.begin(), uniqueIds.end()},
                               });
      CHECK(storage.library().dictionary().size() == dictionarySize + 1);
      CHECK(storage.library().dictionary().generation() == dictionaryGeneration + 1);
      CHECK(storage.library().dictionary().findId(composed).has_value());
      CHECK_FALSE(storage.library().dictionary().findId(decomposed));

      auto const committedSize = storage.library().dictionary().size();
      auto const committedGeneration = storage.library().dictionary().generation();
      auto const noOpRes = fixture.runTask(fixture.commands().editTagsAsync(*res->optNextTargets, {decomposed}, {}));
      REQUIRE(noOpRes);
      CHECK(noOpRes->status == AuthoringStatus::NoOp);
      CHECK(noOpRes->reply.changes.empty());
      CHECK_FALSE(noOpRes->optNextTargets);
      CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
      CHECK(res->optNextTargets->revision() == revision + 1);
      CHECK(runtimeLibrary.snapshot().revision() == revision + 1);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision + 1);
      CHECK(storage.library().dictionary().size() == committedSize);
      CHECK(storage.library().dictionary().generation() == committedGeneration);
      CHECK(publications.size() == 1);
      CHECK(runtimeLibrary.snapshot().selectionTags(uniqueIds) == std::vector<std::string>{composed});

      auto const removeRes = fixture.runTask(fixture.commands().editTagsAsync(*res->optNextTargets, {}, {decomposed}));
      REQUIRE(removeRes);
      CHECK(removeRes->status == AuthoringStatus::Applied);
      REQUIRE(removeRes->reply.changes.size() == uniqueIds.size());

      for (std::size_t index = 0; index < uniqueIds.size(); ++index)
      {
        CHECK(removeRes->reply.changes[index].trackId == uniqueIds[index]);
        CHECK(removeRes->reply.changes[index].addedTags.empty());
        CHECK(removeRes->reply.changes[index].removedTags == std::vector<std::string>{composed});
        CHECK(storedTagNames(uniqueIds[index]).empty());
      }

      CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
      REQUIRE(removeRes->optNextTargets);
      CHECK(std::ranges::equal(removeRes->optNextTargets->trackIds(), orderedIds));
      CHECK(removeRes->optNextTargets->revision() == revision + 2);
      CHECK(removeRes->optNextTargets->matches(runtimeLibrary.authoringAvailability()));
      CHECK(runtimeLibrary.snapshot().revision() == revision + 2);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision + 2);
      CHECK(runtimeLibrary.snapshot().selectionTags(uniqueIds).empty());
      REQUIRE(publications.size() == 2);
      CHECK(publications[1] == LibraryChangeSet{
                                 .libraryRevision = revision + 2,
                                 .tracksMutated = std::vector<TrackId>{uniqueIds.begin(), uniqueIds.end()},
                               });
      CHECK(storage.library().dictionary().size() == committedSize);
      CHECK(storage.library().dictionary().generation() == committedGeneration);
      CHECK(storage.library().dictionary().findId(composed).has_value());

      auto const repeatedRemoveRes =
        fixture.runTask(fixture.commands().editTagsAsync(*removeRes->optNextTargets, {}, {composed}));
      REQUIRE(repeatedRemoveRes);
      CHECK(repeatedRemoveRes->status == AuthoringStatus::NoOp);
      CHECK(repeatedRemoveRes->reply.changes.empty());
      CHECK_FALSE(repeatedRemoveRes->optNextTargets);
      CHECK(std::ranges::equal(removeRes->optNextTargets->trackIds(), orderedIds));
      CHECK(removeRes->optNextTargets->revision() == revision + 2);
      CHECK(runtimeLibrary.snapshot().revision() == revision + 2);
      CHECK(runtimeLibrary.authoringAvailability().libraryRevision == revision + 2);
      CHECK(storage.library().dictionary().size() == committedSize);
      CHECK(storage.library().dictionary().generation() == committedGeneration);
      CHECK(publications.size() == 2);
      CHECK(runtimeLibrary.snapshot().selectionTags(uniqueIds).empty());
    };

    SECTION("one track repeated in the binding")
    {
      auto const orderedIds = std::array{first, first};
      auto const uniqueIds = std::array{first};
      exercise(orderedIds, uniqueIds);
    }

    SECTION("multiple tracks repeated in binding order")
    {
      auto const orderedIds = std::array{second, first, second, first};
      auto const uniqueIds = std::array{second, first};
      exercise(orderedIds, uniqueIds);
    }
  }

  TEST_CASE("TrackTargetAdmission - unique ordered targets replace credits with previously unseen names",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "First"});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second"});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const orderedIds = std::array{second, first};
    auto targetsRes = fixture.library().bindTrackTargets(orderedIds);
    REQUIRE(targetsRes);
    CHECK(std::ranges::equal(targetsRes->trackIds(), orderedIds));
    auto const revision = targetsRes->revision();
    auto const replacement = std::vector<library::Credit>{
      {.name = "New credit", .kind = library::CreditKind::Conductor, .role = "New instrument"},
      {.name = "New credit", .kind = library::CreditKind::Soloist},
      {.name = "Another new credit"}};
    REQUIRE_FALSE(storage.library().dictionary().findId("New credit"));
    REQUIRE_FALSE(storage.library().dictionary().findId("New instrument"));
    REQUIRE_FALSE(storage.library().dictionary().findId("Another new credit"));
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const res = fixture.runTask(fixture.commands().updateMetadataAsync(
      *targetsRes,
      MetadataPatch{.optCredits = CreditReplacement{
                      .kinds = std::bitset<library::kCreditKindCount>{0b1111}, .entries = replacement}}));
    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.changes.size() == 2);
    CHECK(res->reply.changes[0].trackId == second);
    CHECK(res->reply.changes[1].trackId == first);
    REQUIRE(res->optNextTargets);
    CHECK(std::ranges::equal(res->optNextTargets->trackIds(), orderedIds));
    CHECK(res->optNextTargets->revision() == revision + 1);
    CHECK(res->optNextTargets->matches(fixture.library().authoringAvailability()));
    CHECK(publicationCount == 1);
    CHECK(storage.library().dictionary().findId("New credit").has_value());
    CHECK(storage.library().dictionary().findId("New instrument").has_value());
    CHECK(storage.library().dictionary().findId("Another new credit").has_value());
    auto snapshot = fixture.library().snapshot();
    CHECK(snapshot.revision() == revision + 1);

    for (auto const trackId : orderedIds)
    {
      auto const optCredits = snapshot.trackCredits(trackId);
      REQUIRE(optCredits);
      CHECK(*optCredits == replacement);
    }
  }

  TEST_CASE("TrackTargetAdmission - empty and missing targets retain their admission errors",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const existing = storage.addTrack(library::test::TrackSpec{.title = "Existing"});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto& runtimeLibrary = fixture.library();
    auto const revision = runtimeLibrary.snapshot().revision();
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    SECTION("empty set is InvalidInput")
    {
      auto const res = runtimeLibrary.bindTrackTargets(std::span<TrackId const>{});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
    }

    SECTION("invalid ID is NotFound")
    {
      auto const res = runtimeLibrary.bindTrackTargets(std::array{existing, kInvalidTrackId});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("nonexistent ID is NotFound")
    {
      auto const missing = TrackId{std::numeric_limits<std::uint32_t>::max()};
      REQUIRE_FALSE(runtimeLibrary.snapshot().containsTrack(missing));
      auto const res = runtimeLibrary.bindTrackTargets(std::array{existing, missing});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    SECTION("missing target among repeated existing IDs is NotFound")
    {
      auto const res = runtimeLibrary.bindTrackTargets(std::array{existing, existing, kInvalidTrackId});
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
    }

    CHECK(runtimeLibrary.snapshot().revision() == revision);
    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
    CHECK(publicationCount == 0);
  }

  TEST_CASE("TrackTargetAdmission - closing rejects repeated targets as unavailable",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const existing = storage.addTrack(library::test::TrackSpec{.title = "Existing"});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto& runtimeLibrary = fixture.library();
    auto const revision = runtimeLibrary.snapshot().revision();
    runtimeLibrary.beginClosing();
    auto const res = runtimeLibrary.bindTrackTargets(std::array{existing, existing});
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidState);
    CHECK(runtimeLibrary.snapshot().revision() == revision);
  }
} // namespace ao::rt::test
