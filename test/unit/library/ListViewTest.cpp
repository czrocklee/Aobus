// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/Exception.h>
#include <ao/Type.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListView.h>

#include <ranges>
#include <span>
#include <test/unit/library/TestUtils.h>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("ListView - Construct from Data")
  {
    auto const payload = ListBuilder::createNew().serialize();
    auto const view = ListView{payload};
    CHECK(view.tracks().empty());
  }

  TEST_CASE("ListView - Field Accessors")
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

  TEST_CASE("ListView - Manual List with TrackIds")
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

  TEST_CASE("ListView - Smart List with Filter")
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

  TEST_CASE("ListView - Empty Strings")
  {
    auto const payload = ListBuilder::createNew().serialize();
    auto const view = ListView{payload};

    CHECK(view.name().empty());
    CHECK(view.description().empty());
    CHECK(view.filter().empty());
    CHECK(view.isRootParent() == true);
  }

  TEST_CASE("ListView - Invalid Data")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte*>(nullptr), 100};
    REQUIRE_THROWS_AS(ListView{nullSpan}, Exception);

    auto const smallData = std::vector<std::byte>(10);
    REQUIRE_THROWS_AS(ListView{smallData}, Exception);
  }

  TEST_CASE("ListView - isSmart")
  {
    auto const manualPayload = ListBuilder::createNew().name("Manual").serialize();
    auto const manualView = ListView{manualPayload};
    CHECK(manualView.isSmart() == false);

    auto const smartPayload = ListBuilder::createNew().name("Smart").filter("@year > 2020").serialize();
    auto const smartView = ListView{smartPayload};
    CHECK(smartView.isSmart() == true);
  }

  TEST_CASE("ListView - Large trackIds Count")
  {
    auto const payload = ListBuilder::createNew().name("Test").serialize();
    auto const view = ListView{payload};
    CHECK(view.tracks().empty());
  }
} // namespace ao::library::test
