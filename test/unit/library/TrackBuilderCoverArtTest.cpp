// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TrackBuilderTestSupport.h"
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("TrackBuilder - cover art builder edits ordered entries", "[library][unit][track][builder][cover]")
  {
    auto builder = TrackBuilder::createNew();
    auto const data = std::array{std::byte{0x12}, std::byte{0x34}};

    builder.coverArt().add(PictureType::BackCover, ResourceId{41}).add(PictureType::FrontCover, data);

    REQUIRE(builder.coverArt().entries().size() == 2);
    CHECK(builder.coverArt().entries()[0].type == PictureType::BackCover);
    CHECK(std::get<ResourceId>(builder.coverArt().entries()[0].source) == ResourceId{41});
    CHECK(std::ranges::equal(std::get<std::span<std::byte const>>(builder.coverArt().entries()[1].source), data));

    builder.coverArt().erase(0);

    REQUIRE(builder.coverArt().entries().size() == 1);
    CHECK(builder.coverArt().entries()[0].type == PictureType::FrontCover);

    builder.coverArt().clear();
    CHECK(builder.coverArt().entries().empty());

    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});
    REQUIRE(builder.coverArt().entries().size() == 1);
    CHECK(std::get<ResourceId>(builder.coverArt().entries()[0].source) == ResourceId{42});
    CHECK(builder.coverArt().entries()[0].type == PictureType::FrontCover);

    builder.coverArt().clear();
    CHECK(builder.coverArt().entries().empty());

    builder.coverArt()
      .add(PictureType::Other, kInvalidResourceId)
      .add(PictureType::Other, std::span<std::byte const>{});
    CHECK(builder.coverArt().entries().empty());
  }

  TEST_CASE("TrackBuilder - aligns cover table after custom values", "[library][unit][track][builder][cover]")
  {
    auto builder = TrackBuilder::createNew();
    builder.property().uri("song.flac");
    builder.customMetadata().add("odd", "abc");
    builder.coverArt().add(PictureType::BackCover, ResourceId{41});
    builder.coverArt().add(PictureType::FrontCover, ResourceId{42});

    auto context = TrackSerializationContext{};
    auto const coldData = context.serializeCold(builder);

    auto const view = TrackView{std::span<std::byte const>{}, coldData};
    REQUIRE(view.coverArt().count() == 2);
    CHECK(view.coverArt().at(0).type == PictureType::BackCover);
    CHECK(view.coverArt().at(1).type == PictureType::FrontCover);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});

    auto iterated = std::vector<CoverArt>{};

    for (auto const cover : view.coverArt())
    {
      iterated.push_back(cover);
    }

    REQUIRE(iterated.size() == 2);
    CHECK(iterated[0].resourceId == ResourceId{41});
    CHECK(iterated[1].resourceId == ResourceId{42});
  }
} // namespace ao::library::test
