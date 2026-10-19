// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
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

  /**
   * @brief A schema for a protected group, which an older build never overwrites (decision 0020).
   *
   * The group is a mapping whose `version` field records the version that wrote
   * it, and `Schema::kVersion` is the version this build writes. The store reads
   * that field before the schema sees the group, so a newer group is reported as
   * newer rather than as whatever its unknown content would fail as. A save
   * whose schema writes another version, or none, stops the process.
   */
  template<typename Schema, typename T>
  concept ProtectedConfigSchema = ConfigSchema<Schema, T> && requires {
    { Schema::kVersion } -> std::convertible_to<std::uint32_t>;
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

    /// Names the constructor below; see it for what a located-nowhere store does.
    struct NoLocation final
    {};

    /**
     * @brief A store for a session that has nowhere to keep anything.
     *
     * A frontend reaches this when the platform names no home or profile
     * location at all. Every group reads as absent and every write succeeds
     * having stored nothing, so the session runs on defaults and forgets them
     * on exit rather than failing to start or writing into whatever directory
     * it happened to be launched from.
     *
     * Writes report success deliberately: the session never promised to keep
     * anything, so a caller reporting a save failure at every checkpoint would
     * be describing a fault that is not one. Whoever builds this says so once,
     * at the composition root where the reason is still known.
     */
    explicit ConfigStore(NoLocation /*noLocation*/);

    /// Whether this store has somewhere to read from and write to.
    bool hasLocation() const noexcept { return _hasLocation; }

    /// Whether a protected load or save has found @p group written by a newer version.
    bool hasNewerGroup(std::string_view group) const;

    Result<bool> contains(std::string_view group);

    /// Fails as `NotSupported`, removing nothing, for a group `hasNewerGroup` reports.
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
      if (auto const res = ensureLoaded(); !res)
      {
        return std::unexpected{res.error()};
      }

      auto const child = _root.rootref()[yaml::toCsubstr(group)];

      if (!child.readable())
      {
        return false;
      }

      if constexpr (ProtectedConfigSchema<Schema, T>)
      {
        if (auto const res = refuseNewerGroup(group, Schema::kVersion); !res)
        {
          return std::unexpected{res.error()};
        }
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

    /// Fails as `NotSupported`, and remembers @p group, when its stored `version` exceeds @p currentVersion.
    Result<> refuseNewerGroup(std::string_view group, std::uint32_t currentVersion);

    template<typename T, ConfigSchema<T> Schema>
    Result<> serializeGroup(ryml::Tree& candidate, std::string_view group, T const& value, Schema const& schema)
    {
      if constexpr (ProtectedConfigSchema<Schema, T>)
      {
        if (auto const res = refuseNewerGroup(group, Schema::kVersion); !res)
        {
          return res;
        }
      }

      auto root = candidate.rootref();

      if (auto const groupName = yaml::toCsubstr(group); root[groupName].readable())
      {
        root.remove_child(groupName);
      }

      auto output = yaml::appendChild(root, group);

      if (auto const res = schema.serialize(output, value); !res)
      {
        return std::unexpected{withGroupContext(res.error(), "serialize", group)};
      }

      if constexpr (ProtectedConfigSchema<Schema, T>)
      {
        // An older build recognizes a newer group only by this field.
        auto const writtenVersionRes = yaml::requireScalar<std::uint32_t>(output, "version", "protected config group");
        AO_INVARIANT(writtenVersionRes && *writtenVersionRes == Schema::kVersion,
                     "A protected config schema must write its version");
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
      auto const serialize = [this, &candidate, &serializedRes](auto const& write)
      {
        if (serializedRes)
        {
          serializedRes = this->serializeGroup(candidate, write.group, write.value, write.schema);
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
    std::vector<std::string> _newerGroups;
    bool _hasLocation = true;
    bool _loaded = false;
  };
} // namespace ao::rt
