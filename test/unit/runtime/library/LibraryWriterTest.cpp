// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryWriter.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Environment.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryReader.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct RawListRecord final
    {
      std::uint32_t listId = 0;
      std::span<std::byte const> payload{};
    };

    void appendRawListRecords(std::filesystem::path const& path, std::span<RawListRecord const> const records)
    {
      auto environment = lmdb::test::openEnvironment(path, {.flags = lmdb::kEnvNoTls, .maxDatabases = 8});
      auto transaction = lmdb::test::beginWriteTransaction(environment);
      auto database = lmdb::test::openDatabase(transaction, "lists");
      auto writer = database.writer(transaction);

      for (auto const& record : records)
      {
        REQUIRE(writer.create(record.listId, record.payload));
      }

      REQUIRE(transaction.commit());
    }

    void seedRawListRecords(std::filesystem::path const& path, std::span<RawListRecord const> const records)
    {
      {
        [[maybe_unused]] auto library = library::test::makeTestMusicLibrary(path, path);
      }

      appendRawListRecords(path, records);
    }
  } // namespace

  TEST_CASE("LibraryWriter - updateMetadata publishes TracksMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const targetIds = std::array{trackId};
    auto const result = writerFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "New Title"});

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
    REQUIRE(mutated.size() == 1);
    CHECK(mutated[0] == trackId);
  }

  TEST_CASE("LibraryWriter - observers see committed metadata", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    std::size_t observedMutationCount = 0;
    auto observedTrackId = TrackId{kInvalidTrackId};
    std::uint64_t observedRevision = 0;
    auto sub = changes.onChanged(
      [&](LibraryChangeSet const& event) noexcept
      {
        observedMutationCount = event.tracksMutated.size();
        observedTrackId = event.tracksMutated.empty() ? kInvalidTrackId : event.tracksMutated.front();
        observedRevision = event.libraryRevision;
      });

    auto const targetIds = std::array{trackId};
    auto const result = writerFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "Committed Title"});

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
    REQUIRE(observedMutationCount == 1);
    CHECK(observedTrackId == trackId);

    auto transaction = libraryFixture.library().readTransaction();
    CHECK(libraryFixture.library().libraryRevision(transaction) == observedRevision);
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Committed Title");
  }

  TEST_CASE("LibraryWriter - updateMetadata reports no mutation for identical values",
            "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");

    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto const targetIds = std::array{trackId};
    auto const result = writerFixture.updateMetadata(targetIds, MetadataPatch{.optTitle = "Original Title"});

    REQUIRE(result);
    CHECK(result->changes.empty());
    CHECK(mutated.empty());
  }

  TEST_CASE("LibraryWriter - updateMetadata accepts complete metadata patches", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Original Title");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto const targetIds = std::array{trackId};
    auto const patch = MetadataPatch{.optTitle = "New Title",
                                     .optArtist = "Artist",
                                     .optAlbum = "Album",
                                     .optAlbumArtist = "Album Artist",
                                     .optGenre = "Rock",
                                     .optComposer = "Composer",
                                     .optConductor = "Conductor",
                                     .optEnsemble = "Ensemble",
                                     .optWork = "Work",
                                     .optMovement = "Movement",
                                     .optSoloist = "Soloist",
                                     .optYear = 2024,
                                     .optTrackNumber = 1,
                                     .optTrackTotal = 10,
                                     .optDiscNumber = 1,
                                     .optDiscTotal = 2};

    auto const result = writerFixture.updateMetadata(targetIds, patch);
    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    auto const& dictionary = libraryFixture.library().dictionary();
    CHECK(dictionary.get(optView->classical().conductorId()) == "Conductor");
    CHECK(dictionary.get(optView->classical().ensembleId()) == "Ensemble");
    CHECK(dictionary.get(optView->classical().movementId()) == "Movement");
    CHECK(dictionary.get(optView->classical().soloistId()) == "Soloist");
  }

  TEST_CASE("LibraryWriter - updateMetadata applies and removes custom metadata", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto const targetIds = std::array{trackId};

    SECTION("Add/Update custom key")
    {
      auto patch = MetadataPatch{};
      patch.customUpdates["MyKey"] = "MyValue";
      REQUIRE(writerFixture.updateMetadata(targetIds, patch));

      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      auto const custom = optView->customMetadata();
      CHECK(std::ranges::distance(custom) == 1);
      auto const first = *custom.begin();
      CHECK(libraryFixture.library().dictionary().get(first.first) == "MyKey");
      CHECK(first.second == "MyValue");
    }

    SECTION("Remove custom key")
    {
      // First add it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = "Value";
        REQUIRE(writerFixture.updateMetadata(targetIds, patch));
      }

      // Then remove it
      {
        auto patch = MetadataPatch{};
        patch.customUpdates["ToDelete"] = std::nullopt;
        REQUIRE(writerFixture.updateMetadata(targetIds, patch));
      }

      auto const transaction = libraryFixture.library().readTransaction();
      auto const optView =
        libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      CHECK(std::ranges::distance(optView->customMetadata()) == 0);
    }
  }

  TEST_CASE("LibraryWriter - updateMetadata returns storage errors without committing",
            "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto mutated = std::vector<TrackId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& event) noexcept { mutated = event.tracksMutated; });

    auto patch = MetadataPatch{};
    patch.customUpdates["oversized"] = std::string(70'000, 'x');

    auto const result = writerFixture.updateMetadata(std::array{trackId}, patch);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::ValueTooLarge);
    CHECK(result.error().message.contains("Failed to serialize cold track data"));
    CHECK(result.error().message.contains("exceeds uint16_t"));

    CHECK(mutated.empty());

    auto const transaction = libraryFixture.library().readTransaction();
    auto const optView =
      libraryFixture.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    CHECK(std::ranges::distance(optView->customMetadata()) == 0);
  }

  TEST_CASE("LibraryWriter - editTags reports mutations for tag edits", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto const trackId = libraryFixture.addTrack("Track");
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto const trackIdsArr = std::array{trackId};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const toRemove = std::array{std::string{"pop"}};
    auto const result = writerFixture.editTags(trackIdsArr, toAdd, toRemove);

    REQUIRE(result);
    CHECK_FALSE(result->changes.empty());
  }

  TEST_CASE("LibraryWriter - editTags rejects missing targets before mutation", "[runtime][unit][library][tag]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};

    auto const trackIdsArr = std::array{TrackId{999}};
    auto const toAdd = std::array{std::string{"rock"}};
    auto const result = writerFixture.editTags(trackIdsArr, toAdd, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }

  TEST_CASE("LibraryWriter - lists can be created and updated", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "List";
    draft.expression = R"(#favorite)";

    auto const listId = ao::test::requireValue(service.createList(draft));
    CHECK(listId != kInvalidListId);

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    updateDraft.expression = R"(#favorite or #recent)";
    auto const updateRes = service.updateList(updateDraft);
    REQUIRE(updateRes);

    auto const optNode = writerFixture.library().reader().listNode(listId);
    REQUIRE(optNode);
    CHECK(optNode->name == "Updated");
    CHECK(optNode->expression == R"(#favorite or #recent)");
  }

  TEST_CASE("LibraryWriter - updateList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "Original";
    auto const listId = ao::test::requireValue(service.createList(draft));

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { upserted = ev.listsUpserted; });

    auto updateDraft = LibraryWriter::ListDraft{};
    updateDraft.listId = listId;
    updateDraft.name = "Updated";
    auto const updateRes = service.updateList(updateDraft);
    REQUIRE(updateRes);

    REQUIRE(upserted.size() == 1);
    CHECK(upserted[0] == listId);
  }

  TEST_CASE("LibraryWriter - rejects invalid list drafts", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    SECTION("invalid smart filter")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.name = "Invalid";
      draft.expression = "(";

      auto const result = service.createList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("invalid list filter"));
      CHECK(libraryFixture.library().lists().reader(libraryFixture.library().readTransaction()).begin() ==
            library::ListStore::Reader::Iterator{});
    }

    SECTION("empty expression matches the parent source")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.name = "Empty";

      auto const result = service.createList(draft);
      REQUIRE(result);

      auto const optNode = writerFixture.library().reader().listNode(*result);
      REQUIRE(optNode);
      CHECK(optNode->expression.empty());
    }

    SECTION("missing parent")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.name = "Child";
      draft.parentId = ListId{999};

      auto const result = service.createList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent not found"));
    }

    SECTION("self parent")
    {
      auto draft = LibraryWriter::ListDraft{};
      draft.name = "List";
      auto const listId = ao::test::requireValue(service.createList(draft));

      draft.listId = listId;
      draft.parentId = listId;
      auto const result = service.updateList(draft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent cannot be the list itself"));
    }

    SECTION("descendant parent")
    {
      auto parentDraft = LibraryWriter::ListDraft{};
      parentDraft.name = "Parent";
      auto const parentId = ao::test::requireValue(service.createList(parentDraft));

      auto childDraft = LibraryWriter::ListDraft{};
      childDraft.name = "Child";
      childDraft.parentId = parentId;
      auto const childId = ao::test::requireValue(service.createList(childDraft));

      auto grandchildDraft = LibraryWriter::ListDraft{};
      grandchildDraft.name = "Grandchild";
      grandchildDraft.parentId = childId;
      auto const grandchildId = ao::test::requireValue(service.createList(grandchildDraft));

      parentDraft.listId = parentId;
      parentDraft.parentId = grandchildId;
      auto const result = service.updateList(parentDraft);
      REQUIRE(!result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("list parent cannot be a descendant of the list"));
    }
  }

  TEST_CASE("LibraryWriter - updateList skips unchanged drafts", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "List";
    auto const listId = ao::test::requireValue(service.createList(draft));
    draft.listId = listId;

    auto upserted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { upserted = ev.listsUpserted; });

    auto const updateRes = service.updateList(draft);
    REQUIRE(updateRes);
    CHECK(upserted.empty());
  }

  TEST_CASE("LibraryWriter - updateList reports missing lists as NotFound", "[runtime][unit][library][list]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    auto draft = LibraryWriter::ListDraft{};
    draft.listId = ListId{999};
    draft.name = "Missing";

    auto const result = service.updateList(draft);
    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(result.error().message.contains("list not found: 999"));
  }

  TEST_CASE("LibraryWriter - deleteList publishes ListsMutated", "[runtime][unit][library][mutation]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& service = writerFixture.writer();

    auto draft = LibraryWriter::ListDraft{};
    draft.name = "ToDelete";
    auto const listId = ao::test::requireValue(service.createList(draft));

    auto deleted = std::vector<ListId>{};
    auto sub = changes.onChanged([&](LibraryChangeSet const& ev) noexcept { deleted = ev.listsDeleted; });

    REQUIRE(service.deleteList(listId));

    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == listId);
  }

  TEST_CASE("LibraryWriter - subtree delete previews and commits every descendant atomically",
            "[runtime][unit][list-delete][delete-subtree]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto changes = makeStateOnlyLibraryChanges(libraryFixture.library());
    auto writerFixture = LibraryWriterFixture{libraryFixture.library(), changes};
    auto& writer = writerFixture.writer();
    auto const parentId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{.name = "Parent"}));
    auto const childId =
      ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{.parentId = parentId, .name = "Child"}));
    auto const grandchildId =
      ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{.parentId = childId, .name = "Grandchild"}));
    auto const unrelatedId = ao::test::requireValue(writer.createList(LibraryWriter::ListDraft{.name = "Unrelated"}));

    auto const ordinaryDeleteRes = writer.deleteList(parentId);
    REQUIRE_FALSE(ordinaryDeleteRes);
    CHECK(ordinaryDeleteRes.error().code == Error::Code::Conflict);
    CHECK(ordinaryDeleteRes.error().message.contains("Child"));

    auto const previewRes = writer.previewDeleteListAndDescendants(parentId);
    REQUIRE(previewRes);
    CHECK(previewRes->rootListId == parentId);
    REQUIRE(previewRes->deletedLists.size() == 3);
    CHECK(previewRes->deletedLists[0].listId == parentId);
    CHECK(previewRes->deletedLists[1].listId == childId);
    CHECK(previewRes->deletedLists[2].listId == grandchildId);
    CHECK(writerFixture.library().reader().listNode(parentId).has_value());
    CHECK(writerFixture.library().reader().listNode(childId).has_value());
    CHECK(writerFixture.library().reader().listNode(grandchildId).has_value());

    auto events = std::vector<LibraryChangeSet>{};
    auto sub = changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });
    auto const result = writer.deleteListAndDescendants(parentId);

    REQUIRE(result);
    CHECK(result->deletedLists == previewRes->deletedLists);
    REQUIRE(events.size() == 1);
    CHECK(events.front().listsDeleted == std::vector{parentId, childId, grandchildId});
    CHECK_FALSE(writerFixture.library().reader().listNode(parentId).has_value());
    CHECK_FALSE(writerFixture.library().reader().listNode(childId).has_value());
    CHECK_FALSE(writerFixture.library().reader().listNode(grandchildId).has_value());
    CHECK(writerFixture.library().reader().listNode(unrelatedId).has_value());
  }

  TEST_CASE("LibraryWriter - subtree delete rejects cycles without deleting any List",
            "[runtime][regression][list-delete][delete-subtree]")
  {
    auto temp = ao::test::TempDir{};
    auto const firstPayload =
      ao::test::requireValue(library::ListBuilder::makeEmpty().parentId(ListId{2}).name("First").serialize());
    auto const secondPayload =
      ao::test::requireValue(library::ListBuilder::makeEmpty().parentId(ListId{1}).name("Second").serialize());
    seedRawListRecords(temp.path(),
                       std::array{RawListRecord{.listId = 1, .payload = firstPayload},
                                  RawListRecord{.listId = 2, .payload = secondPayload}});
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    auto changes = makeStateOnlyLibraryChanges(musicLibrary);
    auto writerFixture = LibraryWriterFixture{musicLibrary, changes};
    auto events = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });

    auto const result = writerFixture.writer().deleteListAndDescendants(ListId{1});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
    CHECK(result.error().message.contains("cycle"));
    auto transaction = musicLibrary.readTransaction();
    auto reader = musicLibrary.lists().reader(transaction);
    CHECK(reader.get(ListId{1}).has_value());
    CHECK(reader.get(ListId{2}).has_value());
    CHECK(events.empty());
  }

  TEST_CASE("LibraryWriter - corrupt List iteration aborts subtree deletion without partial state",
            "[runtime][regression][list-delete][delete-subtree]")
  {
    auto temp = ao::test::TempDir{};
    auto const rootPayload = ao::test::requireValue(library::ListBuilder::makeEmpty().name("Root").serialize());
    auto const childPayload =
      ao::test::requireValue(library::ListBuilder::makeEmpty().parentId(ListId{1}).name("Child").serialize());
    auto const corruptPayload = std::array<std::byte, 4>{};
    seedRawListRecords(temp.path(),
                       std::array{RawListRecord{.listId = 1, .payload = rootPayload},
                                  RawListRecord{.listId = 2, .payload = childPayload}});
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path());
    appendRawListRecords(temp.path(), std::array{RawListRecord{.listId = 3, .payload = corruptPayload}});
    auto changes = makeStateOnlyLibraryChanges(musicLibrary);
    auto writerFixture = LibraryWriterFixture{musicLibrary, changes};
    auto events = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&events](LibraryChangeSet const& event) noexcept { events.push_back(event); });

    CHECK_THROWS_AS(std::ignore = writerFixture.writer().deleteListAndDescendants(ListId{1}), Exception);

    auto transaction = musicLibrary.readTransaction();
    auto reader = musicLibrary.lists().reader(transaction);
    auto const optRoot = reader.get(ListId{1});
    auto const optChild = reader.get(ListId{2});
    REQUIRE(optRoot);
    REQUIRE(optChild);
    CHECK(optRoot->name() == "Root");
    CHECK(optChild->parentId() == ListId{1});
    CHECK(events.empty());
  }
} // namespace ao::rt::test
