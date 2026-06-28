// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <charconv>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
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
          double parsed = 0.0;
          auto const* const begin = val.data();
          auto const* const end = val.data() + val.size();
          auto const [ptr, ec] = std::from_chars(begin, end, parsed);

          if (ec == std::errc{} && ptr == end)
          {
            return parsed;
          }

          return defaultValue;
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

  BoxedLayoutNode::BoxedLayoutNode() = default;

  BoxedLayoutNode::BoxedLayoutNode(LayoutNode value)
    : nodePtr{std::make_unique<LayoutNode>(std::move(value))}
  {
  }

  BoxedLayoutNode::BoxedLayoutNode(BoxedLayoutNode const& other)
    : nodePtr{other.nodePtr ? std::make_unique<LayoutNode>(*other.nodePtr) : nullptr}
  {
  }

  BoxedLayoutNode& BoxedLayoutNode::operator=(BoxedLayoutNode const& other)
  {
    if (this != &other)
    {
      nodePtr = other.nodePtr ? std::make_unique<LayoutNode>(*other.nodePtr) : nullptr;
    }

    return *this;
  }

  BoxedLayoutNode::BoxedLayoutNode(BoxedLayoutNode&&) noexcept = default;
  BoxedLayoutNode& BoxedLayoutNode::operator=(BoxedLayoutNode&&) noexcept = default;

  BoxedLayoutNode::~BoxedLayoutNode() = default;
} // namespace ao::uimodel
