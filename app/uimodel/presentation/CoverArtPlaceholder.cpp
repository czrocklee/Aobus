// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kMonogramColors = std::to_array<CoverArtPlaceholderRgb>({
      {.red = 90, .green = 141, .blue = 184},
      {.red = 134, .green = 111, .blue = 171},
      {.red = 79, .green = 145, .blue = 133},
      {.red = 174, .green = 107, .blue = 93},
      {.red = 144, .green = 135, .blue = 70},
      {.red = 82, .green = 124, .blue = 162},
      {.red = 158, .green = 103, .blue = 132},
      {.red = 93, .green = 140, .blue = 101},
    });

    constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    constexpr unsigned char kAsciiMaximum = 0x7FU;
    constexpr unsigned char kUtf8ContinuationMask = 0xC0U;
    constexpr unsigned char kUtf8ContinuationValue = 0x80U;
    constexpr unsigned char kUtf8TwoByteMinimum = 0xC2U;
    constexpr unsigned char kUtf8TwoByteMaximum = 0xDFU;
    constexpr unsigned char kUtf8ThreeByteMinimum = 0xE0U;
    constexpr unsigned char kUtf8ThreeByteMaximum = 0xEFU;
    constexpr unsigned char kUtf8SurrogateLead = 0xEDU;
    constexpr unsigned char kUtf8ThreeByteSecondMinimum = 0xA0U;
    constexpr unsigned char kUtf8ThreeByteSecondMaximum = 0x9FU;
    constexpr unsigned char kUtf8FourByteMinimum = 0xF0U;
    constexpr unsigned char kUtf8FourByteMaximum = 0xF4U;
    constexpr unsigned char kUtf8FourByteSecondMinimum = 0x90U;
    constexpr unsigned char kUtf8FourByteSecondMaximum = 0x8FU;

    bool isAsciiWhitespace(unsigned char const value) noexcept
    {
      return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
    }

    std::size_t utf8ScalarLength(std::string_view const text, std::size_t const offset) noexcept
    {
      auto const first = static_cast<unsigned char>(text[offset]);

      if (first <= kAsciiMaximum)
      {
        return 1;
      }

      auto const available = text.size() - offset;
      auto continuation = [&text, offset](std::size_t const index)
      { return (static_cast<unsigned char>(text[offset + index]) & kUtf8ContinuationMask) == kUtf8ContinuationValue; };

      if (first >= kUtf8TwoByteMinimum && first <= kUtf8TwoByteMaximum && available >= 2 && continuation(1))
      {
        return 2;
      }

      if (first >= kUtf8ThreeByteMinimum && first <= kUtf8ThreeByteMaximum && available >= 3 && continuation(1) &&
          continuation(2))
      {
        auto const second = static_cast<unsigned char>(text[offset + 1]);

        if ((first != kUtf8ThreeByteMinimum || second >= kUtf8ThreeByteSecondMinimum) &&
            (first != kUtf8SurrogateLead || second <= kUtf8ThreeByteSecondMaximum))
        {
          return 3;
        }
      }

      if (first >= kUtf8FourByteMinimum && first <= kUtf8FourByteMaximum && available >= 4 && continuation(1) &&
          continuation(2) && continuation(3))
      {
        auto const second = static_cast<unsigned char>(text[offset + 1]);

        if ((first != kUtf8FourByteMinimum || second >= kUtf8FourByteSecondMinimum) &&
            (first != kUtf8FourByteMaximum || second <= kUtf8FourByteSecondMaximum))
        {
          return 4;
        }
      }

      return 0;
    }

    struct NormalizedMonogram final
    {
      std::string text{"?"};
      std::size_t scalarCount = 1;
    };

    NormalizedMonogram normalizeMonogram(std::string_view const text, std::size_t const maxScalars)
    {
      std::size_t offset = 0;

      while (offset < text.size() && isAsciiWhitespace(static_cast<unsigned char>(text[offset])))
      {
        ++offset;
      }

      auto result = std::string{};
      std::size_t scalarCount = 0;

      while (offset < text.size())
      {
        if (isAsciiWhitespace(static_cast<unsigned char>(text[offset])))
        {
          break;
        }

        auto const length = utf8ScalarLength(text, offset);

        if (length == 0)
        {
          return {};
        }

        if (scalarCount < maxScalars)
        {
          auto scalar = std::string{text.substr(offset, length)};

          if (length == 1 && scalar.front() >= 'a' && scalar.front() <= 'z')
          {
            scalar.front() = static_cast<char>(scalar.front() - ('a' - 'A'));
          }

          result.append(scalar);
          ++scalarCount;
        }

        offset += length;
      }

      if (result.empty())
      {
        return {};
      }

      return {.text = std::move(result), .scalarCount = scalarCount};
    }

    std::uint64_t stableHash(std::string_view const value) noexcept
    {
      auto hash = kFnvOffsetBasis;

      for (auto const character : value)
      {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnvPrime;
      }

      return hash;
    }
  } // namespace

  std::string_view coverArtPlaceholderStyleId(CoverArtPlaceholderStyle const style) noexcept
  {
    auto fallback = std::string_view{};

    for (auto const& entry : kCoverArtPlaceholderStyles)
    {
      if (entry.style == style)
      {
        return entry.id;
      }

      if (entry.style == CoverArtPlaceholderStyle::Note)
      {
        fallback = entry.id;
      }
    }

    return fallback;
  }

  std::optional<CoverArtPlaceholderStyle> parseCoverArtPlaceholderStyle(std::string_view const id) noexcept
  {
    for (auto const& entry : kCoverArtPlaceholderStyles)
    {
      if (entry.id == id)
      {
        return entry.style;
      }
    }

    return std::nullopt;
  }

  std::vector<std::string> coverArtPlaceholderStyleIds()
  {
    auto result = std::vector<std::string>{};
    result.reserve(kCoverArtPlaceholderStyles.size());

    for (auto const& entry : kCoverArtPlaceholderStyles)
    {
      result.emplace_back(entry.id);
    }

    return result;
  }

  CoverArtPlaceholderStyle defaultCoverArtPlaceholderStyle(CoverArtPlaceholderSlot const slot) noexcept
  {
    switch (slot)
    {
      case CoverArtPlaceholderSlot::GroupHeading: return CoverArtPlaceholderStyle::Monogram;
      case CoverArtPlaceholderSlot::Inspector: return CoverArtPlaceholderStyle::Vinyl;
      case CoverArtPlaceholderSlot::NowPlaying: return CoverArtPlaceholderStyle::Equalizer;
    }

    return CoverArtPlaceholderStyle::Note;
  }

  CoverArtPlaceholderIdentity makeCoverArtPlaceholderIdentity(std::span<std::string_view const> const candidateTexts)
  {
    for (auto const candidate : candidateTexts)
    {
      if (!candidate.empty())
      {
        return {.primaryText = std::string{candidate}};
      }
    }

    return {};
  }

  CoverArtPlaceholderPresentation makeCoverArtPlaceholderPresentation(CoverArtPlaceholderStyle const style,
                                                                      CoverArtPlaceholderIdentity const& identity)
  {
    auto const colorIndex = stableHash(identity.primaryText) % kMonogramColors.size();
    auto normalized =
      identity.optMonogram ? normalizeMonogram(*identity.optMonogram, 2) : normalizeMonogram(identity.primaryText, 1);
    return {
      .style = style,
      .monogram = std::move(normalized.text),
      .monogramSize = normalized.scalarCount > 1 ? CoverArtPlaceholderMonogramSize::Compact
                                                 : CoverArtPlaceholderMonogramSize::Regular,
      .monogramColor = kMonogramColors[colorIndex],
    };
  }
} // namespace ao::uimodel
