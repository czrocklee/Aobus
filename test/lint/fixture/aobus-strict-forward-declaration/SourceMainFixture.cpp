// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MacroExpansionContext.h"
#include "TemplateValueTypes.h"
#include "WeakOnlyTypes.h"

class CppNegativeTest
{
  // NEGATIVE A pointer spelled directly in this source file is not a candidate.
  TargetBadRawPtr* _ptr;
};

class CppMacroWeakTest
{
  // POSITIVE
  AOBUS_DECLARE_WEAK_MEMBER(TargetBadMacroPtr);
};

class CppMacroWithIncludedStrongUse
{
  // NEGATIVE The included header's by-value field requires the same provider.
  AOBUS_DECLARE_WEAK_MEMBER(CoResidentGraph);
};

class CppMacroWithLateStrongUse
{
  // NEGATIVE The later base specifier requires this type's definition.
  AOBUS_DECLARE_WEAK_MEMBER(PairValueType);
};

class LateStrongUse : public PairValueType
{};
