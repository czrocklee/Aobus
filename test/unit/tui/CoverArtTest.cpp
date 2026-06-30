// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CoverArt.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::vector<std::byte> onePixelRedPng()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18,
        0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
      });

      auto result = std::vector<std::byte>{};
      result.reserve(kBytes.size());

      for (auto const value : kBytes)
      {
        result.push_back(static_cast<std::byte>(value));
      }

      return result;
    }
  } // namespace

  TEST_CASE("CoverArt - empty input produces no preview", "[tui][unit][cover-art]")
  {
    CHECK_FALSE(decodeCoverArtPreview({}, 4, 2).has_value());
  }

  TEST_CASE("CoverArt - decoded image is sampled into terminal cells", "[tui][unit][cover-art]")
  {
    auto const optPreview = decodeCoverArtPreview(onePixelRedPng(), 3, 2);

    REQUIRE(optPreview.has_value());
    REQUIRE(optPreview->size() == 2);
    REQUIRE((*optPreview)[0].size() == 3);

    auto const& first = (*optPreview)[0][0];
    CHECK(first.topRed > 200);
    CHECK(first.topGreen < 50);
    CHECK(first.topBlue < 50);
    CHECK(first.bottomRed > 200);
    CHECK(first.bottomGreen < 50);
    CHECK(first.bottomBlue < 50);
  }

  TEST_CASE("CoverArt - decoded image can be converted to PNG payload", "[tui][unit][cover-art]")
  {
    auto const optPng = decodeCoverArtPng(onePixelRedPng(), 8, 4);

    REQUIRE(optPng.has_value());
    REQUIRE(optPng->size() > 8);
    CHECK((*optPng)[0] == std::byte{0x89});
    CHECK((*optPng)[1] == std::byte{0x50});
    CHECK((*optPng)[2] == std::byte{0x4E});
    CHECK((*optPng)[3] == std::byte{0x47});
  }

  TEST_CASE("CoverArt - Kitty escape advertises image dimensions", "[tui][unit][cover-art]")
  {
    auto const data = std::vector{std::byte{1}, std::byte{2}, std::byte{3}};
    auto const escape = kittyImageEscape(data, 24, 12);

    CHECK(kittyDeleteVisibleImagesEscape() == "\033_Ga=d,d=A,q=2;\033\\");
    CHECK(kittyDeleteImageEscape(kKittyCoverArtImageId) == "\033_Ga=d,i=1,q=2;\033\\");
    CHECK(escape.starts_with("\033_Ga=T,i=1,f=100,t=d,c=24,r=12,q=2,m=0;"));
    CHECK(escape.ends_with("\033\\"));
  }

  TEST_CASE("CoverArt - Kitty escape splits large payloads into continuation chunks", "[tui][unit][cover-art]")
  {
    auto const data = std::vector<std::byte>(4096, std::byte{0x42});

    auto const escape = kittyImageEscape(data, 24, 12, 99);

    CHECK(escape.starts_with("\033_Ga=T,i=99,f=100,t=d,c=24,r=12,q=2,m=1;"));
    CHECK(escape.find("\033_Gm=0;") != std::string::npos);
    CHECK(escape.ends_with("\033\\"));
  }
} // namespace ao::tui::test
