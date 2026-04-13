// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/ListBuilder.h>
#include <rs/core/ListLayout.h>
#include <rs/core/ListStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <array>
#include <vector>

using rs::core::ListBuilder;
using rs::core::ListStore;
using rs::core::ListView;
using rs::lmdb::Database;
using rs::lmdb::Environment;
using rs::lmdb::ReadTransaction;
using rs::lmdb::WriteTransaction;

TEST_CASE("ListBuilder - smart list")
{
  auto payload = ListBuilder::createNew()
                   .name("My Smart List")
                   .description("A smart list")
                   .filter("@artist = 'Test'")
                   .parentId(rs::core::ListId{17})
                   .serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == true);
  CHECK(view.name() == "My Smart List");
  CHECK(view.filter() == "@artist = 'Test'");
  CHECK(view.parentId() == rs::core::ListId{17});
}

TEST_CASE("ListBuilder - manual list")
{
  auto builder = ListBuilder::createNew().name("My Manual List").description("A manual list");
  builder.tracks().add(rs::core::TrackId{100});
  builder.tracks().add(rs::core::TrackId{200});
  builder.tracks().add(rs::core::TrackId{300});
  auto payload = builder.serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.name() == "My Manual List");
  CHECK(view.tracks().size() == 3);
  CHECK(view.tracks()[0] == rs::core::TrackId{100});
  CHECK(view.tracks()[1] == rs::core::TrackId{200});
  CHECK(view.tracks()[2] == rs::core::TrackId{300});
}

TEST_CASE("ListBuilder - manual list empty trackIds")
{
  auto payload = ListBuilder::createNew().name("Empty List").description("No tracks").serialize();
  auto view = ListView{payload};

  CHECK(view.isSmart() == false);
  CHECK(view.tracks().size() == 0);
  CHECK(view.parentId() == rs::core::ListId{0});
}

TEST_CASE("ListBuilder - parentId round-trip through record and view")
{
  auto builder = ListBuilder::createNew()
                   .name("Nested Smart List")
                   .description("Child list")
                   .filter("$year >= 2021")
                   .parentId(rs::core::ListId{42});

  auto payload = builder.serialize();
  auto view = ListView{payload};
  auto record = ListBuilder::fromView(view).record();

  CHECK(view.parentId() == rs::core::ListId{42});
  CHECK(record.parentId == rs::core::ListId{42});

  auto rebuilt = ListBuilder::fromRecord(record).serialize();
  auto rebuiltView = ListView{rebuilt};
  CHECK(rebuiltView.parentId() == rs::core::ListId{42});
}

TEST_CASE("ListBuilder - manual list round-trip through ListStore")
{
  auto temp = TempDir{};
  auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};

  auto wtxn = WriteTransaction{env};
  auto store = ListStore{Database{wtxn, "lists"}};
  wtxn.commit();

  auto builder = ListBuilder::createNew().name("RoundTrip Test").description("Testing round-trip");
  builder.tracks().add(rs::core::TrackId{42});
  builder.tracks().add(rs::core::TrackId{99});
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
  CHECK(found.tracks()[0] == rs::core::TrackId{42});
  CHECK(found.tracks()[1] == rs::core::TrackId{99});
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
  CHECK(found.tracks().size() == 0);
}

TEST_CASE("ListBuilder - name and description offsets")
{
  auto payload = ListBuilder::createNew().name("Offset Test").description("Desc Here").serialize();
  auto view = ListView{payload};

  CHECK(view.name() == "Offset Test");
  CHECK(view.description() == "Desc Here");
}
