// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// Types that should ONLY be used via pointer/reference in the test.
// This header intentionally contains NO types used by-value or as base classes,
// so the checker CAN suggest forward-declaring these.
#pragma once

class TargetBadUniquePtr
{};
class TargetBadSharedPtr
{};
class TargetBadRefPtr
{};
class TargetBadRawPtr
{};
class TargetBadRef
{};
class TargetBadMacroPtr
{};

class ExtraTargetA
{};
