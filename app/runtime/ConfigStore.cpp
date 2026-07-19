// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/utility/AtomicFile.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <array>
#include <cassert>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ao::rt
{
  ConfigStore::ConfigStore(std::filesystem::path filePath, OpenMode mode)
    : _filePath{std::move(filePath)}, _yamlErrorState{_filePath.string()}, _mode{mode}
  {
  }

  Result<bool> ConfigStore::contains(std::string_view const group)
  {
    if (auto const loaded = ensureLoaded(); !loaded)
    {
      return std::unexpected{loaded.error()};
    }

    return _root.is_map(0) && _root.rootref()[yaml::toCsubstr(group)].readable();
  }

  Result<> ConfigStore::removeGroup(std::string_view const group)
  {
    auto candidateResult = prepareWriteCandidate();

    if (!candidateResult)
    {
      return std::unexpected{candidateResult.error()};
    }

    auto candidate = std::move(*candidateResult);
    auto const groupName = yaml::toCsubstr(group);
    auto root = candidate.rootref();

    if (!root[groupName].readable())
    {
      return {};
    }

    root.remove_child(groupName);
    return commitCandidate(std::move(candidate));
  }

  Result<> ConfigStore::ensureLoaded()
  {
    if (_loaded)
    {
      return {};
    }

    auto fileEc = std::error_code{};
    auto const fileExists = std::filesystem::exists(_filePath, fileEc);

    if (fileEc)
    {
      return makeError(Error::Code::IoError,
                       std::format("Failed to inspect config file '{}': {}", _filePath.string(), fileEc.message()));
    }

    if (!fileExists)
    {
      if (_mode == OpenMode::ReadOnly)
      {
        return makeError(Error::Code::NotFound, std::format("Config file not found: {}", _filePath.string()));
      }

      _root.to_map(0);
      _loaded = true;
      return {};
    }

    auto bufferResult = yaml::readFileResult(_filePath);

    if (!bufferResult)
    {
      return std::unexpected{bufferResult.error()};
    }

    auto inputBuffer = std::move(*bufferResult);
    auto root = ryml::Tree{yaml::callbacks(_yamlErrorState)};

    try
    {
      yaml::parseInPlace(root, inputBuffer, _yamlErrorState);
    }
    catch (std::exception const& e)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Failed to parse config file '{}': {}", _filePath.string(), e.what()));
    }

    if (!root.is_map(0))
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Config file '{}' does not contain a top-level mapping", _filePath.string()));
    }

    constexpr auto kNoFixedGroups = std::array<std::string_view, 0>{};

    if (auto const result =
          yaml::validateMapKeys(root.rootref(), kNoFixedGroups, "config document", yaml::UnknownKeyPolicy::Allow);
        !result)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Config file '{}': {}", _filePath.string(), result.error().message));
    }

    _inputBuffer = std::move(inputBuffer);
    _root = std::move(root);
    _loaded = true;
    return {};
  }

  Result<ryml::Tree> ConfigStore::prepareWriteCandidate()
  {
    if (_mode == OpenMode::ReadOnly)
    {
      throwException<Exception>("write called on ReadOnly ConfigStore");
    }

    if (auto const loaded = ensureLoaded(); !loaded)
    {
      return std::unexpected{loaded.error()};
    }

    assert(_root.is_map(0) && "Successful ConfigStore initialization must establish a top-level mapping");

    // The complete-tree snapshot isolates serialization failures until atomic replacement succeeds.
    return _root;
  }

  Result<> ConfigStore::commitCandidate(ryml::Tree&& candidate)
  {
    static_assert(std::is_nothrow_move_assignable_v<ryml::Tree>);

    APP_LOG_INFO("Saving config to: {}", _filePath.string());

    auto const yaml = ryml::emitrs_yaml<std::string>(candidate);

    if (auto const written = utility::writeAtomically(_filePath, yaml); !written)
    {
      return written;
    }

    _root = std::move(candidate);
    return {};
  }
} // namespace ao::rt
