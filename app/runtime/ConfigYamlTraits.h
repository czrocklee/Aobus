// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/pfr/core.hpp>
#include <boost/pfr/core_name.hpp>
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

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
                                   constexpr auto kNames = boost::pfr::names_as_array<T>();

                                   if (auto const child = Node(field); child.IsDefined())
                                   {
                                     node[std::string{kNames.at(index)}] = std::move(child);
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
                                   constexpr auto kNames = boost::pfr::names_as_array<T>();

                                   if (auto const child = n[std::string{kNames.at(index)}])
                                   {
                                     field = child.as<std::remove_cvref_t<decltype(field)>>();
                                   }

                                   ++index;
                                 });
      return true;
    }
  };

  template<typename T>
  concept HasRawMethod = requires(T const& obj) {
    { obj.raw() };
  } && !std::is_aggregate_v<T>;

  template<HasRawMethod T>
  struct convert<T>
  {
    static Node encode(T const& rhs) { return Node{rhs.raw()}; }

    static bool decode(Node const& node, T& rhs)
    {
      using ValueType = std::remove_cvref_t<decltype(std::declval<T const&>().raw())>;
      rhs = T{node.as<ValueType>()};
      return true;
    }
  };
} // namespace YAML
