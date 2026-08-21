// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <span>

namespace ao::i18n::detail
{
  std::span<std::byte const> embeddedCatalogData() noexcept;
} // namespace ao::i18n::detail
