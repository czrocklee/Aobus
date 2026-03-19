// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

// Precompiled Header for stable, frequently-included headers
// This file should be included in all translation units for faster compilation

#pragma once

// Stable headers with high include counts
#include <rs/utility/ByteView.h>
#include <rs/core/Type.h>

// Common std headers used everywhere
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include <functional>
#include <memory>