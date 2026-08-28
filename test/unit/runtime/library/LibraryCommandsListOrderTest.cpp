// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct ListOrderWriterFixture final
    {
      TrackId addTrack(std::string_view title) { return storage.addTrack(title); }

      ListId seedList(std::span<TrackId const> orderTrackIds = {},
                      std::string_view expression = {},
                      ListId parentId = kInvalidListId)
      {
        auto transaction = library::test::writeTransaction(storage.library());
        auto builder = library::ListBuilder::makeEmpty().name("Ordered").filter(expression).parentId(parentId);

        for (auto const trackId : orderTrackIds)
        {
          builder.orderTrackIds().add(trackId);
        }

        auto result =
          transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); });
        REQUIRE(result);
        REQUIRE(transaction.commit());
        return *result;
      }

      Library& library()
      {
        ensureRuntime();
        return writerFixturePtr->library();
      }

      LibraryCommands& commands()
      {
        ensureRuntime();
        return writerFixturePtr->commands();
      }

      template<typename T>
      T run(async::Task<T> task)
      {
        ensureRuntime();
        return writerFixturePtr->runTask(std::move(task));
      }

      BoundListOrder bind(ListId listId, std::span<TrackId const> effectiveTrackIds)
      {
        return ao::test::requireValue(library().bindListOrder(listId, effectiveTrackIds));
      }

      std::vector<TrackId> storedOrder(ListId listId)
      {
        auto transaction = storage.library().readTransaction();
        auto const optView = storage.library().lists().reader(transaction).get(listId);
        REQUIRE(optView);
        return {optView->orderTrackIds().begin(), optView->orderTrackIds().end()};
      }

      void clearEvents() { events.clear(); }

      MusicLibraryFixture storage;
      async::LoopExecutor executor;
      std::unique_ptr<LibraryChanges> changesPtr;
      std::unique_ptr<LibraryCommandsFixture> writerFixturePtr;
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
        changesPtr =
          std::make_unique<LibraryChanges>(executor, storage.library().libraryRevision(transaction), "test-library");
        writerFixturePtr = std::make_unique<LibraryCommandsFixture>(storage.library(), *changesPtr, executor);
        changeSubscription =
          changesPtr->onChanged([this](LibraryChangeSet const& event) noexcept { events.push_back(event); });
      }
    };

    delta::RegularTrackEditScript const& onlyOrderScript(ListOrderWriterFixture const& fixture)
    {
      REQUIRE(fixture.events.size() == 1);
      REQUIRE(fixture.events.front().listOrderChanges.size() == 1);
      auto const* script =
        std::get_if<delta::RegularTrackEditScript>(&fixture.events.front().listOrderChanges.front().operation);
      REQUIRE(script != nullptr);
      return *script;
    }
  } // namespace

  TEST_CASE("LibraryCommands List order - valid move lazily materializes visible members and retains hidden ranks",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const hidden = fixture.addTrack("Hidden");
    auto const listId = fixture.seedList(std::array{first, hidden});
    auto binding = fixture.bind(listId, std::array{first, second, third});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{third}, first));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    CHECK(result->reply.selectedTrackIds == std::vector{third});
    CHECK(result->reply.optBeforeTrackId == std::optional{first});
    CHECK(fixture.storedOrder(listId) == std::vector{third, first, hidden, second});
    auto const& script = onlyOrderScript(fixture);
    CHECK_FALSE(script.edits.empty());
  }

  TEST_CASE("LibraryCommands List order - semantic no-op does not materialize an unranked tail",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const listId = fixture.seedList();
    auto binding = fixture.bind(listId, std::array{first, second});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{first}, second));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::NoOp);
    CHECK(fixture.storedOrder(listId).empty());
    CHECK(fixture.events.empty());
  }

  TEST_CASE("LibraryCommands List order - null anchor moves the selection to the raw order tail",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const listId = fixture.seedList(std::array{first, second, third});
    auto binding = fixture.bind(listId, std::array{first, second, third});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{second}, std::nullopt));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    CHECK(result->reply.optBeforeTrackId == std::nullopt);
    CHECK(fixture.storedOrder(listId) == std::vector{first, third, second});
    std::ignore = onlyOrderScript(fixture);
  }

  TEST_CASE("LibraryCommands List order - empty selection is a non-materializing no-op",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const listId = fixture.seedList();
    auto binding = fixture.bind(listId, std::array{first, second});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, {}, std::nullopt));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::NoOp);
    CHECK(result->reply.selectedTrackIds.empty());
    CHECK(fixture.storedOrder(listId).empty());
    CHECK(fixture.events.empty());
  }

  TEST_CASE("LibraryCommands List order - multi-selection uses bound effective order rather than request order",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const third = fixture.addTrack("Third");
    auto const fourth = fixture.addTrack("Fourth");
    auto const effective = std::array{first, second, third, fourth};
    auto const listId = fixture.seedList();
    auto binding = fixture.bind(listId, effective);
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{fourth, second}, first));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    CHECK(result->reply.selectedTrackIds == std::vector{second, fourth});
    CHECK(fixture.storedOrder(listId) == std::vector{second, fourth, first, third});
  }

  TEST_CASE("LibraryCommands List order - reset forgets visible and hidden positions",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const visible = fixture.addTrack("Visible");
    auto const hidden = fixture.addTrack("Hidden");
    auto const listId = fixture.seedList(std::array{hidden, visible});
    auto binding = fixture.bind(listId, std::array{visible});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().resetListOrder(binding));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    CHECK(result->reply.forgottenPositionCount == 2);
    CHECK(fixture.storedOrder(listId).empty());
    REQUIRE(fixture.events.size() == 1);
    REQUIRE(fixture.events.front().listOrderChanges.size() == 1);
    CHECK(std::holds_alternative<ListOrderReset>(fixture.events.front().listOrderChanges.front().operation));
  }

  TEST_CASE("LibraryCommands List order - forget hidden prunes only absent members",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const hidden = fixture.addTrack("Hidden");
    auto const listId = fixture.seedList(std::array{second, hidden, first});
    auto binding = fixture.bind(listId, std::array{second, first});
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().forgetHiddenListOrder(binding));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Applied);
    CHECK(result->reply.forgottenPositionCount == 1);
    CHECK(fixture.storedOrder(listId) == std::vector{second, first});
    std::ignore = onlyOrderScript(fixture);
  }

  TEST_CASE("LibraryCommands List order - stale bindings cannot overwrite a newer revision",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const listId = fixture.seedList();
    auto binding = fixture.bind(listId, std::array{first, second});
    REQUIRE(fixture.run(fixture.commands().createList(ListDraft{.name = "Unrelated"})));
    fixture.clearEvents();

    auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{second}, first));

    REQUIRE(result);
    CHECK(result->status == AuthoringStatus::Stale);
    CHECK(fixture.storedOrder(listId).empty());
    CHECK(fixture.events.empty());
  }

  TEST_CASE("LibraryCommands List order - invalid selection and anchor are rejected atomically",
            "[runtime][unit][library][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto const listId = fixture.seedList();

    SECTION("selection outside the bound source")
    {
      auto binding = fixture.bind(listId, std::array{first, second});
      auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{TrackId{9999}}, first));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("selected anchor")
    {
      auto binding = fixture.bind(listId, std::array{first, second});
      auto const result = fixture.run(fixture.commands().moveListOrder(binding, std::vector{first}, first));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    CHECK(fixture.storedOrder(listId).empty());
    CHECK(fixture.events.empty());
  }

  TEST_CASE("LibraryWriteLane - List order binding rejects invalid identities and effective sequences",
            "[runtime][unit][library-authoring][list-order]")
  {
    auto fixture = ListOrderWriterFixture{};
    auto const first = fixture.addTrack("First");
    auto const listId = fixture.seedList();

    SECTION("virtual List")
    {
      auto const result = fixture.library().bindListOrder(kInvalidListId, std::array{first});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("missing saved List")
    {
      auto const result = fixture.library().bindListOrder(ListId{9999}, std::array{first});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("invalid track identity")
    {
      auto const result = fixture.library().bindListOrder(listId, std::array{kInvalidTrackId});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("duplicate track identity")
    {
      auto const result = fixture.library().bindListOrder(listId, std::array{first, first});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    SECTION("missing track")
    {
      auto const result = fixture.library().bindListOrder(listId, std::array{TrackId{9999}});

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
    }

    CHECK(fixture.storedOrder(listId).empty());
    CHECK(fixture.events.empty());
  }
} // namespace ao::rt::test
