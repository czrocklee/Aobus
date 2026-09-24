// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include <ao/CoreIds.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("PlaybackSessionYamlSchema - repeated YAML round trips preserve unity and near-unity volume exactly",
            "[runtime][unit][playback-session]")
  {
    for (float const volume : std::array{1.0F, std::nextafter(1.0F, 0.0F), 0.999F})
    {
      auto state =
        PlaybackSessionState{.sourceListId = ListId{1}, .currentTrackId = TrackId{42}, .volume = volume, .muted = true};

      for (std::size_t iteration = 0; iteration < 100U; ++iteration)
      {
        CAPTURE(volume, iteration);
        auto tree = ryml::Tree{yaml::callbacks()};
        REQUIRE(PlaybackSessionYamlSchema{}.serialize(tree.rootref(), state));
        auto const encoded = ryml::emitrs_yaml<std::string>(tree);
        auto decodedTree = ryml::Tree{yaml::callbacks()};
        ryml::parse_in_arena(ryml::to_csubstr(encoded), &decodedTree);
        auto const decodedRes = PlaybackSessionYamlSchema{}.deserialize(decodedTree.rootref(), PlaybackSessionState{});

        REQUIRE(decodedRes);
        // Approximate equality would hide the single-float-step regression.
        CHECK(decodedRes->volume == volume);
        CHECK(decodedRes->muted);
        state = *decodedRes;
      }
    }
  }
} // namespace ao::rt::test
