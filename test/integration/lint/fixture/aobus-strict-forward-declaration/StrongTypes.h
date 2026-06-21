// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// Types used by-value or as base classes in the test — strong dependencies.
// The checker must never suggest forward-declaring these.
// Also co-locates SharedHeaderUsed, which tests the same-file suppression.
#pragma once

#include <cstdint>

class TargetGoodInheritance
{};
class TargetGoodValueType
{};
class TargetGoodInlineCall
{
public:
  void doSomething() {}
};
class TargetGoodByValParam
{};
class TargetGoodByValRet
{};
class SharedHeaderBase
{};
class SharedHeaderUsed
{};

class ExtraTargetB
{
public:
  void doSomething() {}
};
class ExtraTargetC
{};

// Co-resident types: an enum plus a class in the same header.
// Simulates the pattern in Graph.h (NodeType enum + Graph struct)
// and Types.h (Transport enum + PlaybackInput struct).
enum class CoResidentNodeType : std::uint8_t
{
  Decoder,
  Sink,
};

class CoResidentGraph
{};

class CoResidentNode
{};
