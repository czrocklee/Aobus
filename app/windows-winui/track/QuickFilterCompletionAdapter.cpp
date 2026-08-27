// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/QuickFilterCompletionAdapter.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/presentation/PresentationText.h>
#include <ao/utility/UnicodeText.h>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace ao::winui
{
  namespace
  {
    constexpr unsigned char kAsciiUpperBound = 0x80U;
    constexpr unsigned char kTwoByteScalarUpperBound = 0xE0U;
    constexpr unsigned char kThreeByteScalarUpperBound = 0xF0U;

    std::size_t scalarByteCount(unsigned char const leadByte) noexcept
    {
      if (leadByte < kAsciiUpperBound)
      {
        return 1;
      }

      if (leadByte < kTwoByteScalarUpperBound)
      {
        return 2;
      }

      if (leadByte < kThreeByteScalarUpperBound)
      {
        return 3;
      }

      return 4;
    }

    std::optional<std::size_t> utf16OffsetForValidatedByteBoundary(std::string_view const text,
                                                                   std::size_t const targetByteOffset)
    {
      if (targetByteOffset > text.size())
      {
        return std::nullopt;
      }

      std::size_t byteOffset = 0;
      std::size_t utf16Offset = 0;

      while (byteOffset < text.size())
      {
        if (byteOffset == targetByteOffset)
        {
          return utf16Offset;
        }

        auto const byteCount = scalarByteCount(static_cast<unsigned char>(text[byteOffset]));
        byteOffset += byteCount;
        utf16Offset += byteCount == 4 ? 2 : 1;

        if (byteOffset > targetByteOffset)
        {
          return std::nullopt;
        }
      }

      return targetByteOffset == text.size() ? std::optional{utf16Offset} : std::nullopt;
    }
  } // namespace

  std::optional<std::size_t> quickFilterUtf8Cursor(std::string_view const utf8Text, std::size_t const utf16Cursor)
  {
    if (!utility::validateUtf8(utf8Text))
    {
      return std::nullopt;
    }

    std::size_t byteOffset = 0;
    std::size_t utf16Offset = 0;

    if (utf16Cursor == 0)
    {
      return byteOffset;
    }

    while (byteOffset < utf8Text.size())
    {
      auto const byteCount = scalarByteCount(static_cast<unsigned char>(utf8Text[byteOffset]));
      auto const codeUnitCount = byteCount == 4 ? std::size_t{2} : std::size_t{1};
      byteOffset += byteCount;
      utf16Offset += codeUnitCount;

      if (utf16Offset == utf16Cursor)
      {
        return byteOffset;
      }

      if (utf16Offset > utf16Cursor)
      {
        return std::nullopt;
      }
    }

    return std::nullopt;
  }

  std::optional<QuickFilterUtf16Range> quickFilterUtf16Range(std::string_view const utf8Text,
                                                             std::size_t const replaceBegin,
                                                             std::size_t const replaceEnd)
  {
    if (replaceBegin > replaceEnd || replaceEnd > utf8Text.size() || !utility::validateUtf8(utf8Text))
    {
      return std::nullopt;
    }

    auto const optBegin = utf16OffsetForValidatedByteBoundary(utf8Text, replaceBegin);
    auto const optEnd = utf16OffsetForValidatedByteBoundary(utf8Text, replaceEnd);

    if (!optBegin || !optEnd)
    {
      return std::nullopt;
    }

    return QuickFilterUtf16Range{.begin = *optBegin, .end = *optEnd};
  }

  std::vector<QuickFilterSuggestionRow> quickFilterSuggestionRows(rt::CompletionResult const& result,
                                                                  i18n::MessageCatalog const& textCatalog)
  {
    auto rows = std::vector<QuickFilterSuggestionRow>{};
    rows.reserve(result.items.size());

    for (auto const& item : result.items)
    {
      rows.push_back(QuickFilterSuggestionRow{
        .displayText = item.displayText,
        .detailText = uimodel::completionDetail(textCatalog, item.detail),
        .insertText = item.insertText,
        .detailKind = item.detail.kind,
        .rank = item.rank,
      });
    }

    return rows;
  }

  bool quickFilterSuggestionContinuesEditing(QuickFilterSuggestionRow const& row) noexcept
  {
    switch (row.detailKind)
    {
      case rt::CompletionDetailKind::Field:
      case rt::CompletionDetailKind::Alias:
      case rt::CompletionDetailKind::LogicalOperator: return true;
      case rt::CompletionDetailKind::Operator: return row.insertText != "?";
      case rt::CompletionDetailKind::None:
      case rt::CompletionDetailKind::ResolvedText:
      case rt::CompletionDetailKind::Frequency: return false;
    }

    return false;
  }
} // namespace ao::winui
