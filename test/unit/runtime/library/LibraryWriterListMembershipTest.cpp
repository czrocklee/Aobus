// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct ListMembershipFixture final
    {
      TrackId addTrack(std::string_view const title) { return storage.addTrack(title); }

      ListId seedList(std::string_view const name,
                      std::string_view const expression,
                      ListId const parentId = kInvalidListId,
                      std::span<TrackId const> const order = {})
      {
        auto transaction = library::test::writeTransaction(storage.library());
        auto builder = library::ListBuilder::makeEmpty().name(name).filter(expression).parentId(parentId);

        for (auto const trackId : order)
        {
          builder.orderTrackIds().add(trackId);
        }

        auto result = storage.library().lists().writer(transaction).create(ao::test::requireValue(builder.serialize()));
        REQUIRE(result);
        REQUIRE(transaction.commit());
        return *result;
      }

      Library& library()
      {
        ensureRuntime();
        return writerFixturePtr->library();
      }

      LibraryWriter& writer()
      {
        ensureRuntime();
        return writerFixturePtr->writer();
      }

      BoundTrackTargets bind(std::span<TrackId const> const trackIds)
      {
        return ao::test::requireValue(library().bindTrackTargets(trackIds));
      }

      void editTags(std::span<TrackId const> const trackIds,
                    std::span<std::string const> const tagsToAdd,
                    std::span<std::string const> const tagsToRemove)
      {
        ensureRuntime();
        REQUIRE(writerFixturePtr->editTags(trackIds, tagsToAdd, tagsToRemove));
      }

      bool hasTag(TrackId const trackId, std::string_view const tag) const
      {
        auto transaction = storage.library().readTransaction();
        auto const optView =
          storage.library().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
        REQUIRE(optView);
        auto builder = library::TrackBuilder::fromHotView(*optView, storage.library().dictionary());
        return std::ranges::contains(builder.tags().names(), tag);
      }

      std::vector<TrackId> storedOrder(ListId const listId) const
      {
        auto transaction = storage.library().readTransaction();
        auto const optView = storage.library().lists().reader(transaction).get(listId);
        REQUIRE(optView);
        return {optView->orderTrackIds().begin(), optView->orderTrackIds().end()};
      }

      bool hasList(ListId const listId) const
      {
        auto transaction = storage.library().readTransaction();
        return storage.library().lists().reader(transaction).get(listId).has_value();
      }

      std::uint64_t revision() const
      {
        auto transaction = storage.library().readTransaction();
        return storage.library().libraryRevision(transaction);
      }

      void clearEvents() { events.clear(); }

      MusicLibraryFixture storage;
      InlineExecutor executor;
      std::unique_ptr<LibraryChanges> changesPtr;
      std::unique_ptr<LibraryWriterFixture> writerFixturePtr;
      std::vector<LibraryChangeSet> events;
      async::Subscription changeSubscription;

    private:
      void ensureRuntime()
      {
        if (writerFixturePtr)
        {
          return;
        }

        auto const transaction = storage.library().readTransaction();
        changesPtr = std::make_unique<LibraryChanges>(executor, storage.library().libraryRevision(transaction));
        writerFixturePtr = std::make_unique<LibraryWriterFixture>(storage.library(), *changesPtr);
        changeSubscription =
          changesPtr->onChanged([this](LibraryChangeSet const& event) noexcept { events.push_back(event); });
      }
    };
  } // namespace

  TEST_CASE("LibraryWriter List membership - Add writes the visible tag without materializing order",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Road Song");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")");
    auto targets = fixture.bind(std::array{trackId});
    fixture.clearEvents();

    auto const result = fixture.writer().addTracksToList(listId, targets);

    REQUIRE(result);
    CHECK(result->status == TrackAuthoringStatus::Applied);
    CHECK(result->reply.listId == listId);
    CHECK(result->reply.listName == "Road Trip");
    CHECK(result->reply.tag == "road-trip");
    REQUIRE(result->reply.tagEdit.changes.size() == 1);
    CHECK(fixture.hasTag(trackId, "road-trip"));
    CHECK(fixture.storedOrder(listId).empty());
    REQUIRE(fixture.events.size() == 1);
    CHECK(fixture.events.front().tracksMutated == std::vector{trackId});
    CHECK(fixture.events.front().listsUpserted.empty());
    CHECK(fixture.events.front().listOrderChanges.empty());
  }

  TEST_CASE("LibraryWriter List membership - Add is idempotent", "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Road Song");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")");
    auto targets = fixture.bind(std::array{trackId});
    auto firstRes = fixture.writer().addTracksToList(listId, targets);
    REQUIRE(firstRes);
    REQUIRE(firstRes->optNextTargets);
    fixture.clearEvents();
    auto const revision = fixture.revision();

    auto const secondRes = fixture.writer().addTracksToList(listId, *firstRes->optNextTargets);

    REQUIRE(secondRes);
    CHECK(secondRes->status == TrackAuthoringStatus::NoOp);
    CHECK(secondRes->reply.tagEdit.changes.empty());
    CHECK(fixture.revision() == revision);
    CHECK(fixture.events.empty());
    CHECK(fixture.storedOrder(listId).empty());
  }

  TEST_CASE("LibraryWriter List membership - nested Add rejects the whole selection outside the parent",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const eligible = fixture.addTrack("Eligible");
    auto const outside = fixture.addTrack("Outside");
    auto const parentId = fixture.seedList("Eligible", "#eligible");
    auto const childId = fixture.seedList("Playlist", "#playlist", parentId);
    auto const eligibleTag = std::array{std::string{"eligible"}};
    fixture.editTags(std::array{eligible}, eligibleTag, {});
    auto targets = fixture.bind(std::array{eligible, outside});
    fixture.clearEvents();

    auto const rejectedRes = fixture.writer().addTracksToList(childId, targets);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::InvalidInput);
    CHECK(rejectedRes.error().message.contains("outside parent List"));
    CHECK_FALSE(fixture.hasTag(eligible, "playlist"));
    CHECK_FALSE(fixture.hasTag(outside, "playlist"));
    CHECK(fixture.events.empty());

    auto eligibleTargets = fixture.bind(std::array{eligible});
    auto const acceptedRes = fixture.writer().addTracksToList(childId, eligibleTargets);
    REQUIRE(acceptedRes);
    CHECK(acceptedRes->status == TrackAuthoringStatus::Applied);
    CHECK(fixture.hasTag(eligible, "playlist"));
  }

  TEST_CASE("LibraryWriter List membership - explicit Remove forgets rank and tag in one revision",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const selected = fixture.addTrack("Selected");
    auto const hidden = fixture.addTrack("Hidden");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")", kInvalidListId, std::array{hidden, selected});
    auto const roadTrip = std::array{std::string{"road-trip"}};
    fixture.editTags(std::array{selected}, roadTrip, {});
    auto targets = fixture.bind(std::array{selected});
    fixture.clearEvents();
    auto const beforeRevision = fixture.revision();

    auto const result = fixture.writer().removeTracksFromList(listId, targets);

    REQUIRE(result);
    CHECK(result->status == TrackAuthoringStatus::Applied);
    CHECK(result->reply.tag == "road-trip");
    CHECK(result->reply.forgottenPositionTrackIds == std::vector{selected});
    CHECK_FALSE(fixture.hasTag(selected, "road-trip"));
    CHECK(fixture.storedOrder(listId) == std::vector{hidden});
    CHECK(fixture.revision() == beforeRevision + 1);
    REQUIRE(fixture.events.size() == 1);
    CHECK(fixture.events.front().tracksMutated == std::vector{selected});
    CHECK(fixture.events.front().listsUpserted == std::vector{listId});
    REQUIRE(fixture.events.front().listOrderChanges.size() == 1);
    CHECK(fixture.events.front().listOrderChanges.front().listId == listId);
  }

  TEST_CASE("LibraryWriter List membership - generic tag edits retain hidden rank",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Remember Me");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")", kInvalidListId, std::array{trackId});
    auto const roadTrip = std::array{std::string{"road-trip"}};

    fixture.editTags(std::array{trackId}, roadTrip, {});
    fixture.editTags(std::array{trackId}, {}, roadTrip);
    CHECK(fixture.storedOrder(listId) == std::vector{trackId});
    fixture.editTags(std::array{trackId}, roadTrip, {});
    CHECK(fixture.storedOrder(listId) == std::vector{trackId});
  }

  TEST_CASE("LibraryWriter List membership - computed expressions reject direct membership edits",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Computed");
    auto const listId = fixture.seedList("Computed", R"(#"road-trip" and $year >= 2020)");
    auto targets = fixture.bind(std::array{trackId});

    auto const result = fixture.writer().addTracksToList(listId, targets);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(result.error().message.contains("membership is computed"));
  }

  TEST_CASE("LibraryWriter List membership - preview validates without committing",
            "[runtime][unit][library][list-membership]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Preview");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")");
    std::ignore = fixture.library();
    fixture.clearEvents();
    auto const revision = fixture.revision();

    auto const result = fixture.writer().previewAddTracksToList(listId, std::array{trackId});

    REQUIRE(result);
    REQUIRE(result->tagEdit.changes.size() == 1);
    CHECK_FALSE(fixture.hasTag(trackId, "road-trip"));
    CHECK(fixture.revision() == revision);
    CHECK(fixture.events.empty());
  }

  TEST_CASE("LibraryWriter List deletion - optional visible-tag cleanup is previewed and committed atomically",
            "[runtime][unit][list-membership][list-delete]")
  {
    auto fixture = ListMembershipFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const listId = fixture.seedList("Road Trip", R"(#"road-trip")");
    auto const referencingListId = fixture.seedList("Road Mix", R"(#"road-trip" or #instrumental)");
    auto const roadTrip = std::array{std::string{"road-trip"}};
    fixture.editTags(std::array{first, second}, roadTrip, {});
    fixture.clearEvents();

    auto const options = DeleteListOptions{.removeWritableTagFromTracks = true};
    auto const previewRes = fixture.writer().previewDeleteList(listId, options);

    REQUIRE(previewRes);
    REQUIRE(previewRes->optTagImpact);
    CHECK(previewRes->optTagImpact->tag == "road-trip");
    CHECK(previewRes->optTagImpact->taggedTrackCount == 2);
    CHECK(previewRes->optTagImpact->removedFromTrackCount == 2);
    REQUIRE(previewRes->optTagImpact->otherListReferences.size() == 1);
    CHECK(previewRes->optTagImpact->otherListReferences.front().listId == referencingListId);
    CHECK(fixture.hasList(listId));
    CHECK(fixture.hasTag(first, "road-trip"));
    CHECK(fixture.events.empty());

    auto const committedRes = fixture.writer().deleteList(listId, options);

    REQUIRE(committedRes);
    CHECK(*committedRes == *previewRes);
    REQUIRE(committedRes->optTagImpact);
    CHECK(committedRes->optTagImpact->removedFromTrackCount == 2);
    CHECK_FALSE(fixture.hasList(listId));
    CHECK(fixture.hasList(referencingListId));
    CHECK_FALSE(fixture.hasTag(first, "road-trip"));
    CHECK_FALSE(fixture.hasTag(second, "road-trip"));
    REQUIRE(fixture.events.size() == 1);
    CHECK(fixture.events.front().tracksMutated == std::vector{first, second});
    CHECK(fixture.events.front().listsDeleted == std::vector{listId});
  }

  TEST_CASE("LibraryWriter - List subtree deletion cleans only the root writable tag",
            "[runtime][regression][list-membership][list-delete]")
  {
    auto fixture = ListMembershipFixture{};
    auto const trackId = fixture.addTrack("Nested");
    auto const rootId = fixture.seedList("Root Playlist", R"(#root)");
    auto const childId = fixture.seedList("Nested Playlist", R"(#child)", rootId);
    auto const tags = std::array{std::string{"root"}, std::string{"child"}};
    fixture.editTags(std::array{trackId}, tags, {});
    fixture.clearEvents();
    auto const options = DeleteListOptions{.removeWritableTagFromTracks = true};

    auto const previewRes = fixture.writer().previewDeleteListAndDescendants(rootId, options);

    REQUIRE(previewRes);
    REQUIRE(previewRes->deletedLists.size() == 2);
    REQUIRE(previewRes->deletedLists.front().optTagImpact);
    CHECK(previewRes->deletedLists.front().optTagImpact->tag == "root");
    CHECK(previewRes->deletedLists.front().optTagImpact->removedFromTrackCount == 1);
    CHECK(fixture.hasList(rootId));
    CHECK(fixture.hasList(childId));
    CHECK(fixture.hasTag(trackId, "root"));
    CHECK(fixture.hasTag(trackId, "child"));
    CHECK(fixture.events.empty());

    auto const committedRes = fixture.writer().deleteListAndDescendants(rootId, options);

    REQUIRE(committedRes);
    CHECK(*committedRes == *previewRes);
    CHECK_FALSE(fixture.hasList(rootId));
    CHECK_FALSE(fixture.hasList(childId));
    CHECK_FALSE(fixture.hasTag(trackId, "root"));
    CHECK(fixture.hasTag(trackId, "child"));
    REQUIRE(fixture.events.size() == 1);
    CHECK(fixture.events.front().tracksMutated == std::vector{trackId});
    CHECK(fixture.events.front().listsDeleted == std::vector{rootId, childId});
  }
} // namespace ao::rt::test
