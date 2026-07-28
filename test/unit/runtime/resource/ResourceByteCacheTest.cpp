// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/resource/ResourceByteCache.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::rt::test
{
  TEST_CASE("ResourceByteCache - bounded entries reject invalid payloads and evict the least recently used id",
            "[runtime][unit][resource-byte]")
  {
    auto cache = ResourceByteCache{2};

    CHECK_FALSE(cache.store(kInvalidResourceId, ResourceBytes{{std::byte{0x00}}}));
    CHECK_FALSE(cache.store(ResourceId{1}, ResourceBytes{}));
    CHECK(cache.store(ResourceId{1}, ResourceBytes{{std::byte{0x01}}}));
    CHECK(cache.store(ResourceId{2}, ResourceBytes{{std::byte{0x02}}}));
    CHECK(cache.store(ResourceId{1}, ResourceBytes{{std::byte{0x0A}}}));

    auto const replaced = cache.cached(ResourceId{1});
    REQUIRE_FALSE(replaced.empty());
    REQUIRE(replaced.view().size() == 1);
    CHECK(replaced.view()[0] == std::byte{0x0A});
    auto const sharedCopy = cache.cached(ResourceId{1});
    CHECK(sharedCopy.view().data() == replaced.view().data());

    CHECK(cache.store(ResourceId{3}, ResourceBytes{{std::byte{0x03}}}));
    CHECK(cache.cachedCount() == 2);
    CHECK_FALSE(cache.cached(ResourceId{1}).empty());
    CHECK(cache.cached(ResourceId{2}).empty());
    CHECK_FALSE(cache.cached(ResourceId{3}).empty());

    cache.reset();
    CHECK(cache.cachedCount() == 0);
    REQUIRE(replaced.view().size() == 1);
    CHECK(replaced.view()[0] == std::byte{0x0A});
  }
} // namespace ao::rt::test
