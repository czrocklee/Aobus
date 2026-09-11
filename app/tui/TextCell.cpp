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
#include <utility>
#include <vector>

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

    struct CellCluster final
    {
      std::string text;
      std::int32_t columns = 0;
      bool lineBreak = false;
    };

    std::string normalizeLineBreaks(std::string_view const value, CellLineBreaks const lineBreaks)
    {
      auto normalized = std::string{};
      normalized.reserve(value.size());

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        if (auto const ch = value[index]; ch == '\r')
        {
          if (index + 1 < value.size() && value[index + 1] == '\n')
          {
            ++index;
            normalized.push_back(lineBreaks == CellLineBreaks::Preserve ? '\n' : ' ');
          }
          else if (lineBreaks == CellLineBreaks::Flatten)
          {
            normalized.push_back(' ');
          }
        }
        else if (ch == '\n')
        {
          normalized.push_back(lineBreaks == CellLineBreaks::Preserve ? '\n' : ' ');
        }
        else if (ch == '\t' && lineBreaks == CellLineBreaks::Flatten)
        {
          normalized.push_back(' ');
        }
        else
        {
          normalized.push_back(ch);
        }
      }

      return normalized;
    }

    std::vector<CellCluster> cellClusters(std::string_view const value)
    {
      auto clusters = std::vector<CellCluster>{};
      auto cluster = CellCluster{};
      std::size_t clusterGlyphs = 0;
      bool joinsNextGlyph = false;
      bool opensRegionalPair = false;

      auto commitCluster = [&]
      {
        if (clusterGlyphs == 0)
        {
          return;
        }

        clusters.push_back(std::move(cluster));
        cluster = {};
        clusterGlyphs = 0;
        joinsNextGlyph = false;
        opensRegionalPair = false;
      };

      auto appendSegment = [&](std::string_view const segment)
      {
        for (auto const& glyph : ftxui::Utf8ToGlyphs(std::string{segment}))
        {
          auto const codePoint = leadCodePoint(glyph);
          auto const glyphColumns = static_cast<std::int32_t>(ftxui::string_width(glyph));
          auto const continuesCluster =
            clusterGlyphs > 0 &&
            (joinsNextGlyph || isZeroWidthJoiner(codePoint) || isClusterExtender(codePoint) || glyphColumns == 0 ||
             (clusterGlyphs == 1 && opensRegionalPair && isRegionalIndicator(codePoint)));

          if (!continuesCluster)
          {
            commitCluster();
          }

          if (clusterGlyphs == 0)
          {
            opensRegionalPair = isRegionalIndicator(codePoint);
          }

          cluster.text += glyph;
          cluster.columns += glyphColumns;
          ++clusterGlyphs;
          joinsNextGlyph = isZeroWidthJoiner(codePoint);
        }

        commitCluster();
      };
      std::size_t segmentBegin = 0;

      // FTXUI can attach a leading combining mark to an LF glyph. Split first
      // so LF remains structural and each new row applies FTXUI's leading-mark policy.
      while (segmentBegin <= value.size())
      {
        auto const lineBreak = value.find('\n', segmentBegin);

        if (lineBreak == std::string_view::npos)
        {
          appendSegment(value.substr(segmentBegin));
          break;
        }

        appendSegment(value.substr(segmentBegin, lineBreak - segmentBegin));
        clusters.push_back({.text = "\n", .lineBreak = true});
        segmentBegin = lineBreak + 1;
      }

      return clusters;
    }

    std::string clusterText(std::vector<CellCluster> const& clusters, std::size_t const begin, std::size_t const end)
    {
      auto result = std::string{};

      for (std::size_t index = begin; index < end; ++index)
      {
        result += clusters[index].text;
      }

      return result;
    }

    std::string ellipsizeClusters(std::vector<CellCluster> const& clusters,
                                  std::size_t const begin,
                                  std::size_t const end,
                                  std::int32_t const width)
    {
      std::int32_t totalColumns = 0;
      auto lineEnd = begin;

      while (lineEnd < end && !clusters[lineEnd].lineBreak)
      {
        totalColumns += clusters[lineEnd].columns;
        ++lineEnd;
      }

      auto const hasFollowingLine = lineEnd < end && lineEnd + 1 < end;

      if (totalColumns <= width && !hasFollowingLine)
      {
        return clusterText(clusters, begin, lineEnd);
      }

      auto const ellipsisColumns = cellWidth(kCellEllipsis);
      auto const contentColumns = width < ellipsisColumns ? width : width - ellipsisColumns;
      std::int32_t usedColumns = 0;
      auto index = begin;

      while (index < lineEnd && usedColumns + clusters[index].columns <= contentColumns)
      {
        usedColumns += clusters[index].columns;
        ++index;
      }

      auto result = clusterText(clusters, begin, index);

      if (width >= ellipsisColumns)
      {
        result.append(kCellEllipsis);
      }

      return result;
    }

    std::string nextClusterRow(std::vector<CellCluster> const& clusters, std::size_t& cursor, std::int32_t const width)
    {
      auto const lineBegin = cursor;
      auto lastSpace = clusters.size();
      std::int32_t usedColumns = 0;

      while (cursor < clusters.size())
      {
        auto const& cluster = clusters[cursor];

        if (cluster.lineBreak)
        {
          auto row = clusterText(clusters, lineBegin, cursor);
          ++cursor;
          return row;
        }

        if (usedColumns + cluster.columns > width)
        {
          if (cursor == lineBegin)
          {
            auto row = ellipsizeClusters(clusters, cursor, clusters.size(), width);
            cursor = clusters.size();
            return row;
          }

          auto lineEnd = cursor;

          if (lastSpace != clusters.size() && lastSpace > lineBegin)
          {
            lineEnd = lastSpace;
            cursor = lastSpace + 1;
          }

          return clusterText(clusters, lineBegin, lineEnd);
        }

        usedColumns += cluster.columns;

        if (cluster.text == " ")
        {
          lastSpace = cursor;
        }

        ++cursor;
      }

      return clusterText(clusters, lineBegin, clusters.size());
    }
  } // namespace

  std::size_t singleLineControlLength(std::string_view const value) noexcept
  {
    constexpr unsigned char kFirstPrintableAscii = 0x20U;
    constexpr unsigned char kAsciiDelete = 0x7FU;
    constexpr unsigned char kC1LeadByte = 0xC2U;
    constexpr unsigned char kC1FirstTrailByte = 0x80U;
    constexpr unsigned char kC1LastTrailByte = 0x9FU;

    if (value.empty())
    {
      return 0;
    }

    auto const byte = static_cast<unsigned char>(value.front());

    if (byte < kFirstPrintableAscii || byte == kAsciiDelete)
    {
      return 1;
    }

    if (byte == kC1LeadByte && value.size() > 1 && static_cast<unsigned char>(value[1]) >= kC1FirstTrailByte &&
        static_cast<unsigned char>(value[1]) <= kC1LastTrailByte)
    {
      return 2;
    }

    if (value.starts_with("\u2028") || value.starts_with("\u2029"))
    {
      return 3;
    }

    return 0;
  }

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

  std::vector<std::string> wrapCellText(std::string_view const value,
                                        std::int32_t const width,
                                        CellWrapOptions const options)
  {
    auto rows = std::vector<std::string>{};

    if (value.empty() || width <= 0)
    {
      return rows;
    }

    auto const normalized = normalizeLineBreaks(value, options.lineBreaks);
    auto const clusters = cellClusters(normalized);
    std::size_t cursor = 0;

    while (cursor < clusters.size() && (options.maxLines == 0 || rows.size() < options.maxLines))
    {
      auto const isLastRow = options.maxLines > 0 && rows.size() + 1 == options.maxLines;

      if (isLastRow)
      {
        rows.push_back(ellipsizeClusters(clusters, cursor, clusters.size(), width));
        break;
      }

      rows.push_back(nextClusterRow(clusters, cursor, width));

      while (cursor < clusters.size() && clusters[cursor].text == " ")
      {
        ++cursor;
      }
    }

    return rows;
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
