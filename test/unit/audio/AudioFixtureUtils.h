// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  inline std::filesystem::path requireAudioFixture(std::string_view fileName)
  {
    auto const path = std::filesystem::path{TAG_TEST_DATA_DIR} / fileName;

    if (!std::filesystem::exists(path))
    {
      SKIP("Required audio fixture missing: " << path);
    }

    return path;
  }

  inline std::vector<std::uint8_t> readFileBytes(std::filesystem::path const& path)
  {
    auto ifs = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator{ifs}, std::istreambuf_iterator<char>{}};
  }
} // namespace ao::audio::test
