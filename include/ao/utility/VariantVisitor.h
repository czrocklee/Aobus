// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <type_traits>
#include <utility>

namespace ao::utility
{
  namespace detail
  {
    template<typename... Ts>
    struct Overload : Ts...
    {
      using Ts::operator()...;
    };

    template<typename... Ts>
    Overload(Ts...) -> Overload<Ts...>;
  } // namespace detail

  template<typename... Ts>
    requires(std::is_class_v<std::remove_cvref_t<Ts>> && ...)
  auto makeVisitor(Ts&&... ts)
  {
    return detail::Overload{std::forward<Ts>(ts)...};
  }
} // namespace ao::utility
