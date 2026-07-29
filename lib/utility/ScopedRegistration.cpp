// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/ScopedRegistration.h>

#include <functional>
#include <utility>

namespace ao::utility
{
  ScopedRegistration::ScopedRegistration(std::move_only_function<void()> unregister)
    : _unregister{std::move(unregister)}
  {
  }

  ScopedRegistration::~ScopedRegistration()
  {
    reset();
  }

  ScopedRegistration::ScopedRegistration(ScopedRegistration&&) noexcept = default;

  ScopedRegistration& ScopedRegistration::operator=(ScopedRegistration&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      _unregister = std::move(other._unregister);
    }

    return *this;
  }

  void ScopedRegistration::reset()
  {
    if (_unregister)
    {
      auto unregister = std::move(_unregister);
      unregister();
    }
  }
} // namespace ao::utility
