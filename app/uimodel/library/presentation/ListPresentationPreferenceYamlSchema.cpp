// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    Result<> writePreference(ryml::NodeRef node, StoredListPresentationPreference const& preference)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("listId", preference.listId).scalar("presentationId", preference.presentationId);
      return {};
    }

    Result<StoredListPresentationPreference> readPreference(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"listId", "presentationId"});

      auto preference = StoredListPresentationPreference{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("listId", preference.listId).requiredScalar("presentationId", preference.presentationId);
      return std::move(reader).finish(std::move(preference));
    }

    Result<> writeDocument(ryml::NodeRef node, ListPresentationPreferenceDocument const& document)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("version", document.version).sequence("preferences", document.preferences, writePreference);
      return std::move(writer).finish();
    }

    Result<ListPresentationPreferenceDocument> readDocument(ryml::ConstNodeRef node)
    {
      constexpr auto kContext = std::string_view{"list presentation preferences"};

      if (auto const result = yaml::requireMap(node, kContext); !result)
      {
        return std::unexpected{result.error()};
      }

      auto version = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

      if (!version)
      {
        return std::unexpected{version.error()};
      }

      if (*version != kListPresentationPreferenceVersion)
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported list presentation preference version {}", *version));
      }

      constexpr auto kKeys = std::to_array<std::string_view>({"version", "preferences"});

      auto document = ListPresentationPreferenceDocument{.version = *version};
      auto reader = yaml::MapReader{node, kKeys, kContext};
      reader.requiredSequence("preferences", document.preferences, readPreference);
      return std::move(reader).finish(std::move(document));
    }
  } // namespace

  Result<ListPresentationPreferenceDocument> toListPresentationPreferenceDocument(
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

  Result<ListPresentationPreferenceState> listPresentationPreferenceStateFromDocument(
    ListPresentationPreferenceDocument const& document)
  {
    if (document.version != kListPresentationPreferenceVersion)
    {
      return makeError(Error::Code::NotSupported,
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

  Result<> ListPresentationPreferenceYamlSchema::serialize(ryml::NodeRef node,
                                                           ListPresentationPreferenceState const& state) const
  {
    auto document = toListPresentationPreferenceDocument(state);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return writeDocument(node, *document);
  }

  Result<ListPresentationPreferenceState> ListPresentationPreferenceYamlSchema::deserialize(
    ryml::ConstNodeRef node,
    ListPresentationPreferenceState const& /*seed*/) const
  {
    auto document = readDocument(node);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return listPresentationPreferenceStateFromDocument(*document);
  }
} // namespace ao::uimodel
