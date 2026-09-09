// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// POSITIVE
static int const staticValue = 1;

// NEGATIVE
static int const kStaticValue = 1;

// POSITIVE
constexpr int constantValue = 1;

// NEGATIVE
constexpr int kConstantValue = 1;

class Constants final
{
public:
  // POSITIVE
  static int const classValue = 1;

  // NEGATIVE
  static int const kClassValue = 1;
};

template<typename T>
// POSITIVE
concept lowerConcept = true;

template<typename T>
// NEGATIVE
concept UpperConcept = true;

// Semantic predicate vocabulary does not relax project-owned camelCase.
// POSITIVE
bool IsReady();
// POSITIVE
bool HasItems();
// NEGATIVE
bool isReady();
// NEGATIVE
bool hasItems();

void useLocalConstants()
{
  // NEGATIVE
  int const localValue = 1;
  // POSITIVE
  static int const staticLocalValue = 1;
  // NEGATIVE
  static int const kStaticLocalValue = 1;
  // POSITIVE
  constexpr int localConstant = 1;
  // NEGATIVE
  constexpr int kLocalConstant = 1;
  (void)localValue;
  (void)staticLocalValue;
  (void)kStaticLocalValue;
  (void)localConstant;
  (void)kLocalConstant;
}
