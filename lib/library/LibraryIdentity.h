// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::library::detail
{
  // Stable, implementation-owned token used to prevent a transaction from one
  // MusicLibrary being applied to another library's database handles.
  class LibraryIdentity final
  {};
} // namespace ao::library::detail
