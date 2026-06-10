// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// Comprehensive test for the co-resident type suppression.
// Tests the scenario where a header provides both forward-declarable types
// (classes used via pointer) and non-forward-declarable types (enums used
// by value). The checker must NOT suggest forward-declaring a class when
// its header is still needed for a co-resident enum.

#include "StrongTypes.h"
#include "WeakOnlyTypes.h"

#include <cstdint>

// ============================================================
// Scenario 1: Enum by-value field + class pointer (Graph.h pattern)
//
// Simulates QualityAnalyzer.h where:
//   - flow::NodeType (enum) is used by value → strong dep on StrongTypes.h
//   - flow::Graph (class) is used by pointer → weak ref, BUT same header
// ============================================================
class TestEnumFieldCoResident
{
  CoResidentNodeType _nodeType = CoResidentNodeType::Decoder;
  // NEGATIVE CoResidentGraph shares StrongTypes.h with CoResidentNodeType (enum by-value)
  CoResidentGraph* _graph;
};

// ============================================================
// Scenario 2: Enum constant reference + class pointer
//
// Even when the enum isn't a direct field type but its constants are
// referenced in an initializer or expression, the header is still needed.
// ============================================================
class TestEnumConstRefCoResident
{
  CoResidentNodeType _type = CoResidentNodeType::Sink;
  // NEGATIVE Same header provides the enum constant reference
  CoResidentNode* _node;
};

// ============================================================
// Scenario 3: Multiple co-resident pointers from the same header
//
// Both CoResidentGraph and CoResidentNode are in StrongTypes.h, which also
// has the CoResidentNodeType enum. Neither should trigger a warning.
// ============================================================
class TestMultiCoResident
{
  CoResidentNodeType _type = CoResidentNodeType::Decoder;
  // NEGATIVE Both pointers share the header with the enum
  CoResidentGraph* _graphPtr;
  // NEGATIVE
  CoResidentNode* _nodePtr;
};

// ============================================================
// Scenario 4: Class by-value + class pointer from the same header
//
// This tests the original (pre-enum) co-resident suppression still works.
// TargetGoodValueType is used by value (strong ref), and SharedHeaderUsed
// is used by pointer — both from StrongTypes.h.
// ============================================================
class TestClassByValueCoResident
{
  TargetGoodValueType _val;
  // NEGATIVE SharedHeaderUsed shares StrongTypes.h with TargetGoodValueType
  SharedHeaderUsed* _used;
};

// ============================================================
// Scenario 5: Pointer to a type from a header with NO strong deps → POSITIVE
//
// TargetBadRawPtr is in WeakOnlyTypes.h, which has no enums or by-value types.
// The checker should correctly suggest forward-declaring it.
// ============================================================
class TestIsolatedWeakRef
{
  // POSITIVE: FIX-TO: /* forward declare */ TargetBadRawPtr* _ptr;
  TargetBadRawPtr* _ptr;
};

// ============================================================
// Scenario 6: Mixed — one pointer to a co-resident header, one to an isolated header
// ============================================================
class TestMixedCoResident
{
  CoResidentNodeType _type = CoResidentNodeType::Decoder;
  // NEGATIVE co-resident with enum
  CoResidentGraph* _graph;
  // POSITIVE: FIX-TO: /* forward declare */ TargetBadRawPtr* _isolated;
  TargetBadRawPtr* _isolated;
};

// ============================================================
// Scenario 7: Enum as function parameter (by value) — strong dep
// ============================================================
class TestEnumParamCoResident
{
  void process(CoResidentNodeType type);
  // NEGATIVE function param creates a strong dep on StrongTypes.h
  CoResidentGraph* _graph;
};

// ============================================================
// Scenario 8: Enum as function return type — strong dep
// ============================================================
class TestEnumReturnCoResident
{
  CoResidentNodeType nodeType();
  // NEGATIVE return type creates a strong dep on StrongTypes.h
  CoResidentNode* _node;
};
