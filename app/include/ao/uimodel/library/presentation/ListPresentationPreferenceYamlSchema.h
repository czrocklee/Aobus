// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ListPresentationPreferenceStore.h"
#include <ao/Error.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::uimodel
{
  inline constexpr std::uint32_t kListPresentationPreferenceVersion = 1;

  // Persistence DTOs isolate the versioned wire shape from live UIModel state.
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

  Result<ListPresentationPreferenceDocument> toListPresentationPreferenceDocument(
    ListPresentationPreferenceState const& state);
  Result<ListPresentationPreferenceState> listPresentationPreferenceStateFromDocument(
    ListPresentationPreferenceDocument const& document);

  struct ListPresentationPreferenceYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, ListPresentationPreferenceState const& state) const;
    Result<ListPresentationPreferenceState> deserialize(ryml::ConstNodeRef node,
                                                        ListPresentationPreferenceState const& seed) const;
  };
} // namespace ao::uimodel
