// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "GtkSessionBusTestSupport.h"

#include <string_view>

namespace ao::gtk::test
{
  bool isOwnedGtkSessionBus(char const* const address, char const* const ownershipAddress) noexcept
  {
    return address != nullptr && ownershipAddress != nullptr && std::string_view{address}.starts_with("unix:") &&
           std::string_view{address}.size() > 5 && std::string_view{address} == ownershipAddress;
  }
} // namespace ao::gtk::test
