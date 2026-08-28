// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ListPresentations.h"
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
    ListPresentations::Snapshot const& state);
  Result<ListPresentations::Snapshot> listPresentationPreferenceStateFromDocument(
    ListPresentationPreferenceDocument const& document);

  struct ListPresentationPreferenceYamlSchema final
  {
    Result<> serialize(ryml::NodeRef node, ListPresentations::Snapshot const& state) const;
    Result<ListPresentations::Snapshot> deserialize(ryml::ConstNodeRef node,
                                                    ListPresentations::Snapshot const& seed) const;
  };
} // namespace ao::uimodel
