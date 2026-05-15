// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::gtk::layout
{
  /**
   * @brief A simple variant to hold property values in a LayoutNode.
   */
  struct LayoutValue final
  {
    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>>;

    LayoutValue() = default;

    template<typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, LayoutValue>)
    explicit LayoutValue(T&& value)
      : data{std::forward<T>(value)}
    {
    }

    template<typename T>
    T const* getIf() const
    {
      return std::get_if<T>(&data);
    }

    template<typename T>
    T as(T defaultValue) const
    {
      if (auto const* ptr = getIf<T>())
      {
        return *ptr;
      }

      return defaultValue;
    }

    std::string asString(std::string const& defaultValue = "") const
    {
      return std::visit(
        [&defaultValue](auto const& val) -> std::string
        {
          using T = std::decay_t<decltype(val)>;

          if constexpr (std::is_same_v<T, std::string>)
          {
            return val;
          }
          else if constexpr (std::is_same_v<T, bool>)
          {
            return val ? "true" : "false";
          }
          else if constexpr (std::is_arithmetic_v<T>)
          {
            return std::format("{}", val);
          }
          else
          {
            return defaultValue;
          }
        },
        data);
    }

    std::int64_t asInt(std::int64_t defaultValue = 0) const
    {
      return std::visit(
        [defaultValue](auto const& val) -> std::int64_t
        {
          using T = std::decay_t<decltype(val)>;

          if constexpr (std::is_arithmetic_v<T>)
          {
            return static_cast<std::int64_t>(val);
          }
          else if constexpr (std::is_same_v<T, std::string>)
          {
            try
            {
              return std::stoll(val);
            }
            catch (...)
            {
              return defaultValue;
            }
          }
          else
          {
            return defaultValue;
          }
        },
        data);
    }

    bool asBool(bool defaultValue = false) const
    {
      return std::visit(
        [defaultValue](auto const& val) -> bool
        {
          using T = std::decay_t<decltype(val)>;

          if constexpr (std::is_same_v<T, bool>)
          {
            return val;
          }
          else if constexpr (std::is_same_v<T, std::string>)
          {
            if (val == "true")
            {
              return true;
            }

            if (val == "false")
            {
              return false;
            }

            return defaultValue;
          }
          else if constexpr (std::is_arithmetic_v<T>)
          {
            return static_cast<bool>(val);
          }
          else
          {
            return defaultValue;
          }
        },
        data);
    }

    double asDouble(double defaultValue = 0.0) const
    {
      return std::visit(
        [defaultValue](auto const& val) -> double
        {
          using T = std::decay_t<decltype(val)>;

          if constexpr (std::is_arithmetic_v<T>)
          {
            return static_cast<double>(val);
          }
          else if constexpr (std::is_same_v<T, std::string>)
          {
            try
            {
              return std::stod(val);
            }
            catch (...)
            {
              return defaultValue;
            }
          }
          else
          {
            return defaultValue;
          }
        },
        data);
    }

    Value data{};
  };

  /**
   * @brief A node in the layout tree.
   */
  struct LayoutNode final
  {
    /**
     * @brief Helper to get a property value with a fallback.
     */
    template<typename T>
    T getProp(std::string_view key, T defaultValue) const
    {
      if (auto const it = props.find(key); it != props.end())
      {
        return it->second.as<T>(defaultValue);
      }

      return defaultValue;
    }

    /**
     * @brief Helper to get a layout property value with a fallback.
     */
    template<typename T>
    T getLayout(std::string_view key, T defaultValue) const
    {
      if (auto const it = layout.find(key); it != layout.end())
      {
        return it->second.as<T>(defaultValue);
      }

      return defaultValue;
    }

    std::string id{};
    std::string type{};
    std::map<std::string, LayoutValue, std::less<>> props{};
    std::map<std::string, LayoutValue, std::less<>> layout{};
    std::vector<LayoutNode> children{};
  };
}
