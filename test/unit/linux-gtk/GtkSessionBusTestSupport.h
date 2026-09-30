// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::gtk::test
{
  // Admits a session bus only when it is a Unix endpoint the portal marked as
  // owned. Pure string comparison; no D-Bus or GTK contact. See
  // GtkApplicationTestSupport.h for the skip-on-miss wrapper.
  bool isOwnedGtkSessionBus(char const* address, char const* ownershipAddress) noexcept;
} // namespace ao::gtk::test
