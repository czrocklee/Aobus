// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

class SomeCppTarget
{};

// This is a .cpp file, so the strict-forward-declaration checker
// should NOT emit any warnings for this weak reference.
class CppNegativeTest
{
  SomeCppTarget* ptr;
};
