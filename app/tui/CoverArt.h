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

  enum class CoverArtDeliveryMode : std::uint8_t
  {
    Off,
    Blocks,
    Kitty,
  };

  /**
   * @brief The terminal cells one artwork slot occupies in every delivery mode.
   *
   * Blocks paints these cells itself and Kitty reserves them for an image the
   * terminal draws, so the two modes claim the same area and a mode switch
   * cannot move the surrounding layout. The rows are fixed; the columns come
   * from @ref coverArtColumns so the slot is square on the terminal it runs
   * on, and fall back to @ref kCoverArtDefaultColumns when it says nothing.
   */
  constexpr std::int32_t kCoverArtDefaultColumns = 20;
  constexpr std::int32_t kCoverArtRows = 12;
  constexpr double kDefaultCellAspectRatio = 0.60;
  constexpr double kMinimumCellAspectRatio = 0.20;
  constexpr double kMaximumCellAspectRatio = 2.00;
  constexpr std::int32_t kMinimumCoverArtColumns = 12;
  constexpr std::int32_t kMaximumCoverArtColumns = 32;
  constexpr std::uint32_t kKittyCoverArtImageId = 1;
  constexpr std::int32_t kMaximumCoverArtDimension = 8192;
  constexpr std::uint64_t kMaximumCoverArtPixels = 32'000'000;
  constexpr std::size_t kMaximumGeneratedCoverArtBytes = std::size_t{8U} * 1024U * 1024U;

  /**
   * @brief The ratio when @p cellWidth by @p cellHeight is a plausible cell
   *        shape, otherwise @ref kDefaultCellAspectRatio.
   *
   * Every platform query ends here, so the range a terminal has to land in to
   * size artwork is stated once rather than per platform.
   */
  double acceptedCellAspectRatio(double cellWidth, double cellHeight) noexcept;

  /**
   * @brief Queries the terminal cell aspect ratio (width / height) from the
   *        controlling terminal once for session layout.
   *
   * Defined per platform, because the Windows console query needs `windows.h`
   * and its `RGB` macro cannot share a translation unit with `ftxui::Color`.
   */
  double queryTerminalCellAspectRatio() noexcept;

  /**
   * @brief Derives the column count required to render artwork square at
   *        @p rows rows given @p cellAspectRatio.
   */
  std::int32_t coverArtColumns(std::int32_t rows, double cellAspectRatio) noexcept;

  struct CoverArtDecodeLimits final
  {
    std::int32_t maximumDimension = kMaximumCoverArtDimension;
    std::uint64_t maximumPixels = kMaximumCoverArtPixels;
    std::size_t maximumGeneratedBytes = kMaximumGeneratedCoverArtBytes;
  };

  /**
   * Decodes embedded raster artwork supported by stb_image: PNG, JPEG, BMP,
   * GIF, TGA, PSD, HDR, PIC, and
   * PNM. Vector and plugin-defined formats such
   * as SVG, TIFF, and WebP are intentionally not part of the portable
   * TUI
   * decoder contract.
   */
  std::optional<CoverArtRows> decodeCoverArtPreview(std::span<std::byte const> bytes,
                                                    std::size_t columns,
                                                    std::size_t rows,
                                                    CoverArtDecodeLimits limits = {});
  /** Converts the same portable raster formats to a square-cropped PNG. */
  std::optional<std::vector<std::byte>> decodeCoverArtPng(std::span<std::byte const> bytes,
                                                          std::int32_t pixelWidth,
                                                          std::int32_t pixelHeight,
                                                          CoverArtDecodeLimits limits = {});

  std::string kittyDeleteImageEscape(std::uint32_t imageId);
  std::string kittyImageEscape(std::span<std::byte const> pngBytes,
                               std::int32_t columns,
                               std::int32_t rows,
                               std::uint32_t imageId = kKittyCoverArtImageId);

  /// The block artwork alone, or nullptr when @p optPreview holds no transform.
  ftxui::Element renderCoverArtPreview(std::optional<CoverArtRows> const& optPreview);
} // namespace ao::tui
