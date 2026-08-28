// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutComponentStateStore.h"

#include <ao/Contract.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/AtomicFile.h>
#include <ao/utility/Path.h>
#include <ao/yaml/RymlAdapter.h>

#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    void validatePresetId(std::string_view presetId)
    {
      AO_EXPECTS(!(presetId.empty() || presetId.contains('/') || presetId.contains('\\') || presetId.contains("..") ||
                   presetId.contains('\0')),
                 "Invalid preset ID: path traversal attempt or empty ID");
    }
  } // namespace
  ShellLayoutComponentStateStore::ShellLayoutComponentStateStore(std::filesystem::path stateDir)
    : _stateDir{std::move(stateDir)}
  {
  }

  std::filesystem::path ShellLayoutComponentStateStore::filePath(std::string_view presetId) const
  {
    validatePresetId(presetId);
    return _stateDir / utility::pathFromUtf8(std::format("{}.yaml", presetId));
  }

  std::optional<uimodel::LayoutComponentStateDocument> ShellLayoutComponentStateStore::load(
    std::string_view presetId) const
  {
    auto const lock = std::scoped_lock{_mutex};
    return loadUnlocked(presetId);
  }

  std::optional<uimodel::LayoutComponentStateDocument> ShellLayoutComponentStateStore::loadUnlocked(
    std::string_view presetId) const
  {
    auto const path = filePath(presetId);

    auto const fileName = utility::pathToUtf8(path);
    auto bufferRes = yaml::readFileResult(path);

    if (!bufferRes)
    {
      auto existsEc = std::error_code{};

      if (auto const exists = std::filesystem::exists(path, existsEc);
          bufferRes.error().code != Error::Code::IoError || existsEc || exists)
      {
        APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to load state file ({}): {}",
                     utility::pathToUtf8(path),
                     bufferRes.error().message);
      }

      return std::nullopt;
    }

    auto buffer = std::move(*bufferRes);
    auto yamlErrorState = yaml::ErrorCallbackState{fileName};
    auto tree = ryml::Tree{yaml::callbacks()};

    if (auto const parsedRes = yaml::parseInPlace(tree, buffer, yamlErrorState); !parsedRes)
    {
      APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to parse state file ({}): {}",
                   utility::pathToUtf8(path),
                   parsedRes.error().message);
      return std::nullopt;
    }

    auto docRes =
      uimodel::LayoutComponentStateYamlSchema{}.deserialize(tree.rootref(), uimodel::LayoutComponentStateDocument{});

    if (!docRes)
    {
      APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to deserialize state file ({}): {}",
                   utility::pathToUtf8(path),
                   docRes.error().message);
      return std::nullopt;
    }

    if (docRes->preset != presetId)
    {
      APP_LOG_WARN("ShellLayoutComponentStateStore: Ignoring state file ({}) with mismatched preset '{}'",
                   utility::pathToUtf8(path),
                   docRes->preset);
      return std::nullopt;
    }

    return std::move(*docRes);
  }

  void ShellLayoutComponentStateStore::save(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc)
  {
    auto const lock = std::scoped_lock{_mutex};
    saveUnlocked(presetId, doc);
  }

  bool ShellLayoutComponentStateStore::saveUnlocked(std::string_view presetId,
                                                    uimodel::LayoutComponentStateDocument const& doc)
  {
    auto const path = filePath(presetId);
    auto stored = doc;
    stored.preset = presetId;

    auto tree = ryml::Tree{yaml::callbacks()};

    if (auto const serializedRes = uimodel::LayoutComponentStateYamlSchema{}.serialize(tree.rootref(), stored);
        !serializedRes)
    {
      APP_LOG_ERROR("ShellLayoutComponentStateStore: Failed to serialize state file ({}): {}",
                    utility::pathToUtf8(path),
                    serializedRes.error().message);
      return false;
    }

    auto const text = ryml::emitrs_yaml<std::string>(tree);

    if (auto const result = utility::writeAtomically(path, text); !result)
    {
      APP_LOG_ERROR("ShellLayoutComponentStateStore: Failed to save state file ({}): {}",
                    utility::pathToUtf8(path),
                    result.error().message);
      return false;
    }

    return true;
  }

  bool ShellLayoutComponentStateStore::prune(std::string_view presetId,
                                             uimodel::PreparedLayout const& layout,
                                             uimodel::LayoutSchema const& schema)
  {
    auto const lock = std::scoped_lock{_mutex};

    auto doc = loadUnlocked(presetId).value_or(uimodel::LayoutComponentStateDocument{.preset = std::string{presetId}});
    auto const beforeCount = doc.components.size();
    uimodel::pruneComponentState(doc, layout, schema);
    auto const changed =
      doc.components.size() != beforeCount || (doc.components.empty() && std::filesystem::exists(filePath(presetId)));

    if (doc.components.empty())
    {
      removePresetUnlocked(presetId);
      return changed;
    }

    if (!changed)
    {
      return false;
    }

    return saveUnlocked(presetId, doc);
  }

  bool ShellLayoutComponentStateStore::removePreset(std::string_view presetId)
  {
    auto const lock = std::scoped_lock{_mutex};
    return removePresetUnlocked(presetId);
  }

  bool ShellLayoutComponentStateStore::removePresetUnlocked(std::string_view presetId)
  {
    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    auto const existed = std::filesystem::exists(path, ec);
    std::filesystem::remove(path, ec);

    if (ec && existed)
    {
      APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to remove state file ({}): {}",
                   utility::pathToUtf8(path),
                   ec.message());
      return false;
    }

    return true;
  }
} // namespace ao::gtk
