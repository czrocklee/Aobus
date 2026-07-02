// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoverArt.h"

#include <ao/utility/Base64.h>
#include <ao/utility/ByteView.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-object.h>
#include <glib.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    struct Pixel final
    {
      std::uint8_t red = 0;
      std::uint8_t green = 0;
      std::uint8_t blue = 0;
    };

    struct PixbufDeleter final
    {
      void operator()(GdkPixbuf* pixbuf) const noexcept
      {
        if (pixbuf != nullptr)
        {
          ::g_object_unref(pixbuf);
        }
      }
    };

    struct LoaderDeleter final
    {
      void operator()(GdkPixbufLoader* loader) const noexcept
      {
        if (loader != nullptr)
        {
          ::g_object_unref(loader);
        }
      }
    };

    struct ErrorDeleter final
    {
      void operator()(GError* error) const noexcept
      {
        if (error != nullptr)
        {
          ::g_error_free(error);
        }
      }
    };

    using PixbufPtr = std::unique_ptr<GdkPixbuf, PixbufDeleter>;
    using LoaderPtr = std::unique_ptr<GdkPixbufLoader, LoaderDeleter>;
    using ErrorPtr = std::unique_ptr<GError, ErrorDeleter>;
    using BufferPtr = std::unique_ptr<gchar, decltype(&g_free)>;

    constexpr auto kKittyPayloadChunkSize = std::size_t{4096};
    constexpr int kPreviewColumns = 30;
    constexpr int kPreviewRows = 16;

    void closeLoaderSilently(GdkPixbufLoader* loader)
    {
      auto* rawError = static_cast<GError*>(nullptr);
      std::ignore = ::gdk_pixbuf_loader_close(loader, &rawError);
      auto errorPtr = ErrorPtr{rawError};
    }

    PixbufPtr decodePixbuf(std::vector<std::byte> const& bytes)
    {
      if (bytes.empty())
      {
        return {};
      }

      auto loaderPtr = LoaderPtr{::gdk_pixbuf_loader_new()};

      if (loaderPtr == nullptr)
      {
        return {};
      }

      auto* rawError = static_cast<GError*>(nullptr);
      auto const byteSpan = std::span<std::byte const>{bytes.data(), bytes.size()};
      auto const* data = utility::bytes::unsignedCharData(byteSpan);

      if (::gdk_pixbuf_loader_write(loaderPtr.get(), data, bytes.size(), &rawError) == FALSE)
      {
        auto errorPtr = ErrorPtr{rawError};
        closeLoaderSilently(loaderPtr.get());
        return {};
      }

      rawError = nullptr;

      if (::gdk_pixbuf_loader_close(loaderPtr.get(), &rawError) == FALSE)
      {
        auto errorPtr = ErrorPtr{rawError};
        return {};
      }

      auto* pixbuf = ::gdk_pixbuf_loader_get_pixbuf(loaderPtr.get());

      if (pixbuf == nullptr)
      {
        return {};
      }

      (::g_object_ref)(pixbuf);
      return PixbufPtr{pixbuf};
    }

    PixbufPtr squareScaledPixbuf(GdkPixbuf* pixbuf, std::int32_t const pixelWidth, std::int32_t const pixelHeight)
    {
      if (pixbuf == nullptr || pixelWidth <= 0 || pixelHeight <= 0)
      {
        return {};
      }

      auto const width = ::gdk_pixbuf_get_width(pixbuf);
      auto const height = ::gdk_pixbuf_get_height(pixbuf);
      auto const cropSide = std::min(width, height);

      if (cropSide <= 0)
      {
        return {};
      }

      auto const cropX = (width - cropSide) / 2;
      auto const cropY = (height - cropSide) / 2;
      auto croppedPtr = PixbufPtr{::gdk_pixbuf_new_subpixbuf(pixbuf, cropX, cropY, cropSide, cropSide)};

      if (croppedPtr == nullptr)
      {
        return {};
      }

      return PixbufPtr{::gdk_pixbuf_scale_simple(croppedPtr.get(), pixelWidth, pixelHeight, GDK_INTERP_BILINEAR)};
    }

    Pixel pixelAt(GdkPixbuf const* pixbuf, std::int32_t const column, std::int32_t const row)
    {
      auto const nChannels = ::gdk_pixbuf_get_n_channels(pixbuf);
      auto const rowStride = ::gdk_pixbuf_get_rowstride(pixbuf);
      auto const hasAlpha = ::gdk_pixbuf_get_has_alpha(pixbuf) == TRUE;
      auto const* pixels = ::gdk_pixbuf_read_pixels(pixbuf);
      auto const pixelOffset =
        (static_cast<std::ptrdiff_t>(row) * rowStride) + (static_cast<std::ptrdiff_t>(column) * nChannels);
      auto const* pixel = pixels + pixelOffset;

      auto red = pixel[0];
      auto green = pixel[1];
      auto blue = pixel[2];

      if (hasAlpha && nChannels >= 4)
      {
        auto const alpha = static_cast<std::int32_t>(pixel[3]);
        constexpr int kBackground = 18;
        constexpr int kOpaqueAlpha = 255;
        red = static_cast<guchar>((static_cast<std::int32_t>(red) * alpha + kBackground * (kOpaqueAlpha - alpha)) /
                                  kOpaqueAlpha);
        green = static_cast<guchar>((static_cast<std::int32_t>(green) * alpha + kBackground * (kOpaqueAlpha - alpha)) /
                                    kOpaqueAlpha);
        blue = static_cast<guchar>((static_cast<std::int32_t>(blue) * alpha + kBackground * (kOpaqueAlpha - alpha)) /
                                   kOpaqueAlpha);
      }

      return Pixel{.red = red, .green = green, .blue = blue};
    }

    Pixel sampleSquare(GdkPixbuf const* pixbuf,
                       std::int32_t const cropX,
                       std::int32_t const cropY,
                       std::int32_t const cropSide,
                       std::size_t const sampleX,
                       std::size_t const sampleY,
                       std::size_t const sampleWidth,
                       std::size_t const sampleHeight)
    {
      auto const column =
        cropX + std::clamp(static_cast<std::int32_t>((sampleX * static_cast<std::size_t>(cropSide)) / sampleWidth),
                           0,
                           cropSide - 1);
      auto const row =
        cropY + std::clamp(static_cast<std::int32_t>((sampleY * static_cast<std::size_t>(cropSide)) / sampleHeight),
                           0,
                           cropSide - 1);
      return pixelAt(pixbuf, column, row);
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
    if (columns == 0 || rows == 0)
    {
      return std::nullopt;
    }

    auto pixbufPtr = decodePixbuf(bytes);

    if (pixbufPtr == nullptr)
    {
      return std::nullopt;
    }

    auto const width = ::gdk_pixbuf_get_width(pixbufPtr.get());
    auto const height = ::gdk_pixbuf_get_height(pixbufPtr.get());
    auto const cropSide = std::min(width, height);

    if (cropSide <= 0)
    {
      return std::nullopt;
    }

    auto const cropX = (width - cropSide) / 2;
    auto const cropY = (height - cropSide) / 2;
    auto const sampleHeight = rows * 2;
    auto preview = CoverArtRows{};
    preview.reserve(rows);

    for (std::size_t row = 0; row < rows; ++row)
    {
      auto previewRow = std::vector<CoverArtCell>{};
      previewRow.reserve(columns);

      for (std::size_t col = 0; col < columns; ++col)
      {
        auto const top = sampleSquare(pixbufPtr.get(), cropX, cropY, cropSide, col, row * 2, columns, sampleHeight);
        auto const bottom =
          sampleSquare(pixbufPtr.get(), cropX, cropY, cropSide, col, (row * 2) + 1, columns, sampleHeight);

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
    auto pixbufPtr = decodePixbuf(bytes);
    auto scaledPtr = squareScaledPixbuf(pixbufPtr.get(), pixelWidth, pixelHeight);

    if (scaledPtr == nullptr)
    {
      return std::nullopt;
    }

    auto* rawBuffer = static_cast<gchar*>(nullptr);
    gsize bufferSize = 0;

    if (auto* rawError = static_cast<GError*>(nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::gdk_pixbuf_save_to_buffer(scaledPtr.get(), &rawBuffer, &bufferSize, "png", &rawError, nullptr) == FALSE)
    {
      auto errorPtr = ErrorPtr{rawError};
      return std::nullopt;
    }

    auto bufferPtr = BufferPtr{rawBuffer, ::g_free};
    auto result = std::vector<std::byte>{};
    result.reserve(bufferSize);

    auto const bufferBytes = std::as_bytes(std::span{bufferPtr.get(), bufferSize});
    result.insert(result.end(), bufferBytes.begin(), bufferBytes.end());
    return result;
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

      output.append(encoded.substr(offset, chunkSize));
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
