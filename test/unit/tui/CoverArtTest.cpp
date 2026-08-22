// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/CoverArt.h"

#include "CoverArtTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    template<std::size_t Size>
    std::vector<std::byte> toBytes(std::array<std::uint8_t, Size> const& values)
    {
      auto result = std::vector<std::byte>{};
      result.reserve(values.size());

      for (auto const value : values)
      {
        result.push_back(static_cast<std::byte>(value));
      }

      return result;
    }

    std::vector<std::byte> onePixelRedBmp()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
      });
      return toBytes(kBytes);
    }

    std::vector<std::byte> onePixelRedGif()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B,
      });
      return toBytes(kBytes);
    }

    std::vector<std::byte> threePixelBmp()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x42, 0x4D, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0C, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
      });
      return toBytes(kBytes);
    }

    std::vector<std::byte> transparentRedTga()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x28, 0x00, 0x00, 0xFF, 0x00,
      });
      return toBytes(kBytes);
    }

    std::vector<std::byte> twoByTwoRedBlueBmp()
    {
      constexpr auto kBytes = std::to_array<std::uint8_t>({
        0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
      });
      return toBytes(kBytes);
    }

    std::uint32_t pngUint32(std::vector<std::byte> const& png, std::size_t const offset)
    {
      return (std::to_integer<std::uint32_t>(png[offset]) << 24U) |
             (std::to_integer<std::uint32_t>(png[offset + 1]) << 16U) |
             (std::to_integer<std::uint32_t>(png[offset + 2]) << 8U) | std::to_integer<std::uint32_t>(png[offset + 3]);
    }
  } // namespace

  TEST_CASE("CoverArt - empty input produces no preview", "[tui][unit][cover-art]")
  {
    CHECK_FALSE(decodeCoverArtPreview({}, 4, 2).has_value());
  }

  TEST_CASE("CoverArt - decoded image is sampled into terminal cells", "[tui][unit][cover-art]")
  {
    auto const optPreview = decodeCoverArtPreview(support::onePixelRedPng(), 3, 2);

    REQUIRE(optPreview);
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

  TEST_CASE("CoverArt - block artwork fills the shared cell geometry", "[tui][unit][cover-art]")
  {
    auto const optPreview = decodeCoverArtPreview(support::onePixelRedPng(),
                                                  static_cast<std::size_t>(kCoverArtDefaultColumns),
                                                  static_cast<std::size_t>(kCoverArtRows));

    REQUIRE(optPreview);
    CHECK(optPreview->size() == static_cast<std::size_t>(kCoverArtRows));
    CHECK(optPreview->front().size() == static_cast<std::size_t>(kCoverArtDefaultColumns));
    CHECK(renderCoverArtPreview(optPreview) != nullptr);
    // Without a transform there is no artwork element to place at all.
    CHECK(renderCoverArtPreview(std::nullopt) == nullptr);
  }

  TEST_CASE("CoverArt - trailing bytes leave a decodable image", "[tui][unit][cover-art]")
  {
    CHECK(decodeCoverArtPreview(support::distinctPng(7), 3, 2).has_value());
  }

  TEST_CASE("CoverArt - embedded BMP and GIF images remain supported", "[tui][unit][cover-art]")
  {
    auto checkRedPreview = [](std::vector<std::byte> const& bytes)
    {
      auto const optPreview = decodeCoverArtPreview(bytes, 1, 1);

      REQUIRE(optPreview);
      REQUIRE(optPreview->size() == 1);
      REQUIRE((*optPreview)[0].size() == 1);
      CHECK((*optPreview)[0][0].topRed > 200);
      CHECK((*optPreview)[0][0].topGreen < 50);
      CHECK((*optPreview)[0][0].topBlue < 50);
    };

    SECTION("BMP")
    {
      checkRedPreview(onePixelRedBmp());
    }

    SECTION("GIF")
    {
      checkRedPreview(onePixelRedGif());
    }
  }

  TEST_CASE("CoverArt - preview centers non-square images and composites transparency", "[tui][unit][cover-art]")
  {
    SECTION("center crop")
    {
      auto const optPreview = decodeCoverArtPreview(threePixelBmp(), 2, 2);

      REQUIRE(optPreview);
      REQUIRE(optPreview->size() == 2);
      REQUIRE((*optPreview)[0].size() == 2);
      CHECK((*optPreview)[0][0].topRed > 200);
      CHECK((*optPreview)[0][0].topGreen < 50);
      CHECK((*optPreview)[0][0].topBlue < 50);
    }

    SECTION("transparent pixels")
    {
      auto const optPreview = decodeCoverArtPreview(transparentRedTga(), 1, 1);

      REQUIRE(optPreview);
      REQUIRE(optPreview->size() == 1);
      REQUIRE((*optPreview)[0].size() == 1);
      CHECK((*optPreview)[0][0].topRed == 18);
      CHECK((*optPreview)[0][0].topGreen == 18);
      CHECK((*optPreview)[0][0].topBlue == 18);
    }
  }

  TEST_CASE("CoverArt - preview downsampling interpolates neighboring colors", "[tui][unit][cover-art]")
  {
    auto const optPreview = decodeCoverArtPreview(twoByTwoRedBlueBmp(), 1, 1);

    REQUIRE(optPreview);
    REQUIRE(optPreview->size() == 1);
    REQUIRE((*optPreview)[0].size() == 1);
    auto const& cell = (*optPreview)[0][0];
    CHECK(cell.topRed > 100);
    CHECK(cell.topGreen < 20);
    CHECK(cell.topBlue > 100);
    CHECK(cell.bottomRed > 100);
    CHECK(cell.bottomGreen < 20);
    CHECK(cell.bottomBlue > 100);
  }

  TEST_CASE("CoverArt - decoded image can be converted to PNG payload", "[tui][unit][cover-art]")
  {
    auto const optPng = decodeCoverArtPng(support::onePixelRedPng(), 8, 4);

    REQUIRE(optPng);
    REQUIRE(optPng->size() >= 24);
    CHECK((*optPng)[0] == std::byte{0x89});
    CHECK((*optPng)[1] == std::byte{0x50});
    CHECK((*optPng)[2] == std::byte{0x4E});
    CHECK((*optPng)[3] == std::byte{0x47});
    CHECK(pngUint32(*optPng, 16) == 8);
    CHECK(pngUint32(*optPng, 20) == 4);
  }

  TEST_CASE("CoverArt - malformed input and invalid dimensions are rejected", "[tui][unit][cover-art]")
  {
    auto const malformed = std::vector{std::byte{0x42}, std::byte{0x4D}};

    CHECK_FALSE(decodeCoverArtPreview(malformed, 1, 1));
    CHECK_FALSE(decodeCoverArtPreview(support::onePixelRedPng(), 0, 1));
    CHECK_FALSE(decodeCoverArtPng(malformed, 1, 1));
    CHECK_FALSE(decodeCoverArtPng(support::onePixelRedPng(), 0, 1));
    CHECK_FALSE(decodeCoverArtPng(support::onePixelRedPng(), 1, -1));
  }

  TEST_CASE("CoverArt - encoded dimensions and generated output are bounded", "[tui][unit][cover-art]")
  {
    auto const source = twoByTwoRedBlueBmp();

    SECTION("exact dimension and pixel limits are accepted")
    {
      auto limits = CoverArtDecodeLimits{.maximumDimension = 2, .maximumPixels = 4};
      CHECK(decodeCoverArtPreview(source, 1, 1, limits));
    }

    SECTION("source dimensions are checked before full decode")
    {
      auto limits = CoverArtDecodeLimits{.maximumDimension = 1};
      CHECK_FALSE(decodeCoverArtPreview(source, 1, 1, limits));
    }

    SECTION("source pixel count is bounded")
    {
      auto limits = CoverArtDecodeLimits{.maximumPixels = 3};
      CHECK_FALSE(decodeCoverArtPng(source, 1, 1, limits));
    }

    SECTION("PNG callback output stops retaining bytes at the configured limit")
    {
      auto limits = CoverArtDecodeLimits{.maximumGeneratedBytes = 8};
      CHECK_FALSE(decodeCoverArtPng(support::onePixelRedPng(), 8, 4, limits));
    }
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
    CHECK(escape.contains("\033_Gm=0;"));
    CHECK(escape.ends_with("\033\\"));
  }

  TEST_CASE("CoverArt - cover columns adapt to cell aspect ratio", "[tui][unit][cover-art]")
  {
    CHECK(coverArtColumns(12, 0.50) == 24);
    CHECK(coverArtColumns(12, 0.60) == 20);
    CHECK(coverArtColumns(12, 0.62) == 19);
    CHECK(coverArtColumns(12, 0.625) == 19);
    CHECK(coverArtColumns(12, 0.40) == 30);
    // In-range aspect ratios that hit the clamp bounds
    CHECK(coverArtColumns(12, 0.30) == kMaximumCoverArtColumns);
    CHECK(coverArtColumns(12, 1.50) == kMinimumCoverArtColumns);
    // Invalid / out-of-bounds ratios fall back to default
    CHECK(coverArtColumns(12, 0.05) == coverArtColumns(12, kDefaultCellAspectRatio));
    CHECK(coverArtColumns(12, 3.00) == coverArtColumns(12, kDefaultCellAspectRatio));
    CHECK(coverArtColumns(12, 0.0) == coverArtColumns(12, kDefaultCellAspectRatio));
    CHECK(coverArtColumns(0, 0.60) == kCoverArtDefaultColumns);
    CHECK(coverArtColumns(-1, 0.60) == kCoverArtDefaultColumns);
  }

  TEST_CASE("CoverArt - implausible cell geometry does not size artwork", "[tui][unit][cover-art]")
  {
    // Every platform query funnels through here, so this is where a terminal
    // reporting nonsense stops mattering.
    CHECK(acceptedCellAspectRatio(10.0, 20.0) == 0.50);
    CHECK(acceptedCellAspectRatio(6.0, 10.0) == 0.60);
    CHECK(acceptedCellAspectRatio(20.0, 10.0) == 2.00);

    // A terminal reporting no pixel geometry leaves a zero dimension behind.
    CHECK(acceptedCellAspectRatio(0.0, 20.0) == kDefaultCellAspectRatio);
    CHECK(acceptedCellAspectRatio(10.0, 0.0) == kDefaultCellAspectRatio);
    CHECK(acceptedCellAspectRatio(-10.0, 20.0) == kDefaultCellAspectRatio);

    // Ratios past the plausible range are rejected rather than clamped, so an
    // absurd report cannot masquerade as an extreme but usable font.
    CHECK(acceptedCellAspectRatio(1.0, 20.0) == kDefaultCellAspectRatio);
    CHECK(acceptedCellAspectRatio(60.0, 10.0) == kDefaultCellAspectRatio);
    CHECK(acceptedCellAspectRatio(kMinimumCellAspectRatio, 1.0) == kMinimumCellAspectRatio);
    CHECK(acceptedCellAspectRatio(kMaximumCellAspectRatio, 1.0) == kMaximumCellAspectRatio);
  }
} // namespace ao::tui::test
