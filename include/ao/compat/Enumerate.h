// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <concepts>
#include <iterator>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ao::compat::detail
{
  /**
   * @brief Portable stand-in for std::ranges::enumerate_view.
   *
   * libc++ does not implement P2164 (views::enumerate), so a macOS build cannot
   * name std::views::enumerate. This covers what the project asks of it:
   * single-pass forward traversal yielding (index, element) for range-for.
   *
   * It is deliberately not a common_range -- end() returns a sentinel rather
   * than a fabricated end iterator, so no bogus index is ever constructed.
   *
   * Const traversal and size() are provided on the same conditions the standard
   * view provides them. Neither is reached by the project's own call sites, all
   * of which enumerate a prvalue view in a range-for; they are here because the
   * alias resolves to enumerate_view elsewhere, and a call that compiles on
   * Linux failing to compile on macOS is the failure mode this seam exists to
   * prevent.
   *
   * Defined on every platform rather than only where it is selected, so the
   * implementation is compiled and unit tested everywhere instead of only on
   * the one platform that has no alternative.
   *
   * Retirement condition: doc/development/macos-portability.md.
   */
  template<std::ranges::input_range V>
    requires std::ranges::view<V>
  class PortableEnumerateView : public std::ranges::view_interface<PortableEnumerateView<V>>
  {
  public:
    template<bool Const>
    using Base = std::conditional_t<Const, V const, V>;

    /**
     * @brief Traversal position, parameterised on the constness of the view.
     *
     * enumerate_view templates its iterator the same way. The alternative --
     * one iterator built from iterator_t<V> -- cannot express begin() const,
     * because a const view yields iterator_t<V const>, which is a different
     * type whenever the underlying range distinguishes the two.
     */
    template<bool Const>
    class Iterator final
    {
    public:
      // The iterator protocol requires these standard spellings.
      using iterator_concept = std::forward_iterator_tag; // NOLINT(readability-identifier-naming)
      using iterator_category = std::input_iterator_tag;
      using difference_type = std::ranges::range_difference_t<Base<Const>>;
      using value_type = std::tuple<difference_type, std::ranges::range_value_t<Base<Const>>>;

      Iterator() = default;

      constexpr Iterator(std::ranges::iterator_t<Base<Const>> current, difference_type index)
        : _current{std::move(current)}, _index{index}
      {
      }

      constexpr auto operator*() const
      {
        return std::tuple<difference_type, std::ranges::range_reference_t<Base<Const>>>{_index, *_current};
      }

      constexpr Iterator& operator++()
      {
        ++_current;
        ++_index;
        return *this;
      }

      constexpr Iterator operator++(int) // NOLINT(aobus-modernize-use-std-numbers) -- required postfix signature
      {
        auto previous = *this;
        ++*this;
        return previous;
      }

      constexpr std::ranges::iterator_t<Base<Const>> const& base() const noexcept { return _current; }

      friend constexpr bool operator==(Iterator const& left, Iterator const& right)
      {
        return left._current == right._current;
      }

    private:
      std::ranges::iterator_t<Base<Const>> _current{};
      difference_type _index = 0;
    };

    template<bool Const>
    class Sentinel final
    {
    public:
      Sentinel() = default;

      constexpr explicit Sentinel(std::ranges::sentinel_t<Base<Const>> end)
        : _end{std::move(end)}
      {
      }

      friend constexpr bool operator==(Iterator<Const> const& iterator, Sentinel const& sentinel)
      {
        return iterator.base() == sentinel._end;
      }

    private:
      std::ranges::sentinel_t<Base<Const>> _end{};
    };

    PortableEnumerateView()
      requires std::default_initializable<V>
    = default;

    constexpr explicit PortableEnumerateView(V base)
      : _base{std::move(base)}
    {
    }

    constexpr Iterator<false> begin() { return Iterator<false>{std::ranges::begin(_base), 0}; }

    constexpr Iterator<true> begin() const
      requires std::ranges::range<V const>
    {
      return Iterator<true>{std::ranges::begin(_base), 0};
    }

    constexpr Sentinel<false> end() { return Sentinel<false>{std::ranges::end(_base)}; }

    constexpr Sentinel<true> end() const
      requires std::ranges::range<V const>
    {
      return Sentinel<true>{std::ranges::end(_base)};
    }

    // view_interface supplies size() only for a sized sentinel, which this
    // deliberately is not, so the count is forwarded from the base range
    // instead. enumerate_view is sized on exactly the same condition.
    constexpr auto size()
      requires std::ranges::sized_range<V>
    {
      return std::ranges::size(_base);
    }

    constexpr auto size() const
      requires std::ranges::sized_range<V const>
    {
      return std::ranges::size(_base);
    }

  private:
    V _base{};
  };

  template<typename R>
  PortableEnumerateView(R&&) -> PortableEnumerateView<std::views::all_t<R>>;

  /**
   * @brief Range adaptor object providing both enumerate(r) and r | enumerate.
   */
  struct EnumerateAdaptor final
  {
    template<std::ranges::viewable_range R>
    constexpr auto operator()(R&& range) const
    {
      return PortableEnumerateView{std::views::all(std::forward<R>(range))};
    }

    template<std::ranges::viewable_range R>
    friend constexpr auto operator|(R&& range, EnumerateAdaptor const& adaptor)
    {
      return adaptor(std::forward<R>(range));
    }
  };
} // namespace ao::compat::detail

namespace ao::compat::views
{
  /**
   * @brief Yields (index, element) pairs for a range.
   *
   * Resolves to std::views::enumerate where the standard library provides it
   * and to the portable adaptor above on libc++. Naming this instead of the
   * standard adaptor is what keeps the macOS build possible.
   */
#ifdef __cpp_lib_ranges_enumerate
  inline constexpr auto enumerate = std::views::enumerate; // NOLINT(readability-identifier-naming)
#else
  inline constexpr auto enumerate = detail::EnumerateAdaptor{}; // NOLINT(readability-identifier-naming)
#endif
} // namespace ao::compat::views
