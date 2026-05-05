// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <gsl-lite/gsl-lite.hpp>
#include <memory>

namespace ao::utility
{
  /**
   * Helper to create a std::unique_ptr with a zero-overhead C-style deleter.
   * The deleter is defined locally to ensure minimum exposure of internal types.
   *
   * Example:
   *   auto pcm = makeUniquePtr<::snd_pcm_close>(ptr);
   */
  template<auto Fn, typename T>
  auto makeUniquePtr(T* p)
  {
    struct Deleter final
    {
      void operator()(T* ptr) const noexcept { Fn(ptr); }
    };

    return std::unique_ptr<T, Deleter>{p};
  }
} // namespace ao::utility
