// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  struct CoverArtCell final
  {
    std::uint8_t topRed = 0;
    std::uint8_t topGreen = 0;
    std::uint8_t topBlue = 0;
    std::uint8_t bottomRed = 0;
    std::uint8_t bottomGreen = 0;
    std::uint8_t bottomBlue = 0;
  };

  using CoverArtRows = std::vector<std::vector<CoverArtCell>>;
  constexpr std::uint32_t kKittyCoverArtImageId = 1;

  std::optional<CoverArtRows> decodeCoverArtPreview(std::vector<std::byte> const& bytes,
                                                    std::size_t columns,
                                                    std::size_t rows);
  std::optional<std::vector<std::byte>> decodeCoverArtPng(std::vector<std::byte> const& bytes,
                                                          std::int32_t pixelWidth,
                                                          std::int32_t pixelHeight);

  std::string kittyDeleteVisibleImagesEscape();
  std::string kittyDeleteImageEscape(std::uint32_t imageId);
  std::string kittyImageEscape(std::span<std::byte const> pngBytes,
                               std::int32_t columns,
                               std::int32_t rows,
                               std::uint32_t imageId = kKittyCoverArtImageId);

  ftxui::Element renderCoverArtPreview(std::optional<CoverArtRows> const& optPreview);
} // namespace ao::tui
