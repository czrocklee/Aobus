// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WeakOnlyTypes.h"

// Open a system namespace without making the parameter below system-spelled.
#include "SystemNamespace.inc"

// clang-format off: the namespace opener is in the included fragment.
  using Callback = void(::TargetBadRawPtr* target); // POSITIVE
} // namespace system_context
// clang-format on
