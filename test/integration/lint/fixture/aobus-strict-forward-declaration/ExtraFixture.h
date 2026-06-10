// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StrongTypes.h"
#include "WeakOnlyTypes.h"

#include <memory>

class ExtraBadParam
{
  // POSITIVE: FIX-TO: void test(/* forward declare */ ExtraTargetA* a);
  void test(ExtraTargetA* a);
};

class ExtraGoodParam
{
  ExtraTargetB* b;

public:
  // NEGATIVE
  void test() { b->doSomething(); }
};

class ExtraGoodReturnVal
{
  // NEGATIVE
  ExtraTargetC test();
};
