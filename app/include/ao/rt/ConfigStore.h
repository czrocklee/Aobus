// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/yaml/ConfigTraits.h> // IWYU pragma: export
#include <ao/yaml/RymlAdapter.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt
{
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

    explicit ConfigStore(std::filesystem::path filePath, OpenMode mode = OpenMode::ReadWrite);

    Result<bool> contains(std::string_view group);
    Result<> removeGroup(std::string_view group);

    template<typename T, typename... Remaining>
      requires(sizeof...(Remaining) % 2 == 0)
    Result<> save(std::string_view group, T const& obj, Remaining const&... remaining)
    {
      auto candidateResult = prepareWriteCandidate();

      if (!candidateResult)
      {
        return std::unexpected{candidateResult.error()};
      }

      auto candidate = std::move(*candidateResult);
      writeGroups(candidate, group, obj, remaining...);
      return commitCandidate(std::move(candidate));
    }

    template<typename T>
      requires(std::is_copy_constructible_v<T> && std::is_move_assignable_v<T>)
    Result<> load(std::string_view group, T& obj)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return result;
      }

      if (_root.is_map(0))
      {
        if (auto const child = _root.rootref()[yaml::toCsubstr(group)]; child.readable())
        {
          auto candidate = obj;

          try
          {
            if (!yaml::read(child, candidate))
            {
              return makeError(Error::Code::FormatRejected, std::format("Failed to decode config key '{}'", group));
            }
          }
          catch (std::exception const& e)
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Failed to decode config key '{}': {}", group, e.what()));
          }

          obj = std::move(candidate);
        }
      }

      return {};
    }

    /** Strict recursive aggregate/vector decoding for explicitly versioned payloads. */
    template<typename T>
      requires(std::is_copy_constructible_v<T> && std::is_move_assignable_v<T>)
    Result<> loadExact(std::string_view group, T& obj)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return result;
      }

      if (_root.is_map(0))
      {
        if (auto const child = _root.rootref()[yaml::toCsubstr(group)]; child.readable())
        {
          auto candidate = obj;

          try
          {
            if (!yaml::readExact(child, candidate))
            {
              return makeError(
                Error::Code::FormatRejected, std::format("Failed to decode exact config key '{}'", group));
            }
          }
          catch (std::exception const& e)
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Failed to decode exact config key '{}': {}", group, e.what()));
          }

          obj = std::move(candidate);
        }
      }

      return {};
    }

  private:
    // String-literal group names use an array reference to preserve their length without decay.
    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    template<std::size_t N>
    static constexpr std::string_view asGroupView(char const (&group)[N]) noexcept
    {
      return {&group[0], N - 1};
    }
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

    template<typename Group>
    static std::string_view asGroupView(Group const& group)
    {
      return std::string_view{group};
    }

    template<typename T>
    static void writeGroup(ryml::Tree& candidate, std::string_view group, T const& obj)
    {
      auto const groupName = yaml::toCsubstr(group);
      auto child = candidate.rootref()[groupName];

      if (!child.readable())
      {
        child = candidate.rootref().append_child();
        yaml::setKey(child, group);
      }
      else
      {
        child.clear_children();
      }

      yaml::write(child, obj);
    }

    template<typename T>
    static void writeGroups(ryml::Tree& candidate, std::string_view group, T const& obj)
    {
      writeGroup(candidate, group, obj);
    }

    template<typename T, typename NextGroup, typename NextT, typename... Remaining>
    static void writeGroups(ryml::Tree& candidate,
                            std::string_view group,
                            T const& obj,
                            NextGroup const& nextGroup,
                            NextT const& nextObj,
                            Remaining const&... remaining)
    {
      writeGroup(candidate, group, obj);
      writeGroups(candidate, asGroupView(nextGroup), nextObj, remaining...);
    }

    Result<> ensureLoaded();
    Result<ryml::Tree> prepareWriteCandidate();
    Result<> commitCandidate(ryml::Tree&& candidate);

    std::filesystem::path _filePath;
    yaml::ErrorCallbackState _yamlErrorState;
    OpenMode _mode = OpenMode::ReadWrite;
    ryml::Tree _root;
    std::vector<char> _inputBuffer;
    bool _loaded = false;
  };
} // namespace ao::rt
