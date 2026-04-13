// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/Exception.h>
#include <rs/core/ListBuilder.h>
#include <rs/core/ListView.h>
#include <rs/core/Type.h>

#include <ranges>
#include <span>
#include <test/unit/core/TestUtils.h>
#include <vector>

namespace
{
#if defined(__GNUC__) && !defined(__clang__)
  static_assert(std::ranges::view<rs::core::ListView::TrackProxy>);
#endif

  using namespace test;
  using rs::core::ListView;

  TEST_CASE("ListView - Construct from Data")
  {
    auto payload = rs::core::ListBuilder::createNew().serialize();
    auto view = ListView{payload};
    CHECK(view.tracks().size() == 0);
  }

  TEST_CASE("ListView - Field Accessors")
  {
    auto payload = rs::core::ListBuilder::createNew()
                     .name("Test")
                     .description("Desc")
                     .parentId(rs::core::ListId{9})
                     .serialize();
    auto view = ListView{payload};

    CHECK(view.tracks().size() == 0);
    CHECK(view.name() == "Test");
    CHECK(view.description() == "Desc");
    CHECK(view.filter().empty());
    CHECK(view.isSmart() == false);
    CHECK(view.parentId() == rs::core::ListId{9});
    CHECK(view.isRootParent() == false);
  }

  TEST_CASE("ListView - Manual List with TrackIds")
  {
    auto builder = rs::core::ListBuilder::createNew().name("My List").description("Description");
    builder.tracks().add(rs::core::TrackId{100});
    builder.tracks().add(rs::core::TrackId{200});
    builder.tracks().add(rs::core::TrackId{300});
    auto payload = builder.serialize();
    auto view = ListView{payload};

    CHECK(view.tracks().size() == 3);
    CHECK(view.name() == "My List");
    CHECK(view.description() == "Description");
    CHECK(view.isSmart() == false);
    CHECK(view.tracks()[0] == rs::core::TrackId{100});
    CHECK(view.tracks()[1] == rs::core::TrackId{200});
    CHECK(view.tracks()[2] == rs::core::TrackId{300});
  }

  TEST_CASE("ListView - Smart List with Filter")
  {
    auto payload = rs::core::ListBuilder::createNew()
                     .name("Smart List")
                     .description("A smart list")
                     .filter("@year > 2020")
                     .serialize();
    auto view = ListView{payload};

    CHECK(view.tracks().size() == 0);
    CHECK(view.name() == "Smart List");
    CHECK(view.description() == "A smart list");
    CHECK(view.filter() == "@year > 2020");
    CHECK(view.isSmart() == true);
  }

  TEST_CASE("ListView - Empty Strings")
  {
    auto payload = rs::core::ListBuilder::createNew().serialize();
    auto view = ListView{payload};

    CHECK(view.name().empty());
    CHECK(view.description().empty());
    CHECK(view.filter().empty());
    CHECK(view.isRootParent() == true);
  }

  TEST_CASE("ListView - Invalid Data")
  {
    auto nullSpan = std::span<std::byte const>{static_cast<std::byte*>(nullptr), 100};
    REQUIRE_THROWS_AS(ListView{nullSpan}, rs::Exception);

    auto smallData = std::vector<std::byte>{10};
    REQUIRE_THROWS_AS(ListView{smallData}, rs::Exception);
  }

  TEST_CASE("ListView - isSmart")
  {
    auto manualPayload = rs::core::ListBuilder::createNew().name("Manual").serialize();
    auto manualView = ListView{manualPayload};
    CHECK(manualView.isSmart() == false);

    auto smartPayload = rs::core::ListBuilder::createNew().name("Smart").filter("@year > 2020").serialize();
    auto smartView = ListView{smartPayload};
    CHECK(smartView.isSmart() == true);
  }

  TEST_CASE("ListView - Large trackIds Count")
  {
    auto payload = rs::core::ListBuilder::createNew().name("Test").serialize();
    auto view = ListView{payload};
    CHECK(view.tracks().size() == 0);
  }

} // anonymous namespace
