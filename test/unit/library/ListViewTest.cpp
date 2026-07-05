// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListLayout.h>
#include <ao/library/ListView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("ListView - constructs from serialized data", "[library][unit][list]")
  {
    auto const payload = ListBuilder::createNew().serialize();
    auto const view = ListView{payload};
    CHECK(view.tracks().empty());
  }

  TEST_CASE("ListView - returns serialized field values", "[library][unit][list]")
  {
    auto const payload = ListBuilder::createNew().name("Test").description("Desc").parentId(ListId{9}).serialize();
    auto const view = ListView{payload};

    CHECK(view.tracks().empty());
    CHECK(view.name() == "Test");
    CHECK(view.description() == "Desc");
    CHECK(view.filter().empty());
    CHECK(view.isSmart() == false);
    CHECK(view.parentId() == ListId{9});
    CHECK(view.isRootParent() == false);
  }

  TEST_CASE("ListView - returns track IDs for manual lists", "[library][unit][list]")
  {
    auto builder = ListBuilder::createNew().name("My List").description("Description");
    builder.tracks().add(TrackId{100});
    builder.tracks().add(TrackId{200});
    builder.tracks().add(TrackId{300});
    auto const payload = builder.serialize();
    auto const view = ListView{payload};

    CHECK(view.tracks().size() == 3);
    CHECK_FALSE(view.tracks().empty());
    CHECK(view.name() == "My List");
    CHECK(view.description() == "Description");
    CHECK(view.isSmart() == false);
    CHECK(view.tracks()[0] == TrackId{100});
    CHECK(view.tracks()[1] == TrackId{200});
    CHECK(view.tracks()[2] == TrackId{300});
  }

  TEST_CASE("ListView - returns filters for smart lists", "[library][unit][list]")
  {
    auto const payload =
      ListBuilder::createNew().name("Smart List").description("A smart list").filter("@year > 2020").serialize();
    auto const view = ListView{payload};

    CHECK(view.tracks().empty());
    CHECK(view.name() == "Smart List");
    CHECK(view.description() == "A smart list");
    CHECK(view.filter() == "@year > 2020");
    CHECK(view.isSmart() == true);
  }

  TEST_CASE("ListView - returns empty strings when lengths are zero", "[library][unit][list]")
  {
    auto const payload = ListBuilder::createNew().serialize();
    auto const view = ListView{payload};

    CHECK(view.name().empty());
    CHECK(view.description().empty());
    CHECK(view.filter().empty());
    CHECK(view.isRootParent() == true);
  }

  TEST_CASE("ListView - poisons invalid serialized data", "[library][unit][list]")
  {
    auto expectPoisoned = [](ListView const& view)
    {
      CHECK_FALSE(view.isValid());
      CHECK(view.name().empty());
      CHECK(view.description().empty());
      CHECK(view.filter().empty());
      CHECK(view.tracks().empty());
      CHECK(view.parentId() == kInvalidListId);
    };

    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte*>(nullptr), 100};
    expectPoisoned(ListView{nullSpan});

    auto const smallData = std::vector<std::byte>(10);
    expectPoisoned(ListView{smallData});

    SECTION("track-id array overruns the record")
    {
      auto data = std::vector<std::byte>(kListHeaderSize, std::byte{0});
      auto header = ListHeader{};
      header.trackIdsCount = 4;
      std::memcpy(data.data(), &header, sizeof(ListHeader));
      expectPoisoned(ListView{data});
    }

    SECTION("string extent overruns the record")
    {
      auto data = std::vector<std::byte>(kListHeaderSize, std::byte{0});
      auto header = ListHeader{};
      header.nameOffset = 0;
      header.nameLength = 16;
      std::memcpy(data.data(), &header, sizeof(ListHeader));
      expectPoisoned(ListView{data});
    }
  }

  TEST_CASE("ListView - valid records report isValid", "[library][unit][list]")
  {
    auto const payload = ListBuilder::createNew().name("Test").serialize();
    CHECK(ListView{payload}.isValid());
  }

  TEST_CASE("ListView - isSmart", "[library][unit][list]")
  {
    auto const manualPayload = ListBuilder::createNew().name("Manual").serialize();
    auto const manualView = ListView{manualPayload};
    CHECK(manualView.isSmart() == false);

    auto const smartPayload = ListBuilder::createNew().name("Smart").filter("@year > 2020").serialize();
    auto const smartView = ListView{smartPayload};
    CHECK(smartView.isSmart() == true);
  }

  TEST_CASE("ListView - returns large track ID counts", "[library][unit][list]")
  {
    auto const payload = ListBuilder::createNew().name("Test").serialize();
    auto const view = ListView{payload};
    CHECK(view.tracks().empty());
  }
} // namespace ao::library::test
