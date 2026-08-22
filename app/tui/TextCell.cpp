// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TextCell.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace ao::tui
{
  namespace
  {
    constexpr unsigned char kAsciiLimit = 0x80U;
    constexpr unsigned char kTwoByteLeadMask = 0xE0U;
    constexpr unsigned char kTwoByteLead = 0xC0U;
    constexpr unsigned char kTwoByteValueMask = 0x1FU;
    constexpr unsigned char kThreeByteLeadMask = 0xF0U;
    constexpr unsigned char kThreeByteLead = 0xE0U;
    constexpr unsigned char kThreeByteValueMask = 0x0FU;
    constexpr unsigned char kFourByteLeadMask = 0xF8U;
    constexpr unsigned char kFourByteLead = 0xF0U;
    constexpr unsigned char kFourByteValueMask = 0x07U;
    constexpr unsigned char kContinuationValueMask = 0x3FU;
    constexpr unsigned int kContinuationValueBits = 6U;

    constexpr char32_t kZeroWidthJoiner = 0x200D;
    constexpr char32_t kCombiningKeycap = 0x20E3;
    constexpr char32_t kVariationSelectorFirst = 0xFE00;
    constexpr char32_t kVariationSelectorLast = 0xFE0F;
    constexpr char32_t kRegionalIndicatorFirst = 0x1F1E6;
    constexpr char32_t kRegionalIndicatorLast = 0x1F1FF;
    constexpr char32_t kSkinToneModifierFirst = 0x1F3FB;
    constexpr char32_t kSkinToneModifierLast = 0x1F3FF;
    constexpr char32_t kTagCharacterFirst = 0xE0020;
    constexpr char32_t kTagCharacterLast = 0xE007F;

    /**
     * @brief The first scalar value in one FTXUI glyph.
     *
     * Enough to classify a glyph's role in an emoji cluster. FTXUI produced the
     * glyph from decoded text, so an unexpected lead byte means only that this
     * glyph joins nothing, never that the caller must handle an error.
     */
    char32_t leadCodePoint(std::string_view const glyph)
    {
      if (glyph.empty())
      {
        return 0;
      }

      auto const first = static_cast<unsigned char>(glyph.front());

      if (first < kAsciiLimit)
      {
        return first;
      }

      std::size_t continuationCount = 0;
      char32_t value = 0;

      if ((first & kTwoByteLeadMask) == kTwoByteLead)
      {
        continuationCount = 1;
        value = first & kTwoByteValueMask;
      }
      else if ((first & kThreeByteLeadMask) == kThreeByteLead)
      {
        continuationCount = 2;
        value = first & kThreeByteValueMask;
      }
      else if ((first & kFourByteLeadMask) == kFourByteLead)
      {
        continuationCount = 3;
        value = first & kFourByteValueMask;
      }
      else
      {
        return 0;
      }

      if (glyph.size() <= continuationCount)
      {
        return 0;
      }

      for (std::size_t index = 1; index <= continuationCount; ++index)
      {
        value = (value << kContinuationValueBits) | (static_cast<unsigned char>(glyph[index]) & kContinuationValueMask);
      }

      return value;
    }

    bool isZeroWidthJoiner(char32_t const codePoint)
    {
      return codePoint == kZeroWidthJoiner;
    }

    bool isRegionalIndicator(char32_t const codePoint)
    {
      return codePoint >= kRegionalIndicatorFirst && codePoint <= kRegionalIndicatorLast;
    }

    /// Code points that decorate the emoji before them rather than standing alone.
    bool isClusterExtender(char32_t const codePoint)
    {
      auto const isVariationSelector = codePoint >= kVariationSelectorFirst && codePoint <= kVariationSelectorLast;
      auto const isSkinToneModifier = codePoint >= kSkinToneModifierFirst && codePoint <= kSkinToneModifierLast;
      auto const isTagCharacter = codePoint >= kTagCharacterFirst && codePoint <= kTagCharacterLast;

      return isVariationSelector || isSkinToneModifier || isTagCharacter || codePoint == kCombiningKeycap;
    }
  } // namespace

  std::int32_t cellWidth(std::string_view const value)
  {
    return static_cast<std::int32_t>(ftxui::string_width(std::string{value}));
  }

  std::int32_t panelColumnsForContent(std::int32_t const contentColumns, std::int32_t const terminalColumns)
  {
    auto const desiredColumns = contentColumns + kPanelBorderColumns;

    if (terminalColumns <= 0)
    {
      return desiredColumns;
    }

    return std::min(desiredColumns, terminalColumns);
  }

  std::string truncateToCellWidth(std::string_view const value, std::int32_t const width)
  {
    if (width <= 0)
    {
      return {};
    }

    auto result = std::string{};
    std::int32_t used = 0;
    // FTXUI splits an emoji cluster into several glyphs, so cutting between
    // glyphs can emit a dangling joiner or half a flag. Whole clusters are
    // therefore committed or dropped together, while FTXUI stays the authority
    // on how many cells they occupy.
    auto cluster = std::string{};
    std::int32_t clusterColumns = 0;
    std::size_t clusterGlyphs = 0;
    bool joinsNextGlyph = false;
    bool opensRegionalPair = false;

    auto commitCluster = [&]
    {
      if (clusterGlyphs == 0)
      {
        return true;
      }

      if (used + clusterColumns > width)
      {
        return false;
      }

      result += cluster;
      used += clusterColumns;
      cluster.clear();
      clusterColumns = 0;
      clusterGlyphs = 0;
      return true;
    };

    for (auto const& glyph : ftxui::Utf8ToGlyphs(std::string{value}))
    {
      auto const codePoint = leadCodePoint(glyph);
      auto const glyphColumns = static_cast<std::int32_t>(ftxui::string_width(glyph));
      auto const continuesCluster =
        clusterGlyphs > 0 &&
        (joinsNextGlyph || isZeroWidthJoiner(codePoint) || isClusterExtender(codePoint) || glyphColumns == 0 ||
         (clusterGlyphs == 1 && opensRegionalPair && isRegionalIndicator(codePoint)));

      if (!continuesCluster && !commitCluster())
      {
        return result;
      }

      if (clusterGlyphs == 0)
      {
        opensRegionalPair = isRegionalIndicator(codePoint);
      }

      cluster += glyph;
      clusterColumns += glyphColumns;
      ++clusterGlyphs;
      joinsNextGlyph = isZeroWidthJoiner(codePoint);
    }

    std::ignore = commitCluster();
    return result;
  }

  std::string ellipsizeToCellWidth(std::string_view const value, std::int32_t const width)
  {
    if (width <= 0)
    {
      return {};
    }

    if (cellWidth(value) <= width)
    {
      return std::string{value};
    }

    auto const ellipsisColumns = cellWidth(kCellEllipsis);

    if (width < ellipsisColumns)
    {
      return truncateToCellWidth(value, width);
    }

    auto result = truncateToCellWidth(value, width - ellipsisColumns);
    result.append(kCellEllipsis);
    return result;
  }

  std::string fitCellText(std::string_view const value, std::int32_t const width, CellAlignment const alignment)
  {
    auto result = truncateToCellWidth(value, width);
    auto const padding = std::max(0, width - cellWidth(result));

    if (padding <= 0)
    {
      return result;
    }

    if (alignment == CellAlignment::Right)
    {
      result.insert(0, static_cast<std::size_t>(padding), ' ');
      return result;
    }

    result.append(static_cast<std::size_t>(padding), ' ');
    return result;
  }
} // namespace ao::tui
