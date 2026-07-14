// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArt.h"

#include <ao/utility/Base64.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

// stb implementation lives in this translation unit (its only consumer).
// Embedded cover art is not limited to PNG and JPEG, so retain stb's complete
// set of supported image decoders here.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr int kDecodedChannels = 4; // stb decodes to RGBA below
    constexpr auto kKittyPayloadChunkSize = std::size_t{4096};
    constexpr int kPreviewColumns = 30;
    constexpr int kPreviewRows = 16;

    struct Pixel final
    {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
    };

    struct StbiDeleter final
    {
      void operator()(unsigned char* pixels) const noexcept { ::stbi_image_free(pixels); }
    };

    using StbiPixelsPtr = std::unique_ptr<unsigned char, StbiDeleter>;

    struct DecodedImage final
    {
      StbiPixelsPtr pixelsPtr{};
      std::int32_t width = 0;
      std::int32_t height = 0;
    };

    stbi_uc const* asStbiBytes(std::byte const* encodedBytes) noexcept
    {
      // stb's C API accepts an unsigned-byte view over the encoded byte buffer.
      return reinterpret_cast<stbi_uc const*>(encodedBytes); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    std::optional<DecodedImage> decodeImage(std::vector<std::byte> const& bytes)
    {
      if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      {
        return std::nullopt;
      }

      int width = 0;
      int height = 0;
      int sourceChannels = 0;
      auto pixelsPtr = StbiPixelsPtr{::stbi_load_from_memory(asStbiBytes(bytes.data()),
                                                             static_cast<std::int32_t>(bytes.size()),
                                                             &width,
                                                             &height,
                                                             &sourceChannels,
                                                             kDecodedChannels)};

      if (pixelsPtr == nullptr || width <= 0 || height <= 0)
      {
        return std::nullopt;
      }

      return DecodedImage{.pixelsPtr = std::move(pixelsPtr), .width = width, .height = height};
    }

    Pixel compositePixel(unsigned char const* pixel)
    {
      auto red = pixel[0];
      auto green = pixel[1];
      auto blue = pixel[2];
      auto const alpha = static_cast<std::int32_t>(pixel[3]);
      constexpr int kBackground = 18;
      constexpr int kOpaqueAlpha = 255;

      if (alpha < kOpaqueAlpha)
      {
        red = static_cast<unsigned char>(
          ((static_cast<std::int32_t>(red) * alpha) + (kBackground * (kOpaqueAlpha - alpha))) / kOpaqueAlpha);
        green = static_cast<unsigned char>(
          ((static_cast<std::int32_t>(green) * alpha) + (kBackground * (kOpaqueAlpha - alpha))) / kOpaqueAlpha);
        blue = static_cast<unsigned char>(
          ((static_cast<std::int32_t>(blue) * alpha) + (kBackground * (kOpaqueAlpha - alpha))) / kOpaqueAlpha);
      }

      return Pixel{.red = red, .green = green, .blue = blue};
    }

    ftxui::Decorator cellColor(CoverArtCell const& cell)
    {
      return ftxui::color(ftxui::Color::RGB(cell.topRed, cell.topGreen, cell.topBlue)) |
             ftxui::bgcolor(ftxui::Color::RGB(cell.bottomRed, cell.bottomGreen, cell.bottomBlue));
    }
  } // namespace

  std::optional<CoverArtRows> decodeCoverArtPreview(std::vector<std::byte> const& bytes,
                                                    std::size_t const columns,
                                                    std::size_t const rows)
  {
    if (columns == 0 || rows == 0 ||
        columns > static_cast<std::size_t>(std::numeric_limits<int>::max() / kDecodedChannels) ||
        rows > static_cast<std::size_t>(std::numeric_limits<int>::max() / 2))
    {
      return std::nullopt;
    }

    auto const optImage = decodeImage(bytes);

    if (!optImage)
    {
      return std::nullopt;
    }

    auto const cropSide = std::min(optImage->width, optImage->height);

    if (cropSide <= 0)
    {
      return std::nullopt;
    }

    auto const cropX = (optImage->width - cropSide) / 2;
    auto const cropY = (optImage->height - cropSide) / 2;
    auto const sampleHeight = rows * 2;
    auto const inputStride = static_cast<std::ptrdiff_t>(optImage->width) * kDecodedChannels;
    auto const* cropStart = optImage->pixelsPtr.get() + (static_cast<std::ptrdiff_t>(cropY) * inputStride) +
                            (static_cast<std::ptrdiff_t>(cropX) * kDecodedChannels);

    if (columns > std::numeric_limits<std::size_t>::max() / sampleHeight / kDecodedChannels)
    {
      return std::nullopt;
    }

    auto scaled = std::vector<unsigned char>(columns * sampleHeight * kDecodedChannels);

    if (::stbir_resize_uint8_srgb(cropStart,
                                  cropSide,
                                  cropSide,
                                  static_cast<std::int32_t>(inputStride),
                                  scaled.data(),
                                  static_cast<std::int32_t>(columns),
                                  static_cast<std::int32_t>(sampleHeight),
                                  static_cast<std::int32_t>(columns * kDecodedChannels),
                                  STBIR_RGBA) == nullptr)
    {
      return std::nullopt;
    }

    auto preview = CoverArtRows{};
    preview.reserve(rows);

    for (std::size_t row = 0; row < rows; ++row)
    {
      auto previewRow = std::vector<CoverArtCell>{};
      previewRow.reserve(columns);

      for (std::size_t col = 0; col < columns; ++col)
      {
        auto const topOffset = (((row * 2) * columns) + col) * kDecodedChannels;
        auto const bottomOffset = ((((row * 2) + 1) * columns) + col) * kDecodedChannels;
        auto const top = compositePixel(scaled.data() + topOffset);
        auto const bottom = compositePixel(scaled.data() + bottomOffset);

        previewRow.push_back(CoverArtCell{.topRed = top.red,
                                          .topGreen = top.green,
                                          .topBlue = top.blue,
                                          .bottomRed = bottom.red,
                                          .bottomGreen = bottom.green,
                                          .bottomBlue = bottom.blue});
      }

      preview.push_back(std::move(previewRow));
    }

    return preview;
  }

  std::optional<std::vector<std::byte>> decodeCoverArtPng(std::vector<std::byte> const& bytes,
                                                          std::int32_t const pixelWidth,
                                                          std::int32_t const pixelHeight)
  {
    if (pixelWidth <= 0 || pixelHeight <= 0 || pixelWidth > std::numeric_limits<int>::max() / kDecodedChannels)
    {
      return std::nullopt;
    }

    auto const optImage = decodeImage(bytes);

    if (!optImage)
    {
      return std::nullopt;
    }

    auto const cropSide = std::min(optImage->width, optImage->height);

    if (cropSide <= 0)
    {
      return std::nullopt;
    }

    auto const cropX = (optImage->width - cropSide) / 2;
    auto const cropY = (optImage->height - cropSide) / 2;
    auto const inputStride = static_cast<std::ptrdiff_t>(optImage->width) * kDecodedChannels;
    auto const* cropStart = optImage->pixelsPtr.get() + (static_cast<std::ptrdiff_t>(cropY) * inputStride) +
                            (static_cast<std::ptrdiff_t>(cropX) * kDecodedChannels);

    auto const scaledWidth = static_cast<std::size_t>(pixelWidth);
    auto const scaledHeight = static_cast<std::size_t>(pixelHeight);

    if (scaledWidth > std::numeric_limits<std::size_t>::max() / scaledHeight / kDecodedChannels)
    {
      return std::nullopt;
    }

    auto scaled = std::vector<unsigned char>(scaledWidth * scaledHeight * kDecodedChannels);

    if (::stbir_resize_uint8_srgb(cropStart,
                                  cropSide,
                                  cropSide,
                                  static_cast<std::int32_t>(inputStride),
                                  scaled.data(),
                                  pixelWidth,
                                  pixelHeight,
                                  pixelWidth * kDecodedChannels,
                                  STBIR_RGBA) == nullptr)
    {
      return std::nullopt;
    }

    auto png = std::vector<std::byte>{};
    // stb owns this callback signature and requires its exact C `int` size.
    // NOLINTNEXTLINE(aobus-modernize-use-std-numbers)
    auto const appendChunk = [](void* context, void* data, int size)
    {
      auto* out = static_cast<std::vector<std::byte>*>(context);
      auto const chunk = std::span{static_cast<std::byte const*>(data), static_cast<std::size_t>(size)};
      out->insert(out->end(), chunk.begin(), chunk.end());
    };

    if (::stbi_write_png_to_func(
          appendChunk, &png, pixelWidth, pixelHeight, kDecodedChannels, scaled.data(), pixelWidth * kDecodedChannels) ==
        0)
    {
      return std::nullopt;
    }

    return png;
  }

  std::string kittyDeleteVisibleImagesEscape()
  {
    return "\033_Ga=d,d=A,q=2;\033\\";
  }

  std::string kittyDeleteImageEscape(std::uint32_t const imageId)
  {
    return std::format("\033_Ga=d,i={},q=2;\033\\", imageId);
  }

  std::string kittyImageEscape(std::span<std::byte const> pngBytes,
                               std::int32_t const columns,
                               std::int32_t const rows,
                               std::uint32_t const imageId)
  {
    if (pngBytes.empty() || columns <= 0 || rows <= 0)
    {
      return {};
    }

    auto encoded = utility::base64Encode(pngBytes);
    auto output = std::string{};
    std::size_t offset = 0;
    bool first = true;

    while (offset < encoded.size())
    {
      auto const chunkSize = std::min(kKittyPayloadChunkSize, encoded.size() - offset);

      if (auto const hasMore = offset + chunkSize < encoded.size(); first)
      {
        output.append(
          std::format("\033_Ga=T,i={},f=100,t=d,c={},r={},q=2,m={};", imageId, columns, rows, hasMore ? 1 : 0));
        first = false;
      }
      else
      {
        output.append(std::format("\033_Gm={};", hasMore ? 1 : 0));
      }

      output.append(encoded, offset, chunkSize);
      output.append("\033\\");
      offset += chunkSize;
    }

    return output;
  }

  ftxui::Element renderCoverArtPreview(std::optional<CoverArtRows> const& optPreview)
  {
    using namespace ftxui;

    if (!optPreview || optPreview->empty())
    {
      return vbox({text("Cover Art") | bold, separator(), text("No cover art") | dim | center}) | border |
             size(WIDTH, EQUAL, kPreviewColumns) | size(HEIGHT, EQUAL, kPreviewRows);
    }

    auto lines = Elements{};
    lines.reserve(optPreview->size());

    for (auto const& row : *optPreview)
    {
      auto cells = Elements{};
      cells.reserve(row.size());

      for (auto const& cell : row)
      {
        cells.push_back(text("▀") | cellColor(cell));
      }

      lines.push_back(hbox(std::move(cells)));
    }

    return vbox({text("Cover Art") | bold, separator(), vbox(std::move(lines)) | center}) | border |
           size(WIDTH, EQUAL, kPreviewColumns) | size(HEIGHT, EQUAL, kPreviewRows);
  }
} // namespace ao::tui
