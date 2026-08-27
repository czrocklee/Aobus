// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  struct CompletionResult;
}

namespace ao::winui
{
  struct QuickFilterUtf16Range final
  {
    std::size_t begin = 0;
    std::size_t end = 0;

    bool operator==(QuickFilterUtf16Range const&) const = default;
  };

  struct QuickFilterSuggestionRow final
  {
    std::string displayText{};
    std::string detailText{};
    std::string insertText{};
    rt::CompletionDetailKind detailKind = rt::CompletionDetailKind::None;
    std::uint32_t rank = 0;

    bool operator==(QuickFilterSuggestionRow const&) const = default;
  };

  /** Maps a native UTF-16 caret boundary to the matching UTF-8 byte boundary. */
  std::optional<std::size_t> quickFilterUtf8Cursor(std::string_view utf8Text, std::size_t utf16Cursor);

  /** Maps a completion result's UTF-8 byte span to native UTF-16 code-unit boundaries. */
  std::optional<QuickFilterUtf16Range> quickFilterUtf16Range(std::string_view utf8Text,
                                                             std::size_t replaceBegin,
                                                             std::size_t replaceEnd);

  /** Projects semantic completion items without making localized text an identity. */
  std::vector<QuickFilterSuggestionRow> quickFilterSuggestionRows(rt::CompletionResult const& result,
                                                                  i18n::MessageCatalog const& textCatalog);

  /** Whether accepting this row still needs another expression token before submission. */
  bool quickFilterSuggestionContinuesEditing(QuickFilterSuggestionRow const& row) noexcept;
} // namespace ao::winui
