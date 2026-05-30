// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::uimodel::layout
{
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
      if (auto const* ptr = getIf<T>(); ptr != nullptr)
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
            catch (std::invalid_argument const&)
            {
              return defaultValue;
            }
            catch (std::out_of_range const&)
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

    double asDouble(double defaultValue = 0.0) const;

    std::vector<std::string> asStringList() const;

    Value data{};
  };

  struct LayoutNode;

  struct BoxedLayoutNode final
  {
    std::unique_ptr<LayoutNode> nodePtr{};

    BoxedLayoutNode();
    explicit BoxedLayoutNode(LayoutNode value);

    BoxedLayoutNode(BoxedLayoutNode const& other);
    BoxedLayoutNode& operator=(BoxedLayoutNode const& other);

    BoxedLayoutNode(BoxedLayoutNode&&) noexcept;
    BoxedLayoutNode& operator=(BoxedLayoutNode&&) noexcept;

    ~BoxedLayoutNode();
  };

  struct LayoutNode final
  {
    template<typename T>
    T getProp(std::string_view key, T defaultValue) const
    {
      if (auto const it = props.find(key); it != props.end())
      {
        return it->second.as<T>(defaultValue);
      }

      return defaultValue;
    }

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
    std::optional<BoxedLayoutNode> optTooltip{};
  };
}
