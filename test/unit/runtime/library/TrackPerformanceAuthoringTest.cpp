// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    library::RecordingDate storedRecordingDate(library::MusicLibrary& storage, TrackId const trackId)
    {
      auto transaction = storage.readTransaction();
      auto const optView =
        storage.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      return optView->performance().recordingDate();
    }

    std::vector<library::Credit> storedPerformers(library::MusicLibrary& storage, TrackId const trackId)
    {
      auto transaction = storage.readTransaction();
      auto const optView =
        storage.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      auto credits = std::vector<library::Credit>{};

      for (auto const& entry : optView->performance().credits(library::CreditKind::Performer))
      {
        credits.push_back(library::Credit{.name = std::string{storage.dictionary().get(entry.nameId)},
                                          .role = std::string{storage.dictionary().getOrDefault(entry.roleId)}});
      }

      return credits;
    }

    std::string storedTitle(library::MusicLibrary& storage, TrackId const trackId)
    {
      auto transaction = storage.readTransaction();
      auto const optView =
        storage.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
      REQUIRE(optView);
      return std::string{optView->metadata().title()};
    }

    std::string storedSoloist(library::MusicLibrary& storage, TrackId const trackId)
    {
      auto transaction = storage.readTransaction();
      auto const optView =
        storage.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      auto const soloists = optView->performance().credits(library::CreditKind::Soloist);
      REQUIRE(soloists.size() == 1);
      CHECK(soloists.front().roleId == kInvalidDictionaryId);
      return std::string{storage.dictionary().get(soloists.front().nameId)};
    }

    library::test::TrackSpec performanceSpec()
    {
      return library::test::TrackSpec{
        .title = "Before",
        .recordingDate = library::RecordingDate{.year = 1981},
        .credits = {{.name = "Old soloist", .kind = library::CreditKind::Soloist},
                    {.name = "Glenn Gould", .role = "Piano"},
                    {.name = "Jane Smith"}},
      };
    }

    CreditReplacement performerReplacement(std::vector<library::Credit> entries)
    {
      return CreditReplacement{.kinds = std::bitset<library::kCreditKindCount>{0b1000}, .entries = std::move(entries)};
    }
  } // namespace

  TEST_CASE("TrackPerformanceAuthoring - recording date replaces the stored precision and reports the change",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto targets = commandsFixture.bind(std::array{trackId});

    auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
      std::move(targets),
      MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1981, .month = 5, .day = 12}}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.changes.size() == 1);
    auto const& record = res->reply.changes[0];
    CHECK(record.trackId == trackId);
    REQUIRE(record.fields.size() == 1);
    CHECK(record.fields[0] ==
          (TrackFieldChange{.field = "recordingDate", .oldValue = "1981", .newValue = "1981-05-12"}));
    CHECK(publicationCount == 1);
    CHECK(storedRecordingDate(storage.library(), trackId) ==
          library::RecordingDate{.year = 1981, .month = 5, .day = 12});
  }

  TEST_CASE("TrackPerformanceAuthoring - recording date clear and exact no-op", "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    // Clearing the stored date is an applied edit with an empty new value.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      REQUIRE(res->reply.changes[0].fields.size() == 1);
      CHECK(res->reply.changes[0].fields[0] ==
            (TrackFieldChange{.field = "recordingDate", .oldValue = "1981", .newValue = ""}));
      CHECK(publicationCount == 1);
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{});
    }

    // Clearing an already-absent date is a semantic no-op.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::NoOp);
      CHECK(res->reply.changes.empty());
      CHECK(publicationCount == 1);
    }

    // Replacing the absent date with the same year precision applies.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1981}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(publicationCount == 2);
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - partial-precision differences are real edits",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    // Year-only versus month precision is a real edit, not a coarse match.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1981, .month = 5}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981, .month = 5});
      CHECK(publicationCount == 1);
    }

    // Coarsening back to year-only is also a real edit.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1981}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
      CHECK(publicationCount == 2);
    }

    // An identical value at the same precision is a semantic no-op.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1981}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::NoOp);
      CHECK(res->reply.changes.empty());
      CHECK(publicationCount == 2);
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - invalid partial dates reject the whole patch atomically",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto const invalidDates = std::vector{
      library::RecordingDate{.year = 1981, .month = 2, .day = 30}, // No February 30.
      library::RecordingDate{.year = 1981, .day = 5},              // A day without a month.
      library::RecordingDate{.month = 5},                          // A month without a year.
      library::RecordingDate{.year = 0, .month = 5, .day = 1},     // Zero year is not a present date.
    };

    for (auto const& date : invalidDates)
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optTitle = "Must roll back", .optRecordingDate = date}));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(publicationCount == 0);
      CHECK(storedTitle(storage.library(), trackId) == "Before");
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - performer scope replaces and clears with order duplicates and roles",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    // A complete Performer segment replacement preserves order, duplicates, and roles.
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets),
        MetadataPatch{.optCredits = performerReplacement(
                        {{.name = "Bob", .role = "Violin"}, {.name = "Bob", .role = "Viola"}, {.name = "Doe"}})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      REQUIRE(res->reply.changes[0].fields.size() == 1);
      CHECK(res->reply.changes[0].fields[0] ==
            (TrackFieldChange{
              .field = "credits",
              .oldValue = "soloist: Old soloist\nperformer: Glenn Gould (Piano)\nperformer: Jane Smith",
              .newValue = "soloist: Old soloist\nperformer: Bob (Violin)\nperformer: Bob (Viola)\nperformer: Doe"}));
      CHECK(publicationCount == 1);
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{
              {.name = "Bob", .role = "Violin"}, {.name = "Bob", .role = "Viola"}, {.name = "Doe"}});
    }

    // An explicit empty list clears the stored list.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optCredits = performerReplacement({})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      REQUIRE(res->reply.changes[0].fields.size() == 1);
      CHECK(res->reply.changes[0].fields[0] ==
            (TrackFieldChange{
              .field = "credits",
              .oldValue = "soloist: Old soloist\nperformer: Bob (Violin)\nperformer: Bob (Viola)\nperformer: Doe",
              .newValue = "soloist: Old soloist"}));
      CHECK(publicationCount == 2);
      CHECK(storedPerformers(storage.library(), trackId).empty());
      CHECK(storedSoloist(storage.library(), trackId) == "Old soloist");
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
    }

    // Clearing an already-empty list is a semantic no-op.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optCredits = performerReplacement({})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::NoOp);
      CHECK(res->reply.changes.empty());
      CHECK(publicationCount == 2);
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - credit no-op includes within-kind order and role presence",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    // The same entries in the same order, including role presence, are a no-op.
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets),
        MetadataPatch{.optCredits =
                        performerReplacement({{.name = "Glenn Gould", .role = "Piano"}, {.name = "Jane Smith"}})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::NoOp);
      CHECK(res->reply.changes.empty());
      CHECK(publicationCount == 0);
    }

    // Reordering the same entries is a real edit.
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets),
        MetadataPatch{.optCredits =
                        performerReplacement({{.name = "Jane Smith"}, {.name = "Glenn Gould", .role = "Piano"}})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(publicationCount == 1);
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{{.name = "Jane Smith"}, {.name = "Glenn Gould", .role = "Piano"}});
    }

    // A role added to an existing entry is a real edit.
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets),
        MetadataPatch{.optCredits = performerReplacement(
                        {{.name = "Jane Smith", .role = "Cello"}, {.name = "Glenn Gould", .role = "Piano"}})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(publicationCount == 2);
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{
              {.name = "Jane Smith", .role = "Cello"}, {.name = "Glenn Gould", .role = "Piano"}});
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - normalized credit no-op leaves dictionary and revision unchanged",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const expected = std::vector<library::Credit>{{.name = "René", .role = "Flûte"}, {.name = "René"}};
    auto spec = performanceSpec();
    spec.credits = expected;
    auto const trackId = storage.addTrack(spec);
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto targets = commandsFixture.bind(std::array{trackId});
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    auto const revision = targets.revision();

    // Decomposed accents, all surrounding ASCII whitespace forms, and a blank
    // role admit to the exact stored ordered list without interning any text.
    auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
      targets,
      MetadataPatch{.optCredits =
                      performerReplacement({{.name = " \tRene\xCC\x81 \r\n", .role = " \tFlu\xCC\x82te \v\f"},
                                            {.name = "\vRene\xCC\x81\f", .role = "\t\r\n "}})}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::NoOp);
    CHECK(res->reply.changes.empty());
    CHECK_FALSE(res->optNextTargets);
    CHECK(publicationCount == 0);
    CHECK(storedPerformers(storage.library(), trackId) == expected);
    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
    CHECK(commandsFixture.library().authoringAvailability().libraryRevision == revision);
    CHECK(targets.matches(commandsFixture.library().authoringAvailability()));
    auto transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == revision);
  }

  TEST_CASE("TrackPerformanceAuthoring - invalid credit text rejects the whole patch atomically",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    auto const invalidLists = std::vector<std::vector<library::Credit>>{
      {{.name = ""}},                                                             // Empty authored name.
      {{.name = "Glenn Gould", .role = "Piano"}, {.name = " ", .role = "Viola"}}, // Blank second name.
      {{.name = std::string{"Glenn\xED\xA0\x80", 8}, .role = "Piano"}},           // Malformed UTF-8 name.
      {{.name = "Glenn Gould", .role = std::string{"\xC0\xAF", 2}}},              // Malformed UTF-8 role.
    };

    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    auto const revision = commandsFixture.library().snapshot().revision();

    for (auto const& credits : invalidLists)
    {
      auto targets = commandsFixture.bind(std::array{trackId});

      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optTitle = "Must roll back", .optCredits = performerReplacement(credits)}));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(publicationCount == 0);
      CHECK(storedTitle(storage.library(), trackId) == "Before");
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{{.name = "Glenn Gould", .role = "Piano"}, {.name = "Jane Smith"}});
      CHECK(storage.library().dictionary().size() == dictionarySize);
      CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
      CHECK(commandsFixture.library().snapshot().revision() == revision);
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - date and scoped credit edits preserve unrelated fields",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};

    // A Performer-only patch keeps the date, Soloist credits, and hot title.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optCredits = performerReplacement({{.name = "Ada", .role = "Piano"}})}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      REQUIRE(res->reply.changes[0].fields.size() == 1);
      CHECK(res->reply.changes[0].fields[0].field == "credits");
      CHECK(storedTitle(storage.library(), trackId) == "Before");
      CHECK(storedSoloist(storage.library(), trackId) == "Old soloist");
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
    }

    // A recording-date-only patch keeps every credit kind.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
        std::move(targets), MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1955}}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      REQUIRE(res->reply.changes.size() == 1);
      REQUIRE(res->reply.changes[0].fields.size() == 1);
      CHECK(res->reply.changes[0].fields[0].field == "recordingDate");
      CHECK(storedSoloist(storage.library(), trackId) == "Old soloist");
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{{.name = "Ada", .role = "Piano"}});
    }

    // An unrelated hot-field patch keeps both new performance fields.
    {
      auto targets = commandsFixture.bind(std::array{trackId});
      auto const res = commandsFixture.runTask(
        commandsFixture.commands().updateMetadataAsync(std::move(targets), MetadataPatch{.optTitle = "After"}));

      REQUIRE(res);
      CHECK(res->status == AuthoringStatus::Applied);
      CHECK(storedTitle(storage.library(), trackId) == "After");
      CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1955});
      CHECK(storedPerformers(storage.library(), trackId) ==
            std::vector<library::Credit>{{.name = "Ada", .role = "Piano"}});
      CHECK(storedSoloist(storage.library(), trackId) == "Old soloist");
    }
  }

  TEST_CASE("TrackPerformanceAuthoring - stale binding applies neither date nor credits",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const trackId = storage.addTrack(performanceSpec());
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto commandsFixture = LibraryCommandsFixture{storage.library(), changes};

    auto staleTargets = commandsFixture.bind(std::array{trackId});

    // An intervening library commit invalidates the retained binding's stamp.
    REQUIRE(commandsFixture.updateMetadata(std::array{trackId}, MetadataPatch{.optTitle = "Intervening"}));

    auto const res = commandsFixture.runTask(commandsFixture.commands().updateMetadataAsync(
      std::move(staleTargets),
      MetadataPatch{.optRecordingDate = library::RecordingDate{.year = 1955},
                    .optCredits = performerReplacement({{.name = "Ada"}})}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Stale);
    CHECK(res->reply.changes.empty());
    CHECK(storedTitle(storage.library(), trackId) == "Intervening");
    CHECK(storedRecordingDate(storage.library(), trackId) == library::RecordingDate{.year = 1981});
    CHECK(storedPerformers(storage.library(), trackId) ==
          std::vector<library::Credit>{{.name = "Glenn Gould", .role = "Piano"}, {.name = "Jane Smith"}});
  }

  TEST_CASE("TrackPerformanceAuthoring - scoped replacement retains each target's unselected segments",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const firstCredits = std::vector<library::Credit>{
      {.name = "Old conductor", .kind = library::CreditKind::Conductor, .role = "Guest"},
      {.name = "First ensemble", .kind = library::CreditKind::Ensemble},
      {.name = "First soloist", .kind = library::CreditKind::Soloist, .role = "Violin"},
      {.name = "First performer", .role = "Cello"},
    };
    auto const secondCredits = std::vector<library::Credit>{
      {.name = "Other conductor", .kind = library::CreditKind::Conductor},
      {.name = "Second ensemble", .kind = library::CreditKind::Ensemble, .role = "Choir"},
      {.name = "Second performer"},
      {.name = "Second performer"},
    };
    auto const first = storage.addTrack(library::test::TrackSpec{.title = "First", .credits = firstCredits});
    auto const second = storage.addTrack(library::test::TrackSpec{.title = "Second", .credits = secondCredits});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto const targets = fixture.bind(std::array{first, second});
    auto replacement = CreditReplacement{};
    auto firstExpected = std::vector<library::Credit>{};
    auto secondExpected = std::vector<library::Credit>{};
    auto firstReport = std::string{};
    auto secondReport = std::string{};

    SECTION("one selected kind")
    {
      replacement = {
        .kinds = std::bitset<library::kCreditKindCount>{0b0001},
        .entries = {{.name = "New conductor", .kind = library::CreditKind::Conductor, .role = "Principal"}}};
      firstExpected = {replacement.entries[0], firstCredits[1], firstCredits[2], firstCredits[3]};
      secondExpected = {replacement.entries[0], secondCredits[1], secondCredits[2], secondCredits[3]};
      firstReport = "conductor: New conductor (Principal)\nensemble: First ensemble\nsoloist: First soloist "
                    "(Violin)\nperformer: First performer (Cello)";
      secondReport = "conductor: New conductor (Principal)\nensemble: Second ensemble (Choir)\nperformer: Second "
                     "performer\nperformer: Second performer";
    }

    SECTION("multiple selected kinds clear an omitted selected segment")
    {
      replacement = {.kinds = std::bitset<library::kCreditKindCount>{0b0011},
                     .entries = {{.name = "New conductor", .kind = library::CreditKind::Conductor}}};
      firstExpected = {replacement.entries[0], firstCredits[2], firstCredits[3]};
      secondExpected = {replacement.entries[0], secondCredits[2], secondCredits[3]};
      firstReport = "conductor: New conductor\nsoloist: First soloist (Violin)\nperformer: First performer (Cello)";
      secondReport = "conductor: New conductor\nperformer: Second performer\nperformer: Second performer";
    }

    SECTION("all kinds replace the whole list in canonical stable order")
    {
      replacement = {.kinds = std::bitset<library::kCreditKindCount>{0b1111},
                     .entries = {{.name = "Performer", .role = "Cello"},
                                 {.name = "Soloist", .kind = library::CreditKind::Soloist, .role = "Violin"},
                                 {.name = "Conductor", .kind = library::CreditKind::Conductor},
                                 {.name = "Ensemble", .kind = library::CreditKind::Ensemble},
                                 {.name = "Performer", .role = "Cello"}}};
      firstExpected = {replacement.entries[2],
                       replacement.entries[3],
                       replacement.entries[1],
                       replacement.entries[0],
                       replacement.entries[4]};
      secondExpected = firstExpected;
      firstReport = "conductor: Conductor\nensemble: Ensemble\nsoloist: Soloist (Violin)\nperformer: Performer "
                    "(Cello)\nperformer: Performer (Cello)";
      secondReport = firstReport;
    }

    SECTION("an empty multi-kind replacement clears only its scope")
    {
      replacement.kinds = std::bitset<library::kCreditKindCount>{0b0101};
      firstExpected = {firstCredits[1], firstCredits[3]};
      secondExpected = {secondCredits[1], secondCredits[2], secondCredits[3]};
      firstReport = "ensemble: First ensemble\nperformer: First performer (Cello)";
      secondReport = "ensemble: Second ensemble (Choir)\nperformer: Second performer\nperformer: Second performer";
    }

    SECTION("an empty all-kind replacement clears everything")
    {
      replacement.kinds.set();
    }

    auto const res =
      fixture.runTask(fixture.commands().updateMetadataAsync(targets, MetadataPatch{.optCredits = replacement}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.changes.size() == 2);
    CHECK(res->reply.changes[0].trackId == first);
    CHECK(res->reply.changes[1].trackId == second);
    REQUIRE(res->reply.changes[0].fields.size() == 1);
    REQUIRE(res->reply.changes[1].fields.size() == 1);
    CHECK(res->reply.changes[0].fields[0].field == "credits");
    CHECK(res->reply.changes[1].fields[0].field == "credits");
    CHECK(res->reply.changes[0].fields[0].newValue == firstReport);
    CHECK(res->reply.changes[1].fields[0].newValue == secondReport);
    CHECK(res->reply.changes[0].fields[0].oldValue ==
          "conductor: Old conductor (Guest)\nensemble: First ensemble\nsoloist: First soloist (Violin)\nperformer: "
          "First performer (Cello)");
    CHECK(res->reply.changes[1].fields[0].oldValue ==
          "conductor: Other conductor\nensemble: Second ensemble (Choir)\nperformer: Second performer\nperformer: "
          "Second performer");
    REQUIRE(res->optNextTargets);
    CHECK(res->optNextTargets->revision() == targets.revision() + 1);
    REQUIRE(publications.size() == 1);
    CHECK(publications[0] ==
          LibraryChangeSet{.libraryRevision = targets.revision() + 1, .tracksMutated = {first, second}});
    auto const snapshot = fixture.library().snapshot();
    CHECK(snapshot.trackCredits(first) == firstExpected);
    CHECK(snapshot.trackCredits(second) == secondExpected);
    CHECK(snapshot.revision() == targets.revision() + 1);
  }

  TEST_CASE("TrackPerformanceAuthoring - canonical grouping and normalized all-kind text are a no-op",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const expected = std::vector<library::Credit>{
      {.name = "René", .kind = library::CreditKind::Conductor, .role = "Flûte"},
      {.name = "Ensemble", .kind = library::CreditKind::Ensemble},
      {.name = "Soloist", .kind = library::CreditKind::Soloist},
      {.name = "René"},
      {.name = "René"},
      {.name = "\u00a0Name\u00a0", .role = "\u00a0Role\u00a0"},
    };
    auto const trackId = storage.addTrack(library::test::TrackSpec{.credits = expected});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const targets = fixture.bind(std::array{trackId});
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });
    auto const patch = MetadataPatch{
      .optCredits = CreditReplacement{
        .kinds = std::bitset<library::kCreditKindCount>{0b1111},
        .entries = {{.name = " \tRene\u0301 \r\n", .role = "\t\r\n "},
                    {.name = " Soloist\v", .kind = library::CreditKind::Soloist},
                    {.name = "\fRene\u0301\t", .kind = library::CreditKind::Conductor, .role = " Flu\u0302te "},
                    {.name = " Ensemble ", .kind = library::CreditKind::Ensemble},
                    {.name = "René"},
                    {.name = "\u00a0Name\u00a0", .role = "\u00a0Role\u00a0"}},
      }};

    auto const res = fixture.runTask(fixture.commands().updateMetadataAsync(targets, patch));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::NoOp);
    CHECK(res->reply.changes.empty());
    CHECK_FALSE(res->optNextTargets);
    CHECK(publicationCount == 0);
    CHECK(fixture.library().snapshot().trackCredits(trackId) == expected);
    CHECK(fixture.library().snapshot().revision() == targets.revision());
    CHECK(targets.matches(fixture.library().authoringAvailability()));
    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
    auto transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == targets.revision());
  }

  TEST_CASE("TrackPerformanceAuthoring - multiplicity and kind changes are real edits",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const original =
      std::vector<library::Credit>{{.name = "Ada", .role = "Piano"}, {.name = "Ada", .role = "Piano"}};
    auto const trackId = storage.addTrack(library::test::TrackSpec{.credits = original});
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto replacement = original;

    SECTION("removing an exact duplicate")
    {
      replacement.pop_back();
    }

    SECTION("adding an exact duplicate")
    {
      replacement.push_back(original[0]);
    }

    SECTION("changing kind with the same name and role")
    {
      replacement[0].kind = library::CreditKind::Soloist;
    }

    auto const targets = fixture.bind(std::array{trackId});
    auto const res = fixture.runTask(fixture.commands().updateMetadataAsync(
      targets,
      MetadataPatch{.optCredits = CreditReplacement{
                      .kinds = std::bitset<library::kCreditKindCount>{0b1111}, .entries = replacement}}));

    REQUIRE(res);
    CHECK(res->status == AuthoringStatus::Applied);
    REQUIRE(res->reply.changes.size() == 1);
    CHECK(res->reply.changes[0].trackId == trackId);
    REQUIRE(res->reply.changes[0].fields.size() == 1);
    CHECK(res->reply.changes[0].fields[0].field == "credits");
    CHECK(fixture.library().snapshot().trackCredits(trackId) == replacement);
    CHECK(fixture.library().snapshot().revision() == targets.revision() + 1);
  }

  TEST_CASE("TrackPerformanceAuthoring - invalid scopes and enum values reject the composed patch without effects",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const spec = performanceSpec();
    auto const trackId = storage.addTrack(spec);
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const targets = fixture.bind(std::array{trackId});
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    auto replacement = CreditReplacement{};
    std::size_t publicationCount = 0;
    [[maybe_unused]] auto subscription =
      changes.onChanged([&publicationCount](LibraryChangeSet const&) noexcept { ++publicationCount; });

    SECTION("empty mask and empty entries")
    {
    }

    SECTION("empty mask with entries")
    {
      replacement.entries = {{.name = "Unseen name"}};
    }

    SECTION("valid entry outside selected scope")
    {
      replacement = {.kinds = std::bitset<library::kCreditKindCount>{0b0001}, .entries = {{.name = "Unseen name"}}};
    }

    SECTION("invalid kind before indexing an all-kind mask")
    {
      replacement = {.kinds = std::bitset<library::kCreditKindCount>{0b1111},
                     .entries = {{.name = "Unseen name", .kind = static_cast<library::CreditKind>(255)}}};
    }

    auto const res = fixture.runTask(
      fixture.commands().updateMetadataAsync(targets,
                                             MetadataPatch{.optTitle = "Must roll back",
                                                           .optRecordingDate = library::RecordingDate{.year = 1955},
                                                           .optCredits = replacement,
                                                           .customUpdates = {{"Unseen key", "Unseen value"}}}));

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InvalidInput);
    CHECK(publicationCount == 0);
    CHECK(storedTitle(storage.library(), trackId) == spec.title);
    CHECK(storedRecordingDate(storage.library(), trackId) == spec.recordingDate);
    CHECK(fixture.library().snapshot().trackCredits(trackId) == spec.credits);
    CHECK_FALSE(fixture.library().snapshot().trackCustomMetadataValue(trackId, "Unseen key"));
    CHECK(fixture.library().snapshot().revision() == targets.revision());
    CHECK(targets.matches(fixture.library().authoringAvailability()));
    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);
    CHECK_FALSE(storage.library().dictionary().findId("Unseen name"));
    CHECK_FALSE(storage.library().dictionary().findId("Unseen key"));
  }

  TEST_CASE("TrackPerformanceAuthoring - later merged-record overflow aborts all staged rows text and publication",
            "[runtime][unit][library-authoring]")
  {
    auto storage = MusicLibraryFixture{};
    auto const firstSpec = performanceSpec();
    auto laterSpec = performanceSpec();
    laterSpec.title = "Later";
    laterSpec.uri = "later.flac";
    laterSpec.customMetadata = {{"Retained key", std::string(65'400, 'x')}};
    auto const first = storage.addTrack(firstSpec);
    auto const later = storage.addTrack(laterSpec);
    auto changes = makeStateOnlyLibraryChanges(storage.library());
    auto fixture = LibraryCommandsFixture{storage.library(), changes};
    auto const targets = fixture.bind(std::array{first, later});
    auto const dictionarySize = storage.library().dictionary().size();
    auto const dictionaryGeneration = storage.library().dictionary().generation();
    auto publications = std::vector<LibraryChangeSet>{};
    [[maybe_unused]] auto subscription = changes.onChanged([&publications](LibraryChangeSet const& changeSet) noexcept
                                                           { publications.push_back(changeSet); });
    auto const replacement = CreditReplacement{
      .kinds = std::bitset<library::kCreditKindCount>{0b0001},
      .entries = std::vector<library::Credit>(
        24, library::Credit{.name = "Unseen conductor", .kind = library::CreditKind::Conductor, .role = "Unseen role"}),
    };

    auto const patch = MetadataPatch{.optTitle = "Must roll back",
                                     .optArtist = "Unseen artist",
                                     .optCredits = replacement,
                                     .customUpdates = {{"Unseen key", "Unseen value"}}};

    SECTION("metadata command")
    {
      auto const res = fixture.runTask(fixture.commands().updateMetadataAsync(targets, patch));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("combined metadata and tags command")
    {
      auto const res = fixture.runTask(fixture.commands().updatePropertiesAsync(
        targets, TrackPropertiesPatch{.metadata = patch, .tagsToAdd = {"Unseen tag"}}));
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::ValueTooLarge);
    }

    CHECK(publications.empty());
    auto const snapshot = fixture.library().snapshot();
    CHECK(snapshot.revision() == targets.revision());
    CHECK(targets.matches(fixture.library().authoringAvailability()));
    CHECK(storedTitle(storage.library(), first) == firstSpec.title);
    CHECK(storedTitle(storage.library(), later) == laterSpec.title);
    CHECK(snapshot.trackField(first, TrackField::Artist) == TrackFieldRawValue{firstSpec.artist});
    CHECK(snapshot.trackField(later, TrackField::Artist) == TrackFieldRawValue{laterSpec.artist});
    CHECK(snapshot.trackCredits(first) == firstSpec.credits);
    CHECK(snapshot.trackCredits(later) == laterSpec.credits);
    CHECK(storedRecordingDate(storage.library(), first) == firstSpec.recordingDate);
    CHECK(storedRecordingDate(storage.library(), later) == laterSpec.recordingDate);
    CHECK(snapshot.trackCustomMetadataValue(later, "Retained key") == laterSpec.customMetadata[0].second);
    CHECK_FALSE(snapshot.trackCustomMetadataValue(first, "Unseen key"));
    CHECK_FALSE(snapshot.trackCustomMetadataValue(later, "Unseen key"));
    CHECK(snapshot.selectionTags(std::array{first}).empty());
    CHECK(snapshot.selectionTags(std::array{later}).empty());
    CHECK(storage.library().dictionary().size() == dictionarySize);
    CHECK(storage.library().dictionary().generation() == dictionaryGeneration);

    for (auto const* text : std::array{"Unseen conductor", "Unseen role", "Unseen artist", "Unseen key", "Unseen tag"})
    {
      CHECK_FALSE(storage.library().dictionary().findId(text));
    }

    auto transaction = storage.library().readTransaction();
    CHECK(storage.library().libraryRevision(transaction) == targets.revision());
  }
} // namespace ao::rt::test
