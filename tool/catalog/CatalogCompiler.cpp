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
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  struct TranslationTarget final
  {
    std::filesystem::path resourceFile;
    std::filesystem::path reswFile;
  };

  struct GeneratedFile final
  {
    std::filesystem::path path;
    std::string content;
  };

  struct Options final
  {
    std::filesystem::path rootResource;
    std::filesystem::path pseudoSource;
    std::filesystem::path englishResw;
    std::filesystem::path pseudoResw;
    std::vector<TranslationTarget> translations;
  };

  using PathMember = std::filesystem::path Options::*;

  struct SinglePathOption final
  {
    std::string_view name;
    PathMember destination;
    bool assigned = false;
  };

  constexpr auto kUsage = std::string_view{
    "Usage: ao_catalog_compiler --root-res PATH --pseudo-source PATH --en-resw PATH --pseudo-resw PATH "
    "[--translation RES_PATH RESW_PATH]..."};

  ao::Result<Options> parseOptions(std::span<char const* const> const args)
  {
    auto options = Options{};
    auto singlePathOptions = std::array{
      SinglePathOption{.name = "--root-res", .destination = &Options::rootResource},
      SinglePathOption{.name = "--pseudo-source", .destination = &Options::pseudoSource},
      SinglePathOption{.name = "--en-resw", .destination = &Options::englishResw},
      SinglePathOption{.name = "--pseudo-resw", .destination = &Options::pseudoResw},
    };
    auto* const singlePathOptionsEnd = singlePathOptions.data() + singlePathOptions.size();

    for (std::size_t index = 1; index < args.size();)
    {
      auto const name = std::string_view{args[index]};
      auto* const singlePathOption =
        std::ranges::find(singlePathOptions.data(), singlePathOptionsEnd, name, &SinglePathOption::name);

      if (singlePathOption != singlePathOptionsEnd)
      {
        if (singlePathOption->assigned)
        {
          return ao::makeError(ao::Error::Code::InvalidInput, "Duplicate option: " + std::string{name});
        }

        if (index + 1 >= args.size())
        {
          return ao::makeError(ao::Error::Code::InvalidInput, "Missing value for " + std::string{name});
        }

        options.*(singlePathOption->destination) = ao::utility::pathFromUtf8(args[index + 1]);
        singlePathOption->assigned = true;
        index += 2;
        continue;
      }

      if (name == "--translation")
      {
        if (index + 2 >= args.size())
        {
          return ao::makeError(ao::Error::Code::InvalidInput, std::string{kUsage});
        }

        auto resourcePath = ao::utility::pathFromUtf8(args[index + 1]);
        auto reswPath = ao::utility::pathFromUtf8(args[index + 2]);
        options.translations.push_back({
          .resourceFile = std::move(resourcePath),
          .reswFile = std::move(reswPath),
        });
        index += 3;
        continue;
      }

      return ao::makeError(ao::Error::Code::InvalidInput, std::string{"Unknown option: "} + std::string{name});
    }

    if (!std::ranges::all_of(singlePathOptions, &SinglePathOption::assigned))
    {
      return ao::makeError(ao::Error::Code::InvalidInput, std::string{kUsage});
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
    auto makeAbsolute = [](std::filesystem::path& path) -> ao::Result<>
    {
      auto error = std::error_code{};
      auto absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return ao::makeError(
          ao::Error::Code::IoError,
          "Could not resolve catalog path " + ao::utility::pathToUtf8(path) + ": " + error.message());
      }

      path = std::move(absolute);
      return {};
    };

    auto const fixedPaths = std::array{
      &options.rootResource,
      &options.pseudoSource,
      &options.englishResw,
      &options.pseudoResw,
    };

    for (auto* const path : fixedPaths)
    {
      if (auto res = makeAbsolute(*path); !res)
      {
        return std::unexpected{res.error()};
      }
    }

    for (auto& translation : options.translations)
    {
      auto const translationPaths = std::array{
        &translation.resourceFile,
        &translation.reswFile,
      };

      for (auto* const path : translationPaths)
      {
        if (auto res = makeAbsolute(*path); !res)
        {
          return std::unexpected{res.error()};
        }
      }
    }

    auto outputPaths = std::vector{options.englishResw, options.pseudoResw};

    for (auto const& translation : options.translations)
    {
      if (std::ranges::contains(outputPaths, translation.reswFile))
      {
        return ao::makeError(
          ao::Error::Code::InvalidInput,
          "Catalog outputs resolve to the same path: " + ao::utility::pathToUtf8(translation.reswFile));
      }

      outputPaths.push_back(translation.reswFile);
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

    if (auto rootIdsRes = validateRootIds(*rootRes); !rootIdsRes)
    {
      return rootIdsRes;
    }

    auto generatedFiles = std::vector<GeneratedFile>{};
    generatedFiles.reserve(paths.translations.size() + 3U);

    for (auto const& translation : paths.translations)
    {
      auto const translationDirectory = ao::utility::pathToUtf8(translation.resourceFile.parent_path());
      auto translationRes = ao::i18n::detail::loadCompiledCatalog(
        translationDirectory, ao::utility::pathToUtf8(translation.resourceFile.stem()));

      if (!translationRes)
      {
        return std::unexpected{translationRes.error()};
      }

      if (auto validationRes = ao::i18n::detail::validateTranslationCatalog(*rootRes, *translationRes); !validationRes)
      {
        return validationRes;
      }

      auto translationMessagesRes =
        ao::i18n::detail::projectWinUiResources(*translationRes, ao::i18n::detail::MissingWinUiMessagePolicy::Omit);

      if (!translationMessagesRes)
      {
        return std::unexpected{translationMessagesRes.error()};
      }

      generatedFiles.push_back({
        .path = translation.reswFile,
        .content = ao::i18n::detail::renderResw(*translationMessagesRes),
      });
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

    auto englishReswMessagesRes =
      ao::i18n::detail::projectWinUiResources(*rootRes, ao::i18n::detail::MissingWinUiMessagePolicy::Reject);

    if (!englishReswMessagesRes)
    {
      return std::unexpected{englishReswMessagesRes.error()};
    }

    if (auto appendRes = appendWinUiEnglishResources(*englishReswMessagesRes); !appendRes)
    {
      return appendRes;
    }

    auto pseudoReswMessagesRes =
      ao::i18n::detail::projectWinUiResources(pseudo, ao::i18n::detail::MissingWinUiMessagePolicy::Reject);

    if (!pseudoReswMessagesRes)
    {
      return std::unexpected{pseudoReswMessagesRes.error()};
    }

    generatedFiles.push_back({
      .path = paths.pseudoSource,
      .content = ao::i18n::detail::renderIcuResource("qps_Ploc", pseudo),
    });
    generatedFiles.push_back({
      .path = paths.englishResw,
      .content = ao::i18n::detail::renderResw(*englishReswMessagesRes),
    });
    generatedFiles.push_back({
      .path = paths.pseudoResw,
      .content = ao::i18n::detail::renderResw(*pseudoReswMessagesRes),
    });

    for (auto const& generatedFile : generatedFiles)
    {
      auto error = std::error_code{};
      std::filesystem::create_directories(generatedFile.path.parent_path(), error);

      if (error)
      {
        return ao::makeError(ao::Error::Code::IoError,
                             "Could not create catalog output directory " +
                               ao::utility::pathToUtf8(generatedFile.path.parent_path()) + ": " + error.message());
      }
    }

    for (auto const& generatedFile : generatedFiles)
    {
      if (auto writeRes = writeFile(generatedFile.path, generatedFile.content); !writeRes)
      {
        return writeRes;
      }
    }

    return {};
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
