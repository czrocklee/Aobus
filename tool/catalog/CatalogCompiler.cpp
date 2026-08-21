// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "i18n/CatalogPattern.h"
#include "i18n/MessageIds.h"
#include "i18n/WinUiResourceProjection.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  constexpr std::size_t kExpectedArgCount = 13;
  constexpr std::size_t kOptionSlotCount = 6;
  constexpr std::size_t kOptionRootResSlot = 0;
  constexpr std::size_t kOptionDeResSlot = 1;
  constexpr std::size_t kOptionPseudoSourceSlot = 2;
  constexpr std::size_t kOptionEnReswSlot = 3;
  constexpr std::size_t kOptionDeReswSlot = 4;
  constexpr std::size_t kOptionPseudoReswSlot = 5;

  struct Options final
  {
    std::filesystem::path rootResource;
    std::filesystem::path germanResource;
    std::filesystem::path pseudoSource;
    std::filesystem::path englishResw;
    std::filesystem::path germanResw;
    std::filesystem::path pseudoResw;
  };

  ao::Result<Options> parseOptions(std::span<char const* const> const args)
  {
    if (args.size() != kExpectedArgCount)
    {
      return ao::makeError(ao::Error::Code::InvalidInput,
                           "Usage: ao_catalog_compiler --root-res PATH --de-res PATH "
                           "--pseudo-source PATH --en-resw PATH --de-resw PATH --pseudo-resw PATH");
    }

    auto options = Options{};
    auto assigned = std::array<bool, kOptionSlotCount>{};

    for (std::size_t index = 1; index < args.size(); index += 2)
    {
      auto const name = std::string_view{args[index]};
      auto const value = ao::utility::pathFromUtf8(args[index + 1]);
      auto assign = [&](std::size_t const slot, std::filesystem::path& destination) -> ao::Result<>
      {
        if (assigned[slot])
        {
          return ao::makeError(ao::Error::Code::InvalidInput, std::string{name} + " appears more than once");
        }

        assigned[slot] = true;
        destination = value;
        return {};
      };

      auto assignedRes = ao::Result<>{};

      if (name == "--root-res")
      {
        assignedRes = assign(kOptionRootResSlot, options.rootResource);
      }
      else if (name == "--de-res")
      {
        assignedRes = assign(kOptionDeResSlot, options.germanResource);
      }
      else if (name == "--pseudo-source")
      {
        assignedRes = assign(kOptionPseudoSourceSlot, options.pseudoSource);
      }
      else if (name == "--en-resw")
      {
        assignedRes = assign(kOptionEnReswSlot, options.englishResw);
      }
      else if (name == "--de-resw")
      {
        assignedRes = assign(kOptionDeReswSlot, options.germanResw);
      }
      else if (name == "--pseudo-resw")
      {
        assignedRes = assign(kOptionPseudoReswSlot, options.pseudoResw);
      }
      else
      {
        return ao::makeError(ao::Error::Code::InvalidInput, std::string{"Unknown option: "} + std::string{name});
      }

      if (!assignedRes)
      {
        return std::unexpected{assignedRes.error()};
      }
    }

    if (!std::ranges::all_of(assigned, std::identity{}))
    {
      return ao::makeError(ao::Error::Code::InvalidInput, "Catalog compiler is missing a required output option");
    }

    return options;
  }

  ao::Result<> validateRootIds(std::span<ao::i18n::detail::CatalogMessage const> const messages)
  {
    auto expected = std::vector<std::string_view>{};
    expected.reserve(ao::i18n::detail::kMessageDefinitions.size());

    for (auto const& definition : ao::i18n::detail::kMessageDefinitions)
    {
      expected.push_back(definition.key);
    }

    std::ranges::sort(expected);

    auto actual = std::vector<std::string_view>{};
    actual.reserve(messages.size());

    for (auto const& message : messages)
    {
      actual.push_back(message.id);
    }

    std::ranges::sort(actual);

    if (actual != expected)
    {
      return ao::makeError(
        ao::Error::Code::FormatRejected, "The root catalog ids do not match the typed MessageId registry");
    }

    return {};
  }

  ao::Result<> writeFile(std::filesystem::path const& path, std::string_view const content)
  {
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};

    if (!stream)
    {
      return ao::makeError(
        ao::Error::Code::IoError, "Could not open generated catalog output " + ao::utility::pathToUtf8(path));
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));

    if (!stream)
    {
      return ao::makeError(
        ao::Error::Code::IoError, "Could not write generated catalog output " + ao::utility::pathToUtf8(path));
    }

    return {};
  }

  ao::Result<std::vector<ao::i18n::detail::CatalogMessage>> projectWinUiResources(
    std::span<ao::i18n::detail::CatalogMessage const> const messages)
  {
    auto result = std::vector<ao::i18n::detail::CatalogMessage>{messages.begin(), messages.end()};
    result.reserve(result.size() + ao::i18n::detail::kWinUiResourceAliases.size());

    for (auto const& positional : ao::i18n::detail::kWinUiPositionalResources)
    {
      auto source = std::ranges::find(result, positional.messageId, &ao::i18n::detail::CatalogMessage::id);

      if (source == result.end())
      {
        return ao::makeError(
          ao::Error::Code::FormatRejected,
          "WinUI positional resource references unknown message id '" + std::string{positional.messageId} + "'");
      }

      auto projectedRes = ao::i18n::detail::projectWinUiPositionalPattern(source->pattern, positional.argumentName);

      if (!projectedRes)
      {
        return ao::makeError(projectedRes.error().code,
                             "WinUI resource cannot positionalize message '" + std::string{positional.messageId} +
                               "': " + projectedRes.error().message);
      }

      source->pattern = std::move(*projectedRes);
    }

    for (auto& message : result)
    {
      message.pattern = ao::i18n::detail::unescapeIcuApostrophePairs(message.pattern);
    }

    for (auto const& alias : ao::i18n::detail::kWinUiResourceAliases)
    {
      auto const source = std::ranges::find(result, alias.messageId, &ao::i18n::detail::CatalogMessage::id);

      if (source == result.end())
      {
        return ao::makeError(
          ao::Error::Code::FormatRejected,
          "WinUI resource alias references unknown message id '" + std::string{alias.messageId} + "'");
      }

      auto signatureRes = ao::i18n::detail::messageArgumentSignature(source->pattern);

      if (!signatureRes)
      {
        return std::unexpected{signatureRes.error()};
      }

      if (!signatureRes->empty())
      {
        return ao::makeError(
          ao::Error::Code::FormatRejected,
          "WinUI resource alias has an incompatible message signature for '" + std::string{alias.messageId} + "'");
      }

      if (std::ranges::contains(result, alias.resourceId, &ao::i18n::detail::CatalogMessage::id))
      {
        return ao::makeError(ao::Error::Code::FormatRejected,
                             "WinUI resource alias collides with message id '" + std::string{alias.resourceId} + "'");
      }

      result.push_back({.id = std::string{alias.resourceId}, .pattern = source->pattern});
    }

    return result;
  }

  ao::Result<> appendWinUiEnglishResources(std::vector<ao::i18n::detail::CatalogMessage>& messages)
  {
    messages.reserve(messages.size() + ao::i18n::detail::kWinUiEnglishResources.size());

    for (auto const& resource : ao::i18n::detail::kWinUiEnglishResources)
    {
      if (std::ranges::contains(messages, resource.resourceId, &ao::i18n::detail::CatalogMessage::id))
      {
        return ao::makeError(
          ao::Error::Code::FormatRejected,
          "WinUI English resource collides with generated resource id '" + std::string{resource.resourceId} + "'");
      }

      messages.push_back({.id = std::string{resource.resourceId}, .pattern = std::string{resource.text}});
    }

    return {};
  }

  ao::Result<Options> absoluteOptions(Options options)
  {
    auto const paths = std::array{
      &options.rootResource,
      &options.germanResource,
      &options.pseudoSource,
      &options.englishResw,
      &options.germanResw,
      &options.pseudoResw,
    };

    for (auto* const path : paths)
    {
      auto error = std::error_code{};
      auto absolute = std::filesystem::absolute(*path, error);

      if (error)
      {
        return ao::makeError(
          ao::Error::Code::IoError,
          "Could not resolve catalog path " + ao::utility::pathToUtf8(*path) + ": " + error.message());
      }

      *path = std::move(absolute);
    }

    return options;
  }

  ao::Result<> run(Options const& options)
  {
    auto absoluteRes = absoluteOptions(options);

    if (!absoluteRes)
    {
      return std::unexpected{absoluteRes.error()};
    }

    auto const& paths = *absoluteRes;

    auto const rootDirectory = ao::utility::pathToUtf8(paths.rootResource.parent_path());
    auto rootRes =
      ao::i18n::detail::loadCompiledCatalog(rootDirectory, ao::utility::pathToUtf8(paths.rootResource.stem()));

    if (!rootRes)
    {
      return std::unexpected{rootRes.error()};
    }

    auto const germanDirectory = ao::utility::pathToUtf8(paths.germanResource.parent_path());
    auto germanRes =
      ao::i18n::detail::loadCompiledCatalog(germanDirectory, ao::utility::pathToUtf8(paths.germanResource.stem()));

    if (!germanRes)
    {
      return std::unexpected{germanRes.error()};
    }

    if (auto rootIdsRes = validateRootIds(*rootRes); !rootIdsRes)
    {
      return rootIdsRes;
    }

    if (auto validationRes = ao::i18n::detail::validateTranslationCatalog(*rootRes, *germanRes); !validationRes)
    {
      return validationRes;
    }

    auto pseudo = std::vector<ao::i18n::detail::CatalogMessage>{};
    pseudo.reserve(rootRes->size());

    for (auto const& message : *rootRes)
    {
      auto patternRes = ao::i18n::detail::pseudoLocalizePattern(message.pattern);

      if (!patternRes)
      {
        return ao::makeError(
          patternRes.error().code, "Could not pseudo-localize '" + message.id + "': " + patternRes.error().message);
      }

      pseudo.push_back({.id = message.id, .pattern = std::move(*patternRes)});
    }

    if (auto validationRes = ao::i18n::detail::validateTranslationCatalog(*rootRes, pseudo); !validationRes)
    {
      return validationRes;
    }

    for (auto const& output : {paths.pseudoSource.parent_path(),
                               paths.englishResw.parent_path(),
                               paths.germanResw.parent_path(),
                               paths.pseudoResw.parent_path()})
    {
      auto error = std::error_code{};
      std::filesystem::create_directories(output, error);

      if (error)
      {
        return ao::makeError(
          ao::Error::Code::IoError,
          "Could not create catalog output directory " + ao::utility::pathToUtf8(output) + ": " + error.message());
      }
    }

    if (auto writeRes = writeFile(paths.pseudoSource, ao::i18n::detail::renderIcuResource("qps_Ploc", pseudo));
        !writeRes)
    {
      return writeRes;
    }

    auto englishReswMessagesRes = projectWinUiResources(*rootRes);

    if (!englishReswMessagesRes)
    {
      return std::unexpected{englishReswMessagesRes.error()};
    }

    if (auto appendRes = appendWinUiEnglishResources(*englishReswMessagesRes); !appendRes)
    {
      return appendRes;
    }

    auto germanReswMessagesRes = projectWinUiResources(*germanRes);

    if (!germanReswMessagesRes)
    {
      return std::unexpected{germanReswMessagesRes.error()};
    }

    auto pseudoReswMessagesRes = projectWinUiResources(pseudo);

    if (!pseudoReswMessagesRes)
    {
      return std::unexpected{pseudoReswMessagesRes.error()};
    }

    if (auto writeRes = writeFile(paths.englishResw, ao::i18n::detail::renderResw(*englishReswMessagesRes)); !writeRes)
    {
      return writeRes;
    }

    if (auto writeRes = writeFile(paths.germanResw, ao::i18n::detail::renderResw(*germanReswMessagesRes)); !writeRes)
    {
      return writeRes;
    }

    return writeFile(paths.pseudoResw, ao::i18n::detail::renderResw(*pseudoReswMessagesRes));
  }
} // namespace

int main(int const argc, char const* const argv[])
{
  try
  {
    auto optionsRes = parseOptions({argv, static_cast<std::size_t>(argc)});

    if (!optionsRes)
    {
      std::cerr << optionsRes.error().message << '\n';
      return 2;
    }

    if (auto result = run(*optionsRes); !result)
    {
      std::cerr << result.error().message << '\n';
      return 1;
    }

    return 0;
  }
  catch (std::exception const& error)
  {
    AO_AUDITED_CATCH(DiagnosticFallback);
    std::cerr << "Catalog compiler failed: " << error.what() << '\n';
    return 1;
  }
}
