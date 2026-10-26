// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionItem.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  std::vector<std::string> insertTexts(std::vector<CompletionItem> const& items);

  void checkCompletionItem(CompletionItem const& item,
                           std::string_view displayText,
                           std::string_view insertText,
                           CompletionDetailKind detailKind,
                           std::uint32_t frequency,
                           std::uint32_t rank);
} // namespace ao::rt::test
