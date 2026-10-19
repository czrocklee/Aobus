// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/utility/Uuid.h>

#include <ryml.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  inline constexpr auto kListPresentationsConfigGroup = std::string_view{"trackView.presentations"};
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
    utility::UuidBytes libraryId{};
    std::vector<StoredListPresentationPreference> preferences{};
  };

  Result<ListPresentationPreferenceDocument> toListPresentationPreferenceDocument(
    ListPresentations::Snapshot const& state,
    utility::UuidBytes const& libraryId);
  Result<ListPresentations::Snapshot> listPresentationPreferenceStateFromDocument(
    ListPresentationPreferenceDocument const& document);

  // Bound to the library whose list ids the preferences name. A document saved
  // for another library keeps only its virtual-list preferences.
  class ListPresentationPreferenceYamlSchema final
  {
  public:
    explicit ListPresentationPreferenceYamlSchema(utility::UuidBytes const& libraryId)
      : _libraryId{libraryId}
    {
    }

    Result<> serialize(ryml::NodeRef node, ListPresentations::Snapshot const& state) const;
    Result<ListPresentations::Snapshot> deserialize(ryml::ConstNodeRef node,
                                                    ListPresentations::Snapshot const& seed) const;

  private:
    utility::UuidBytes _libraryId;
  };
} // namespace ao::uimodel
