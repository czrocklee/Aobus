// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/utility/Log.h"
#include <ao/rt/yaml/Utils.h>

#include <boost/pfr/core.hpp>
#include <boost/pfr/core_name.hpp>
#include <ryml.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::yaml
{
  // ── Concepts ────────────────────────────────────────────────────────────────

  template<typename T>
  concept Arithmetic = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

  template<typename T>
  concept EnumType = std::is_enum_v<T>;

  template<typename T>
  concept HasRawMethod = requires(T const& obj) {
    { obj.raw() };
  } && !std::is_aggregate_v<T>;

  template<typename T>
  concept PfrAggregate =
    std::is_aggregate_v<T> && !requires { std::declval<T>().raw(); } && !requires { typename T::value_type; };

  // ── Forward Declarations ────────────────────────────────────────────────────

  inline void write(ryml::NodeRef node, bool value);
  inline bool read(ryml::ConstNodeRef node, bool& value);

  template<Arithmetic T>
  void write(ryml::NodeRef node, T value);
  template<Arithmetic T>
  bool read(ryml::ConstNodeRef node, T& value);

  inline void write(ryml::NodeRef node, std::string_view value);
  inline void write(ryml::NodeRef node, std::string const& value);
  inline bool read(ryml::ConstNodeRef node, std::string& value);

  inline void write(ryml::NodeRef node, std::filesystem::path const& rhs);
  inline bool read(ryml::ConstNodeRef node, std::filesystem::path& rhs);

  template<typename T>
  void write(ryml::NodeRef node, std::optional<T> const& rhs);
  template<typename T>
  bool read(ryml::ConstNodeRef node, std::optional<T>& rhs);

  template<EnumType T>
  void write(ryml::NodeRef node, T const& rhs);
  template<EnumType T>
  bool read(ryml::ConstNodeRef node, T& rhs);

  template<HasRawMethod T>
  void write(ryml::NodeRef node, T const& rhs);
  template<HasRawMethod T>
  bool read(ryml::ConstNodeRef node, T& rhs);

  template<typename T>
  void write(ryml::NodeRef node, std::vector<T> const& rhs);
  template<typename T>
  bool read(ryml::ConstNodeRef node, std::vector<T>& rhs);

  template<typename T, std::size_t N>
  void write(ryml::NodeRef node, std::array<T, N> const& rhs);
  template<typename T, std::size_t N>
  bool read(ryml::ConstNodeRef node, std::array<T, N>& rhs);

  template<typename T>
  void write(ryml::NodeRef node, std::map<std::string, T, std::less<>> const& rhs);
  template<typename T>
  bool read(ryml::ConstNodeRef node, std::map<std::string, T, std::less<>>& rhs);

  template<typename T>
  void write(ryml::NodeRef node, std::map<ListId, T> const& rhs);
  template<typename T>
  inline bool read(ryml::ConstNodeRef node, std::map<ListId, T>& rhs);

  template<PfrAggregate T>
  void write(ryml::NodeRef node, T const& obj);
  template<PfrAggregate T>
  bool read(ryml::ConstNodeRef node, T& obj);

  // ── Basic Types ─────────────────────────────────────────────────────────────

  inline void write(ryml::NodeRef node, std::string_view value)
  {
    setValue(node, value);
  }

  inline void write(ryml::NodeRef node, std::string const& value)
  {
    write(node, std::string_view{value});
  }

  inline bool read(ryml::ConstNodeRef node, std::string& value)
  {
    if (!node.has_val())
    {
      return false;
    }

    auto const view = scalarView(node);
    value = view;
    return true;
  }

  // ── Numeric overloads ───────────────────────────────────────────────────────

  template<Arithmetic T>
  inline void write(ryml::NodeRef node, T value)
  {
    node << value;
  }

  template<Arithmetic T>
  inline bool read(ryml::ConstNodeRef node, T& value)
  {
    if (!node.has_val())
    {
      return false;
    }

    node >> value;
    return true;
  }

  inline void write(ryml::NodeRef node, bool value)
  {
    node << value;
  }

  inline bool read(ryml::ConstNodeRef node, bool& value)
  {
    if (!node.has_val())
    {
      return false;
    }

    node >> value;
    return true;
  }

  inline void write(ryml::NodeRef node, std::filesystem::path const& rhs)
  {
    write(node, rhs.string());
  }

  inline bool read(ryml::ConstNodeRef node, std::filesystem::path& rhs)
  {
    if (!node.has_val())
    {
      return false;
    }

    rhs = scalarView(node);
    return true;
  }

  template<typename T>
  inline void write(ryml::NodeRef node, std::optional<T> const& rhs)
  {
    if (rhs)
    {
      write(node, *rhs);
    }
    else
    {
      node << nullptr;
    }
  }

  template<typename T>
  inline bool read(ryml::ConstNodeRef node, std::optional<T>& rhs)
  {
    if (!node.readable() || (node.has_val() && node.val_is_null()))
    {
      rhs = std::nullopt;
      return true;
    }

    T val;

    if (read(node, val))
    {
      rhs = std::move(val);
      return true;
    }

    rhs = std::nullopt;
    return false;
  }

  template<EnumType T>
  inline void write(ryml::NodeRef node, T const& rhs)
  {
    node << static_cast<int>(rhs);
  }

  template<EnumType T>
  inline bool read(ryml::ConstNodeRef node, T& rhs)
  {
    if (int val = 0; read(node, val))
    {
      rhs = static_cast<T>(val);
      return true;
    }

    return false;
  }

  template<HasRawMethod T>
  inline void write(ryml::NodeRef node, T const& rhs)
  {
    write(node, rhs.raw());
  }

  template<HasRawMethod T>
  inline bool read(ryml::ConstNodeRef node, T& rhs)
  {
    using ValueType = std::remove_cvref_t<decltype(std::declval<T const&>().raw())>;
    ValueType val;

    if (read(node, val))
    {
      rhs = T{std::move(val)};
      return true;
    }

    return false;
  }

  template<typename T>
  inline void write(ryml::NodeRef node, std::vector<T> const& rhs)
  {
    node |= ryml::SEQ;

    for (auto const& item : rhs)
    {
      auto child = node.append_child();
      write(child, item);
    }
  }

  template<typename T>
  inline bool read(ryml::ConstNodeRef node, std::vector<T>& rhs)
  {
    if (!node.is_seq())
    {
      return false;
    }

    rhs.clear();
    rhs.reserve(node.num_children());

    for (auto const& child : node.children())
    {
      if (auto val = T{}; read(child, val))
      {
        rhs.push_back(std::move(val));
      }
    }

    return true;
  }

  template<typename T, std::size_t N>
  inline void write(ryml::NodeRef node, std::array<T, N> const& rhs)
  {
    node |= ryml::SEQ;

    for (auto const& item : rhs)
    {
      auto child = node.append_child();
      write(child, item);
    }
  }

  template<typename T, std::size_t N>
  inline bool read(ryml::ConstNodeRef node, std::array<T, N>& rhs)
  {
    if (!node.is_seq())
    {
      return false;
    }

    std::size_t index = 0;

    for (auto const& child : node.children())
    {
      if (index >= N)
      {
        break;
      }

      if (auto val = T{}; read(child, val))
      {
        rhs.at(index) = std::move(val);
      }

      ++index;
    }

    return true;
  }

  template<typename T>
  inline void write(ryml::NodeRef node, std::map<std::string, T, std::less<>> const& rhs)
  {
    node |= ryml::MAP;

    for (auto const& [key, value] : rhs)
    {
      auto child = node.append_child();
      setKey(child, key);
      write(child, value);
    }
  }

  template<typename T>
  inline bool read(ryml::ConstNodeRef node, std::map<std::string, T, std::less<>>& rhs)
  {
    if (!node.is_map())
    {
      return false;
    }

    rhs.clear();

    for (auto const& child : node.children())
    {
      if (auto val = T{}; read(child, val))
      {
        rhs.emplace(keyView(child), std::move(val));
      }
    }

    return true;
  }

  template<typename T>
  inline void write(ryml::NodeRef node, std::map<ListId, T> const& rhs)
  {
    node |= ryml::MAP;

    for (auto const& [key, value] : rhs)
    {
      auto child = node.append_child();
      setKey(child, std::to_string(key.raw()));
      write(child, value);
    }
  }

  template<typename T>
  inline bool read(ryml::ConstNodeRef node, std::map<ListId, T>& rhs)
  {
    if (!node.is_map())
    {
      return false;
    }

    rhs.clear();

    for (auto const& child : node.children())
    {
      if (auto val = T{}; read(child, val))
      {
        auto const keyStr = keyView(child);
        std::uint32_t listIdVal = 0;
        auto const [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), listIdVal);

        if (ec == std::errc{})
        {
          rhs.emplace(ListId{listIdVal}, std::move(val));
        }
        else
        {
          APP_LOG_WARN(
            "ConfigTraits: Skipping invalid list ID key in map: {}", std::string_view{keyStr.data(), keyStr.size()});
        }
      }
    }

    return true;
  }

  template<PfrAggregate T>
  inline void write(ryml::NodeRef node, T const& obj)
  {
    node |= ryml::MAP;
    boost::pfr::for_each_field(obj,
                               [&node, index = std::size_t{0}](auto const& field) mutable
                               {
                                 constexpr auto kNames = boost::pfr::names_as_array<T>();
                                 auto child = node.append_child();
                                 setKey(child, kNames.at(index));
                                 write(child, field);
                                 ++index;
                               });
  }

  template<PfrAggregate T>
  inline bool read(ryml::ConstNodeRef node, T& obj)
  {
    if (!node.is_map())
    {
      return false;
    }

    bool success = true;
    boost::pfr::for_each_field(obj,
                               [&node, &success, index = std::size_t{0}](auto& field) mutable
                               {
                                 constexpr auto kNames = boost::pfr::names_as_array<T>();
                                 auto const key = kNames.at(index);

                                 if (auto const child = findChild(node, key); child.readable())
                                 {
                                   if (!read(child, field))
                                   {
                                     success = false;
                                   }
                                 }

                                 ++index;
                               });
    return success;
  }
} // namespace ao::rt::yaml
