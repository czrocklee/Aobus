// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include <ao/rt/LibraryMutationService.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel::list
{
  enum class SmartListStatus : std::uint8_t
  {
    EmptySource,
    Valid,
    InvalidExpression
  };

  struct SmartListEditorViewState final
  {
    std::string name;
    std::string description;
    std::string localExpression;
    std::string effectiveExpression;

    SmartListStatus status = SmartListStatus::Valid;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
  };

  class SmartListEditorModel final
  {
  public:
    static std::string composeEffectiveExpression(std::string_view parent, std::string_view local);

    static bool canSubmit(std::string_view name, SmartListStatus status);

    static rt::LibraryMutationService::ListDraft createDraft(ListId parentListId,
                                                             ListId editListId,
                                                             std::string const& name,
                                                             std::string const& description,
                                                             std::string const& expression);
  };
} // namespace ao::uimodel::list
