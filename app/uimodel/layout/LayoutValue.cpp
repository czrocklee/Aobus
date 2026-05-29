// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutNode.h>

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::uimodel::layout
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
    if (auto const* ptr = std::get_if<std::vector<std::string>>(&data); ptr != nullptr)
    {
      return *ptr;
    }

    if (auto const* ptr = std::get_if<std::string>(&data); ptr != nullptr && !ptr->empty())
    {
      return {*ptr};
    }

    return {};
  }
}
