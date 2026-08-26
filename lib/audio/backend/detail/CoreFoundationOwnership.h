// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <CoreFoundation/CFBase.h>

#include <memory>
#include <type_traits>

namespace ao::audio::backend::detail
{
  /** @brief Releases an owned Core Foundation reference. */
  template<typename Reference>
  struct CoreFoundationDeleter final
  {
    using pointer = Reference;

    void operator()(Reference const value) const noexcept
    {
      if (value != nullptr)
      {
        ::CFRelease(value);
      }
    }
  };

  /** @brief Unique ownership for a Core Foundation create/copy result. */
  template<typename Reference>
  using CoreFoundationPtr = std::unique_ptr<std::remove_pointer_t<Reference>, CoreFoundationDeleter<Reference>>;
} // namespace ao::audio::backend::detail
