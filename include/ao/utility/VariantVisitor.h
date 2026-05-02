#pragma once

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
  }

  template<typename... Ts>
  auto makeVisitor(Ts&&... ts)
  {
    return detail::Overload{std::forward<Ts>(ts)...};
  }
}
