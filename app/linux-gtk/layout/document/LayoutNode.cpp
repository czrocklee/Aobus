// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/document/LayoutNode.h"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::gtk::layout
{
  double LayoutValue::asDouble(double defaultValue) const
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

  std::vector<std::string> LayoutValue::asStringList() const
  {
    if (auto const* ptr = std::get_if<std::vector<std::string>>(&data))
    {
      return *ptr;
    }

    if (auto const* ptr = std::get_if<std::string>(&data); ptr != nullptr && !ptr->empty())
    {
      return {*ptr};
    }

    return {};
  }
} // namespace ao::gtk::layout
