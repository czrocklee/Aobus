// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/pfr.hpp>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ao::rt
{
  template<typename T>
  YAML::Node toYamlNode(T const& obj);

  template<typename T>
  void fromYamlNode(YAML::Node const& node, T& obj);

  namespace detail
  {
    template<typename T>
    struct is_optional : std::false_type
    {};
    template<typename T>
    struct is_optional<std::optional<T>> : std::true_type
    {};
    template<typename T>
    inline constexpr bool is_optional_v = is_optional<T>::value;

    template<typename T>
    struct is_vector : std::false_type
    {};
    template<typename T, typename A>
    struct is_vector<std::vector<T, A>> : std::true_type
    {};
    template<typename T>
    inline constexpr bool is_vector_v = is_vector<T>::value;

    template<typename T>
    struct is_map : std::false_type
    {};
    template<typename K, typename V, typename C, typename A>
    struct is_map<std::map<K, V, C, A>> : std::true_type
    {};
    template<typename T>
    inline constexpr bool is_map_v = is_map<T>::value;

    template<typename T>
    concept HasValueMethod = requires(T t) {
      { t.value() };
    };

    template<typename T>
    YAML::Node toYamlNodeImpl(T const& obj)
    {
      using Raw = std::remove_cvref_t<T>;

      if constexpr (std::is_same_v<Raw, std::string>)
      {
        return YAML::Node{obj};
      }
      else if constexpr (std::is_arithmetic_v<Raw>)
      {
        return YAML::Node{obj};
      }
      else if constexpr (std::is_enum_v<Raw>)
      {
        return YAML::Node{static_cast<int>(obj)};
      }
      else if constexpr (detail::is_optional_v<Raw>)
      {
        return obj ? detail::toYamlNodeImpl(*obj) : YAML::Node{};
      }
      else if constexpr (detail::is_vector_v<Raw>)
      {
        auto node = YAML::Node{};

        for (auto const& item : obj)
        {
          node.push_back(detail::toYamlNodeImpl(item));
        }

        return node;
      }
      else if constexpr (detail::is_map_v<Raw>)
      {
        auto node = YAML::Node{};

        for (auto const& [k, v] : obj)
        {
          node[k] = detail::toYamlNodeImpl(v);
        }

        return node;
      }
      else if constexpr (detail::HasValueMethod<Raw>)
      {
        return detail::toYamlNodeImpl(obj.value());
      }
      else
      {
        auto node = YAML::Node{};
        boost::pfr::for_each_field(obj,
                                   [&node, index = std::size_t{0}](auto const& field) mutable
                                   {
                                     constexpr auto names = boost::pfr::names_as_array<Raw>();
                                     auto key = std::string{names[index]};
                                     if (auto child = detail::toYamlNodeImpl(field); child.IsDefined())
                                     {
                                       node[key] = std::move(child);
                                     }

                                     ++index;
                                   });
        return node;
      }
    }

    template<typename T>
    void fromYamlNodeImpl(YAML::Node const& node, T& obj)
    {
      using Raw = std::remove_cvref_t<T>;

      if constexpr (std::is_same_v<Raw, std::string>)
      {
        obj = node.as<std::string>();
      }
      else if constexpr (std::is_arithmetic_v<Raw>)
      {
        obj = node.as<Raw>();
      }
      else if constexpr (std::is_enum_v<Raw>)
      {
        obj = static_cast<Raw>(node.as<int>());
      }
      else if constexpr (detail::is_optional_v<Raw>)
      {
        if (node.IsDefined() && !node.IsNull())
        {
          typename Raw::value_type value{};
          detail::fromYamlNodeImpl(node, value);
          obj = std::move(value);
        }
      }
      else if constexpr (detail::is_vector_v<Raw>)
      {
        obj.clear();

        for (auto const& item : node)
        {
          typename Raw::value_type value{};
          detail::fromYamlNodeImpl(item, value);
          obj.push_back(std::move(value));
        }
      }
      else if constexpr (detail::is_map_v<Raw>)
      {
        obj.clear();

        for (auto const& item : node)
        {
          typename Raw::mapped_type value{};
          detail::fromYamlNodeImpl(item.second, value);
          obj.emplace(item.first.as<typename Raw::key_type>(), std::move(value));
        }
      }
      else if constexpr (detail::HasValueMethod<Raw>)
      {
        using ValueType = std::remove_cvref_t<decltype(obj.value())>;
        auto value = ValueType{};
        detail::fromYamlNodeImpl(node, value);
        obj = Raw{std::move(value)};
      }
      else
      {
        boost::pfr::for_each_field(obj,
                                   [&node, index = std::size_t{0}](auto& field) mutable
                                   {
                                     constexpr auto names = boost::pfr::names_as_array<Raw>();

                                     if (auto child = node[std::string{names[index]}])
                                     {
                                       detail::fromYamlNodeImpl(child, field);
                                     }

                                     ++index;
                                   });
      }
    }
  } // namespace detail

  template<typename T>
  YAML::Node toYamlNode(T const& obj)
  {
    return detail::toYamlNodeImpl(obj);
  }

  template<typename T>
  void fromYamlNode(YAML::Node const& node, T& obj)
  {
    detail::fromYamlNodeImpl(node, obj);
  }

  class ConfigStore
  {
  public:
    virtual ~ConfigStore() = default;
    explicit ConfigStore(std::filesystem::path filePath);

    void flush();

    template<typename T>
    void save(std::string_view group, T const& obj)
    {
      ensureLoaded();
      _root[std::string{group}] = toYamlNode(obj);
    }

    template<typename T>
    void load(std::string_view group, T& obj)
    {
      ensureLoaded();
      if (auto child = _root[std::string{group}])
      {
        fromYamlNode(child, obj);
      }
    }

  private:
    void ensureLoaded();

    std::filesystem::path _filePath;
    YAML::Node _root;
    bool _loaded = false;
  };
}
