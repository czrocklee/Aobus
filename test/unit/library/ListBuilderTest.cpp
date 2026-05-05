// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <array>
#include <vector>

using ao::library::ListBuilder;
using ao::library::ListStore;
using ao::library::ListView;
using ao::lmdb::Database;
using ao::lmdb::Environment;
using ao::lmdb::ReadTransaction;
using ao::lmdb::WriteTransaction;

TEST_CASE("ListBuilder - smart list")
{
  auto payload = ListBuilder::createNew()
                   .name("My Smart List")
                   .description("A smart list")
                   .filter("@artist = 'Test'")
                   .parentId(ao::ListId{17})
                   .serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == true);
  CHECK(view.name() == "My Smart List");
  CHECK(view.filter() == "@artist = 'Test'");
  CHECK(view.parentId() == ao::ListId{17});
}

TEST_CASE("ListBuilder - manual list")
{
  auto builder = ListBuilder::createNew().name("My Manual List").description("A manual list");
  builder.tracks().add(ao::TrackId{100});
  builder.tracks().add(ao::TrackId{200});
  builder.tracks().add(ao::TrackId{300});
  auto payload = builder.serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.name() == "My Manual List");
  CHECK(view.tracks().size() == 3);
  CHECK(view.tracks()[0] == ao::TrackId{100});
  CHECK(view.tracks()[1] == ao::TrackId{200});
  CHECK(view.tracks()[2] == ao::TrackId{300});
}

TEST_CASE("ListBuilder - manual list empty trackIds")
{
  auto payload = ListBuilder::createNew().name("Empty List").description("No tracks").serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.tracks().empty());
  CHECK(view.parentId() == ao::ListId{0});
}

TEST_CASE("ListBuilder - parentId round-trip through View")
{
  auto builder = ListBuilder::createNew()
                   .name("Nested Smart List")
                   .description("Child list")
                   .filter("$year >= 2021")
                   .parentId(ao::ListId{42});

  auto payload = builder.serialize();
  auto view = ListView{payload};

  CHECK(view.parentId() == ao::ListId{42});

  auto rebuilt = ListBuilder::fromView(view).serialize();
  auto rebuiltView = ListView{rebuilt};
  CHECK(rebuiltView.parentId() == ao::ListId{42});
  CHECK(rebuiltView.name() == "Nested Smart List");
  CHECK(rebuiltView.filter() == "$year >= 2021");
}

TEST_CASE("ListBuilder - manual list round-trip through ListStore")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  auto builder = ListBuilder::createNew().name("RoundTrip Test").description("Testing round-trip");
  builder.tracks().add(ao::TrackId{42});
  builder.tracks().add(ao::TrackId{99});
  auto payload = builder.serialize();

  auto wtxn2 = WriteTransaction{env};
  auto [id, createdView] = store.writer(wtxn2).create(payload);
  wtxn2.commit();

  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());

  auto& found = *optFound;
  CHECK(found.isSmart() == false);
  CHECK(found.name() == "RoundTrip Test");
  CHECK(found.tracks().size() == 2);
  CHECK(found.tracks()[0] == ao::TrackId{42});
  CHECK(found.tracks()[1] == ao::TrackId{99});
}

TEST_CASE("ListBuilder - smart list round-trip through ListStore")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  auto payload = ListBuilder::createNew()
                   .name("Smart RoundTrip")
                   .description("Testing smart list round-trip")
                   .filter("@year > 2020")
                   .serialize();

  auto wtxn2 = WriteTransaction{env};
  auto [id, createdView] = store.writer(wtxn2).create(payload);
  wtxn2.commit();

  auto rtxn = ReadTransaction{env};
  auto optFound = store.reader(rtxn).get(id);
  REQUIRE(optFound.has_value());

  auto& found = *optFound;
  CHECK(found.isSmart() == true);
  CHECK(found.name() == "Smart RoundTrip");
  CHECK(found.filter() == "@year > 2020");
  CHECK(found.tracks().empty());
}

TEST_CASE("ListBuilder - name and description offsets")
{
  auto payload = ListBuilder::createNew().name("Offset Test").description("Desc Here").serialize();
  auto view = ListView{payload};

  CHECK(view.name() == "Offset Test");
  CHECK(view.description() == "Desc Here");
}
