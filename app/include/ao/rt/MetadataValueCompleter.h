// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CompletionItem.h"
#include "CompletionResult.h"
#include "TrackField.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class CompletionService;

  class MetadataValueCompleter final
  {
  public:
    MetadataValueCompleter(CompletionService& vocabulary, TrackField field);

    std::vector<CompletionItem> complete(std::string_view prefix, std::size_t limit = kCompletionResultLimit);
    CompletionProvider asProvider() const;

  private:
    CompletionService& _vocabulary;
    TrackField _field;
  };
} // namespace ao::rt
