// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("CoverArtRequestModel - selection generation suppresses stale asynchronous results",
            "[uimodel][unit][cover-art]")
  {
    auto model = CoverArtRequestModel{};
    auto const first = model.select(ResourceId{10});
    auto const second = model.select(ResourceId{11});

    CHECK_FALSE(model.accepts(first));
    CHECK(model.accepts(second));
    CHECK_FALSE(model.store(first, {std::byte{0x10}}));
    CHECK(model.store(second, {std::byte{0x11}, std::byte{0x12}}));

    auto const cached = model.cached(ResourceId{11});
    REQUIRE(cached.size() == 2);
    CHECK(cached[0] == std::byte{0x11});
    CHECK(cached[1] == std::byte{0x12});
  }

  TEST_CASE("CoverArtRequestModel - cache evicts the least recently used entry at its limit",
            "[uimodel][unit][cover-art]")
  {
    auto model = CoverArtRequestModel{2};
    auto first = model.select(ResourceId{1});
    REQUIRE(model.store(first, {std::byte{0x01}}));
    auto second = model.select(ResourceId{2});
    REQUIRE(model.store(second, {std::byte{0x02}}));

    REQUIRE_FALSE(model.cached(ResourceId{1}).empty());
    auto third = model.select(ResourceId{3});
    REQUIRE(model.store(third, {std::byte{0x03}}));

    CHECK(model.cachedCount() == 2);
    CHECK_FALSE(model.cached(ResourceId{1}).empty());
    CHECK(model.cached(ResourceId{2}).empty());
    CHECK_FALSE(model.cached(ResourceId{3}).empty());
  }

  TEST_CASE("CoverArtRequestModel - clearing selection rejects an in-flight completion",
            "[uimodel][regression][cover-art][concurrency]")
  {
    auto model = CoverArtRequestModel{};
    auto const inFlight = model.select(ResourceId{42});

    model.clearSelection();

    CHECK(model.selectedResourceId() == kInvalidResourceId);
    CHECK_FALSE(model.accepts(inFlight));
    CHECK_FALSE(model.store(inFlight, {std::byte{0x42}}));
    CHECK(model.cachedCount() == 0);
  }

  TEST_CASE("CoverArtRequestModel - reset separates library resource namespaces",
            "[uimodel][regression][cover-art][concurrency]")
  {
    auto model = CoverArtRequestModel{};
    auto const previousLibrary = model.select(ResourceId{42});
    REQUIRE(model.store(previousLibrary, {std::byte{0x01}}));

    model.reset();
    auto const currentLibrary = model.select(ResourceId{42});

    CHECK(model.cachedCount() == 0);
    CHECK_FALSE(model.accepts(previousLibrary));
    CHECK(model.accepts(currentLibrary));
    CHECK_FALSE(model.store(previousLibrary, {std::byte{0x02}}));
    CHECK(model.store(currentLibrary, {std::byte{0x03}}));
    REQUIRE(model.cached(ResourceId{42}).size() == 1);
    CHECK(model.cached(ResourceId{42})[0] == std::byte{0x03});
  }
} // namespace ao::uimodel::test
