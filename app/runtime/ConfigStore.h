// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <boost/pfr.hpp>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

// yaml-cpp does not provide convert for std::optional or enum types.
// Provide partial specializations so ConfigStore can unify all types
// through the YAML::convert mechanism.

namespace YAML
{
  template<typename T>
  struct convert<std::optional<T>>
  {
    static Node encode(std::optional<T> const& rhs) { return rhs ? Node{*rhs} : Node{}; }

    static bool decode(Node const& node, std::optional<T>& rhs)
    {
      if (node.IsDefined() && !node.IsNull())
      {
        rhs = node.as<T>();
      }

      return true;
    }
  };

  template<typename T>
  concept EnumType = std::is_enum_v<T>;

  template<EnumType T>
  struct convert<T>
  {
    static Node encode(T const& rhs) { return Node{static_cast<int>(rhs)}; }

    static bool decode(Node const& node, T& rhs)
    {
      rhs = static_cast<T>(node.as<int>());
      return true;
    }
  };

  template<typename T>
  concept PfrAggregate = std::is_aggregate_v<T>;

  template<PfrAggregate T>
  struct convert<T>
  {
    static Node encode(T const& obj)
    {
      auto node = Node{};
      boost::pfr::for_each_field(obj,
                                 [&node, index = std::size_t{0}](auto const& field) mutable
                                 {
                                   constexpr auto names = boost::pfr::names_as_array<T>();

                                   if (auto const child = Node(field); child.IsDefined())
                                   {
                                     node[std::string{names[index]}] = std::move(child);
                                   }

                                   ++index;
                                 });
      return node;
    }

    static bool decode(Node const& n, T& obj)
    {
      if (!n.IsMap())
      {
        return false;
      }

      boost::pfr::for_each_field(obj,
                                 [&n, index = std::size_t{0}](auto& field) mutable
                                 {
                                   constexpr auto names = boost::pfr::names_as_array<T>();

                                   if (auto const child = n[std::string{names[index]}])
                                   {
                                     field = child.as<std::remove_cvref_t<decltype(field)>>();
                                   }

                                   ++index;
                                 });
      return true;
    }
  };

  template<typename T>
  concept HasValueMethod = requires(T const& t) {
    { t.value() };
  } && !std::is_aggregate_v<T>;

  template<HasValueMethod T>
  struct convert<T>
  {
    static Node encode(T const& rhs) { return Node{rhs.value()}; }

    static bool decode(Node const& node, T& rhs)
    {
      using ValueType = std::remove_cvref_t<decltype(std::declval<T const&>().value())>;
      rhs = T{node.as<ValueType>()};
      return true;
    }
  };
} // namespace YAML

namespace ao::rt
{
  class ConfigStore final
  {
  public:
    enum class OpenMode : std::uint8_t
    {
      ReadWrite, // file may not exist yet, will be created on flush
      ReadOnly,  // file must already exist, NotFound is an error for load()
    };

    virtual ~ConfigStore() = default;
    explicit ConfigStore(std::filesystem::path filePath, OpenMode mode = OpenMode::ReadWrite);

    ao::Result<> flush();

    template<typename T>
    void save(std::string_view group, T const& obj)
    {
      if (_mode == OpenMode::ReadOnly)
      {
        throw std::logic_error{"save() called on ReadOnly ConfigStore"};
      }

      if (auto const result = ensureLoaded(); !result && result.error().code != ao::Error::Code::NotFound)
      {
        return;
      }

      _root[std::string{group}] = YAML::Node{obj};
    }

    template<typename T>
    ao::Result<> load(std::string_view group, T& obj)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return result;
      }

      if (auto const child = _root[std::string{group}])
      {
        try
        {
          obj = child.as<T>();
        }
        catch (std::exception const& e)
        {
          return makeError(
            ao::Error::Code::FormatRejected, std::format("Failed to decode config key '{}': {}", group, e.what()));
        }
      }

      return {};
    }

  private:
    ao::Result<> ensureLoaded();

    std::filesystem::path _filePath;
    OpenMode _mode = OpenMode::ReadWrite;
    YAML::Node _root;
    bool _loaded = false;
  };
}
