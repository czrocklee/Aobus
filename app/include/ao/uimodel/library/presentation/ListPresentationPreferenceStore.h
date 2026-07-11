// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/Signal.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class TrackPresentationCatalog;

  struct ListPresentationPreferenceState final
  {
    std::map<ListId, std::string> presentations;
  };

  class ListPresentationPreferenceStore final
  {
  public:
    explicit ListPresentationPreferenceStore(TrackPresentationCatalog& catalog);

    std::map<ListId, std::string> const& listPresentations() const noexcept { return _presentations; }
    void setListPresentations(std::map<ListId, std::string> const& presentations);

    std::optional<std::string_view> presentationIdForList(ListId listId) const;
    void setPresentationIdForList(ListId listId, std::string_view presentationId);
    void clearPresentationForList(ListId listId);

    rt::TrackPresentationSpec presentationForList(ListPresentationContext const& context) const;

    rt::Signal<ListId>& signalChanged() noexcept { return _changed; }

  private:
    TrackPresentationCatalog& _catalog;
    std::map<ListId, std::string> _presentations{};
    rt::Signal<ListId> _changed;
  };
} // namespace ao::uimodel
