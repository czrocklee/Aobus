// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#ifndef __APPLE__
#include <ao/audio/BackendIds.h>
#endif
#include <ao/audio/BackendProvider.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::audio::test
{
  TEST_CASE("createPlatformBackendProviders - preserves native backend preference order", "[audio][unit][backend]")
  {
    auto providers = createPlatformBackendProviders();

#ifdef _WIN32
    REQUIRE(providers.size() == 1);
    REQUIRE(providers[0] != nullptr);
    CHECK(providers[0]->status().descriptor.id == kBackendWasapi);
#elifdef __linux__
    REQUIRE(providers.size() == 2);
    REQUIRE(providers[0] != nullptr);
    REQUIRE(providers[1] != nullptr);
    CHECK(providers[0]->status().descriptor.id == kBackendPipeWire);
    CHECK(providers[1]->status().descriptor.id == kBackendAlsa);
#elifdef __APPLE__
    // macOS has no CoreAudio provider yet. An empty list is the specified
    // behaviour, not a gap: Player keeps every engine on NullBackend, so the
    // application runs silently instead of refusing to start.
    CHECK(providers.empty());
#else
#error "The platform audio provider expectation is missing"
#endif

    for (auto const& providerPtr : providers)
    {
      providerPtr->shutdown();
    }
  }
} // namespace ao::audio::test
