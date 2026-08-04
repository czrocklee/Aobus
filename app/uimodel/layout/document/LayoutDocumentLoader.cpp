// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutDocumentLoader.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDialect.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutValidation.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>

#include <ryml.hpp>

#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  Result<PreparedLayout> prepareShellDocument(std::string_view const yaml,
                                              std::string_view const sourceName,
                                              LayoutComponentCatalog const& components,
                                              LayoutActionCatalog const& actions,
                                              LayoutDialect const& dialect)
  {
    constexpr auto kLimits = LayoutDocumentLimits{};

    if (yaml.size() > kLimits.maxFileBytes)
    {
      return makeError(Error::Code::ValueTooLarge,
                       std::format("{} layout document '{}' is {} bytes, over the {} byte limit",
                                   dialect.name,
                                   sourceName,
                                   yaml.size(),
                                   kLimits.maxFileBytes));
    }

    auto document = LayoutDocument{};

    try
    {
      auto errorState = yaml::ErrorCallbackState{std::string{sourceName}};
      auto tree = ryml::Tree{yaml::callbacks(errorState)};
      yaml::parseInArena(tree, yaml, errorState);

      auto deserialized = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});

      if (!deserialized)
      {
        // Keep the schema's code: an unsupported version is a different defect
        // from a malformed node, even though both reject the whole candidate.
        return makeError(
          deserialized.error().code,
          std::format(
            "Failed to read {} layout document '{}': {}", dialect.name, sourceName, deserialized.error().message));
      }

      document = std::move(*deserialized);
    }
    catch (std::exception const& exception)
    {
      return makeError(
        Error::Code::FormatRejected,
        std::format("Failed to parse {} layout document '{}': {}", dialect.name, sourceName, exception.what()));
    }

    auto prepared = prepareLayout(document, kLimits);

    if (!prepared)
    {
      return std::unexpected{prepared.error()};
    }

    if (auto validated = requireValidLayout(*prepared, components, actions, dialect); !validated)
    {
      return makeError(validated.error().code,
                       std::format("{} layout document '{}': {}", dialect.name, sourceName, validated.error().message));
    }

    return prepared;
  }
} // namespace ao::uimodel
