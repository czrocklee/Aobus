// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <lmdb.h>
#include <test/unit/lmdb/TestUtils.h>

namespace ao::library::test
{
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  TEST_CASE("ListBuilder - smart list")
  {
    auto const payload = ListBuilder::createNew()
                           .name("My Smart List")
                           .description("A smart list")
                           .filter("@artist = 'Test'")
                           .parentId(ListId{17})
                           .serialize();
    auto const view = ListView{payload};

    CHECK(view.isSmart() == true);
    CHECK(view.name() == "My Smart List");
    CHECK(view.filter() == "@artist = 'Test'");
    CHECK(view.parentId() == ListId{17});
  }

  TEST_CASE("ListBuilder - manual list")
  {
    auto builder = ListBuilder::createNew().name("My Manual List").description("A manual list");
    builder.tracks().add(TrackId{100});
    builder.tracks().add(TrackId{200});
    builder.tracks().add(TrackId{300});
    auto const payload = builder.serialize();
    auto const view = ListView{payload};

    CHECK(view.isSmart() == false);
    CHECK(view.name() == "My Manual List");
    CHECK(view.tracks().size() == 3);
    CHECK(view.tracks()[0] == TrackId{100});
    CHECK(view.tracks()[1] == TrackId{200});
    CHECK(view.tracks()[2] == TrackId{300});
  }

  TEST_CASE("ListBuilder - manual list empty trackIds")
  {
    auto const payload = ListBuilder::createNew().name("Empty List").description("No tracks").serialize();
    auto const view = ListView{payload};

    CHECK(view.isSmart() == false);
    CHECK(view.tracks().empty());
    CHECK(view.parentId() == ListId{0});
  }

  TEST_CASE("ListBuilder - parentId round-trip through View")
  {
    auto builder = ListBuilder::createNew()
                     .name("Nested Smart List")
                     .description("Child list")
                     .filter("$year >= 2021")
                     .parentId(ListId{42});

    auto const payload = builder.serialize();
    auto const view = ListView{payload};

    CHECK(view.parentId() == ListId{42});

    auto const rebuilt = ListBuilder::fromView(view).serialize();
    auto const rebuiltView = ListView{rebuilt};
    CHECK(rebuiltView.parentId() == ListId{42});
    CHECK(rebuiltView.name() == "Nested Smart List");
    CHECK(rebuiltView.filter() == "$year >= 2021");
  }

  TEST_CASE("ListBuilder - manual list round-trip through ListStore")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ListStore{Database{wtxn, "lists"}};
    wtxn.commit();

    auto builder = ListBuilder::createNew().name("RoundTrip Test").description("Testing round-trip");
    builder.tracks().add(TrackId{42});
    builder.tracks().add(TrackId{99});
    auto const payload = builder.serialize();

    auto wtxn2 = WriteTransaction{env};
    auto const [id, createdView] = store.writer(wtxn2).create(payload);
    wtxn2.commit();

    auto rtxn = ReadTransaction{env};
    auto const optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound.has_value());

    auto const& found = *optFound;
    CHECK(found.isSmart() == false);
    CHECK(found.name() == "RoundTrip Test");
    CHECK(found.tracks().size() == 2);
    CHECK(found.tracks()[0] == TrackId{42});
    CHECK(found.tracks()[1] == TrackId{99});
  }

  TEST_CASE("ListBuilder - smart list round-trip through ListStore")
  {
    auto const temp = TempDir{};
    auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

    auto wtxn = WriteTransaction{env};
    auto store = ListStore{Database{wtxn, "lists"}};
    wtxn.commit();

    auto const payload = ListBuilder::createNew()
                           .name("Smart RoundTrip")
                           .description("Testing smart list round-trip")
                           .filter("@year > 2020")
                           .serialize();

    auto wtxn2 = WriteTransaction{env};
    auto const [id, createdView] = store.writer(wtxn2).create(payload);
    wtxn2.commit();

    auto rtxn = ReadTransaction{env};
    auto const optFound = store.reader(rtxn).get(id);
    REQUIRE(optFound.has_value());

    auto const& found = *optFound;
    CHECK(found.isSmart() == true);
    CHECK(found.name() == "Smart RoundTrip");
    CHECK(found.filter() == "@year > 2020");
    CHECK(found.tracks().empty());
  }

  TEST_CASE("ListBuilder - name and description offsets")
  {
    auto const payload = ListBuilder::createNew().name("Offset Test").description("Desc Here").serialize();
    auto const view = ListView{payload};

    CHECK(view.name() == "Offset Test");
    CHECK(view.description() == "Desc Here");
  }
} // namespace ao::library::test
