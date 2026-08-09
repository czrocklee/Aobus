// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt
{
  template<typename Schema, typename T>
  concept ConfigSchema =
    requires(Schema const& schema, ryml::NodeRef output, ryml::ConstNodeRef input, T const& value) {
      { schema.serialize(output, value) } -> std::same_as<Result<>>;
      { schema.deserialize(input, value) } -> std::same_as<Result<T>>;
    };

  template<typename T, ConfigSchema<T> Schema>
  struct ConfigWrite final
  {
    std::string_view group;
    T const& value;
    Schema schema;
  };

  template<typename T, ConfigSchema<T> Schema>
  ConfigWrite<T, Schema> configWrite(std::string_view group, T const& value, Schema schema)
  {
    return {.group = group, .value = value, .schema = std::move(schema)};
  }

  class ConfigStore final
  {
  public:
    enum class OpenMode : std::uint8_t
    {
      ReadWrite, // file may not exist yet, will be created on save
      ReadOnly,  // file must already exist, NotFound is an error for load()
    };

    ~ConfigStore() = default;

    ConfigStore(ConfigStore const&) = delete;
    ConfigStore& operator=(ConfigStore const&) = delete;
    ConfigStore(ConfigStore&&) = delete;
    ConfigStore& operator=(ConfigStore&&) = delete;

    /// @param optMaxFileBytes Optional ceiling applied before parsing and before replacement.
    explicit ConfigStore(std::filesystem::path filePath,
                         OpenMode mode = OpenMode::ReadWrite,
                         std::optional<std::size_t> optMaxFileBytes = std::nullopt);

    Result<bool> contains(std::string_view group);
    Result<> removeGroup(std::string_view group);

    template<typename T, ConfigSchema<T> Schema>
    Result<> save(std::string_view group, T const& value, Schema schema)
    {
      return saveWrites(configWrite(group, value, std::move(schema)));
    }

    template<typename... T, typename... Schema>
      requires(sizeof...(T) > 1)
    Result<> saveTogether(ConfigWrite<T, Schema> const&... writes)
    {
      return saveWrites(writes...);
    }

    template<typename T, ConfigSchema<T> Schema>
      requires std::is_move_assignable_v<T>
    Result<bool> load(std::string_view group, T& value, Schema const& schema)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return std::unexpected{result.error()};
      }

      auto const child = _root.rootref()[yaml::toCsubstr(group)];

      if (!child.readable())
      {
        return false;
      }

      auto deserializedRes = schema.deserialize(child, value);

      if (!deserializedRes)
      {
        return std::unexpected{withGroupContext(deserializedRes.error(), "deserialize", group)};
      }

      value = std::move(*deserializedRes);
      return true;
    }

  private:
    static Error withGroupContext(Error error, std::string_view operation, std::string_view group);

    template<typename T, ConfigSchema<T> Schema>
    static Result<> serializeGroup(ryml::Tree& candidate, std::string_view group, T const& value, Schema const& schema)
    {
      auto root = candidate.rootref();

      if (auto const groupName = yaml::toCsubstr(group); root[groupName].readable())
      {
        root.remove_child(groupName);
      }

      auto output = yaml::appendChild(root, group);

      if (auto const result = schema.serialize(output, value); !result)
      {
        return std::unexpected{withGroupContext(result.error(), "serialize", group)};
      }

      return {};
    }

    template<typename... T, typename... Schema>
    Result<> saveWrites(ConfigWrite<T, Schema> const&... writes)
    {
      auto candidateRes = prepareWriteCandidate();

      if (!candidateRes)
      {
        return std::unexpected{candidateRes.error()};
      }

      auto candidate = std::move(*candidateRes);

      auto serializedRes = Result<>{};
      auto const serialize = [&candidate, &serializedRes](auto const& write)
      {
        if (serializedRes)
        {
          serializedRes = serializeGroup(candidate, write.group, write.value, write.schema);
        }
      };
      (serialize(writes), ...);

      if (!serializedRes)
      {
        return serializedRes;
      }

      return commitCandidate(std::move(candidate));
    }

    Result<> ensureLoaded();
    Result<ryml::Tree> prepareWriteCandidate();
    Result<> commitCandidate(ryml::Tree&& candidate);

    std::filesystem::path _filePath;
    yaml::ErrorCallbackState _yamlErrorState;
    OpenMode _mode = OpenMode::ReadWrite;
    ryml::Tree _root;
    std::vector<char> _inputBuffer;
    std::optional<std::size_t> _optMaxFileBytes;
    bool _loaded = false;
  };
} // namespace ao::rt
