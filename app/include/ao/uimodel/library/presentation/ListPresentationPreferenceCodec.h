// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ListPresentationPreferenceStore.h"
#include <ao/Error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::uimodel
{
  inline constexpr std::uint32_t kListPresentationPreferenceVersion = 1;

  // Persistence DTOs: member names are the exact versioned YAML keys.
  struct StoredListPresentationPreference final
  {
    std::uint32_t listId = 0;
    std::string presentationId{};
  };

  struct ListPresentationPreferenceDocument final
  {
    std::uint32_t version = kListPresentationPreferenceVersion;
    std::vector<StoredListPresentationPreference> preferences{};
  };

  Result<ListPresentationPreferenceDocument> encodeListPresentationPreferences(
    ListPresentationPreferenceState const& state);
  Result<ListPresentationPreferenceState> decodeListPresentationPreferences(
    ListPresentationPreferenceDocument const& document);
} // namespace ao::uimodel
