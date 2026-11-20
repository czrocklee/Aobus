// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "StrongTypes.h"

// Spell the declarator name in this header, not in the expansion's arguments.
#define AOBUS_DECLARE_WEAK_MEMBER(Type) Type* _borrowed

class IncludedStrongUse
{
  TargetGoodValueType _value;
};
