// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ListWriter.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    template<typename Writer>
    concept AcceptsPreparedList =
      requires(Writer& writer, ListBuilder::Prepared const& prepared) { writer.create(prepared); };

    static_assert(!std::is_copy_constructible_v<ListWriter>);
    static_assert(!AcceptsPreparedList<ListWriter>);

    ListId createList(ListWriter& writer, std::string_view const name, ListId const parentId = kInvalidListId)
    {
      auto const list = ListBuilder::makeEmpty().name(name).parentId(parentId);
      return ao::test::requireValue(writer.create(list));
    }
  } // namespace

  TEST_CASE("ListWriter - create and update validate the live parent graph", "[library][unit][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto rootId = kInvalidListId;
    auto childId = kInvalidListId;

    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.lists();
          rootId = createList(writer, "Root");
          childId = createList(writer, "Child", rootId);
          return {};
        }));
      REQUIRE(transaction.commit());
    }

    auto missingTransaction = writeTransaction(library);
    auto missingParent = ListBuilder::makeEmpty().name("Missing parent").parentId(ListId{childId.raw() + 100});
    auto missingRes =
      missingTransaction.apply([&](LibraryWrite& write) { return write.lists().create(missingParent); });
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().code == Error::Code::InvalidInput);

    auto cycleTransaction = writeTransaction(library);
    auto cycle = ListBuilder::makeEmpty().name("Root").parentId(childId);
    auto cycleRes = cycleTransaction.apply([&](LibraryWrite& write) { return write.lists().update(rootId, cycle); });
    REQUIRE_FALSE(cycleRes);
    CHECK(cycleRes.error().code == Error::Code::InvalidInput);

    auto readTransaction = library.readTransaction();
    auto const optRoot = library.lists().reader(readTransaction).get(rootId);
    auto const optChild = library.lists().reader(readTransaction).get(childId);
    REQUIRE(optRoot);
    REQUIRE(optChild);
    CHECK(optRoot->parentId() == kInvalidListId);
    CHECK(optChild->parentId() == rootId);
  }

  TEST_CASE("ListWriter - update does not upsert a missing id", "[library][regression][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    auto const missingId = ListId{41};
    auto const list = ListBuilder::makeEmpty().name("Missing");

    auto updateRes = transaction.apply([&](LibraryWrite& write) { return write.lists().update(missingId, list); });

    REQUIRE_FALSE(updateRes);
    CHECK(updateRes.error().code == Error::Code::NotFound);
    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.lists().reader(readTransaction).get(missingId));
  }

  TEST_CASE("ListWriter - self-parenting and missing deletion targets return typed errors",
            "[library][unit][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto listId = kInvalidListId;

    {
      auto transaction = writeTransaction(library);
      auto createRes = transaction.apply([&](LibraryWrite& write)
                                         { return write.lists().create(ListBuilder::makeEmpty().name("List")); });
      REQUIRE(createRes);
      listId = *createRes;
      REQUIRE(transaction.commit());
    }

    {
      auto transaction = writeTransaction(library);
      auto selfParent = ListBuilder::makeEmpty().name("List").parentId(listId);
      auto updateRes = transaction.apply([&](LibraryWrite& write) { return write.lists().update(listId, selfParent); });
      REQUIRE_FALSE(updateRes);
      CHECK(updateRes.error().code == Error::Code::InvalidInput);
    }

    {
      auto transaction = writeTransaction(library);
      auto removeRes = transaction.apply([](LibraryWrite& write) { return write.lists().remove(ListId{4242}); });
      REQUIRE_FALSE(removeRes);
      CHECK(removeRes.error().code == Error::Code::NotFound);
    }

    auto transaction = writeTransaction(library);
    auto removeSubtreeRes =
      transaction.apply([](LibraryWrite& write) { return write.lists().removeSubtree(ListId{4242}); });
    REQUIRE_FALSE(removeSubtreeRes);
    CHECK(removeSubtreeRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("ListWriter - leaf deletion conflicts while a child remains", "[library][unit][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto parentId = kInvalidListId;
    auto childId = kInvalidListId;

    {
      auto transaction = writeTransaction(library);
      REQUIRE(transaction.apply(
        [&](LibraryWrite& write) -> Result<>
        {
          auto writer = write.lists();
          parentId = createList(writer, "Parent");
          childId = createList(writer, "Child", parentId);
          return {};
        }));
      REQUIRE(transaction.commit());
    }

    auto conflictTransaction = writeTransaction(library);
    auto removeRes = conflictTransaction.apply([&](LibraryWrite& write) { return write.lists().remove(parentId); });
    REQUIRE_FALSE(removeRes);
    CHECK(removeRes.error().code == Error::Code::Conflict);

    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [&](LibraryWrite& write) -> Result<>
      {
        auto writer = write.lists();
        REQUIRE(writer.get(parentId));
        REQUIRE(writer.get(childId));
        REQUIRE(writer.remove(childId));
        REQUIRE(writer.remove(parentId));
        return {};
      }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK_FALSE(library.lists().reader(readTransaction).get(parentId));
    CHECK_FALSE(library.lists().reader(readTransaction).get(childId));
  }

  TEST_CASE("ListWriter - subtree deletion returns root-first ids and preserves unrelated Lists",
            "[library][unit][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    auto rootId = kInvalidListId;
    auto childId = kInvalidListId;
    auto grandchildId = kInvalidListId;
    auto siblingId = kInvalidListId;
    auto unrelatedId = kInvalidListId;
    auto removedRes = transaction.apply(
      [&](LibraryWrite& write) -> Result<std::vector<ListId>>
      {
        auto writer = write.lists();
        rootId = createList(writer, "Root");
        childId = createList(writer, "Child", rootId);
        grandchildId = createList(writer, "Grandchild", childId);
        siblingId = createList(writer, "Sibling", rootId);
        unrelatedId = createList(writer, "Unrelated");
        return writer.removeSubtree(rootId);
      });

    REQUIRE(removedRes);
    CHECK(*removedRes == std::vector{rootId, childId, grandchildId, siblingId});
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    REQUIRE(library.lists().reader(readTransaction).get(unrelatedId));
    CHECK_FALSE(library.lists().reader(readTransaction).get(rootId));
    CHECK_FALSE(library.lists().reader(readTransaction).get(childId));
    CHECK_FALSE(library.lists().reader(readTransaction).get(grandchildId));
    CHECK_FALSE(library.lists().reader(readTransaction).get(siblingId));
  }

  TEST_CASE("ListWriter - clear removes the complete saved List graph", "[library][unit][list-writer]")
  {
    auto const temp = ao::test::TempDir{};
    auto library = makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto transaction = writeTransaction(library);
    REQUIRE(transaction.apply(
      [](LibraryWrite& write) -> Result<>
      {
        auto writer = write.lists();
        auto const rootId = createList(writer, "Root");
        std::ignore = createList(writer, "Child", rootId);
        return writer.clear();
      }));
    REQUIRE(transaction.commit());

    auto readTransaction = library.readTransaction();
    CHECK(library.lists().reader(readTransaction).begin() == ListStore::Reader::EndSentinel{});
  }
} // namespace ao::library::test
