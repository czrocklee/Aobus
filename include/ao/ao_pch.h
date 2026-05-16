// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

// Precompiled Header for stable, frequently-included headers
// This file should be included in all translation units for faster compilation

#pragma once

// Stable headers with high include counts
#include <ao/Type.h>             // NOLINT(misc-include-cleaner)
#include <ao/utility/ByteView.h> // NOLINT(misc-include-cleaner)

// Common std headers used everywhere
#include <array>       // NOLINT(misc-include-cleaner)
#include <cstddef>     // NOLINT(misc-include-cleaner)
#include <cstdint>     // NOLINT(misc-include-cleaner)
#include <functional>  // NOLINT(misc-include-cleaner)
#include <memory>      // NOLINT(misc-include-cleaner)
#include <optional>    // NOLINT(misc-include-cleaner)
#include <span>        // NOLINT(misc-include-cleaner)
#include <string_view> // NOLINT(misc-include-cleaner)
#include <vector>      // NOLINT(misc-include-cleaner)
