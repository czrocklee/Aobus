// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceCodec.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>

#include <format>
#include <string>

namespace ao::uimodel
{
  Result<ListPresentationPreferenceDocument> encodeListPresentationPreferences(
    ListPresentationPreferenceState const& state)
  {
    auto document = ListPresentationPreferenceDocument{};
    document.preferences.reserve(state.presentations.size());

    for (auto const& [listId, presentationId] : state.presentations)
    {
      if (listId == kInvalidListId)
      {
        return makeError(Error::Code::InvalidState, "Cannot persist a presentation preference for the invalid list id");
      }

      if (presentationId.empty())
      {
        return makeError(Error::Code::InvalidState, "Cannot persist an empty presentation id");
      }

      document.preferences.push_back(StoredListPresentationPreference{
        .listId = listId.raw(),
        .presentationId = presentationId,
      });
    }

    return document;
  }

  Result<ListPresentationPreferenceState> decodeListPresentationPreferences(
    ListPresentationPreferenceDocument const& document)
  {
    if (document.version != kListPresentationPreferenceVersion)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Unsupported list presentation preference version {}", document.version));
    }

    auto state = ListPresentationPreferenceState{};

    for (auto const& preference : document.preferences)
    {
      auto const listId = ListId{preference.listId};

      if (listId == kInvalidListId)
      {
        return makeError(Error::Code::FormatRejected, "List presentation preference uses the invalid list id");
      }

      if (preference.presentationId.empty())
      {
        return makeError(Error::Code::FormatRejected, "List presentation preference uses an empty presentation id");
      }

      if (!state.presentations.emplace(listId, preference.presentationId).second)
      {
        return makeError(Error::Code::FormatRejected,
                         std::format("List presentation preferences repeat list id {}", preference.listId));
      }
    }

    return state;
  }
} // namespace ao::uimodel
