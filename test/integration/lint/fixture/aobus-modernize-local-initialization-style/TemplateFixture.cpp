// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
  struct ExplicitRecord final
  {
    explicit constexpr ExplicitRecord(std::int32_t /*value*/) {}
  };

  enum class DependentMode : std::int32_t
  {
    One = 1,
  };
} // namespace

// Diagnose the source pattern, never a primitive type acquired by instantiation.
template<typename T>
struct Box final
{
  // NEGATIVE
  inline static auto value = T{1};

  // Nondependent primary-template primitives still report once.
  // POSITIVE
  inline static auto literal = 1;

  // POSITIVE
  inline static auto cast = std::int32_t{1};

  // NEGATIVE
  inline static auto product = std::size_t{8} * 1024;

  // NEGATIVE
  inline static auto staticCast = static_cast<std::int32_t>(1);
};

// Cover implicit and explicit instantiation without changing the source rules.
static_assert(sizeof(Box<std::int32_t>::value) == sizeof(std::int32_t));
template struct Box<double>;
template struct Box<ExplicitRecord>;

template<typename T>
// NEGATIVE
inline auto dependentVariable = T{1};

template<typename T>
// POSITIVE
inline auto literalVariable = 1;

template<typename T>
// POSITIVE
inline auto castVariable = std::int32_t{1};

static_assert(sizeof(dependentVariable<std::int32_t>) == sizeof(std::int32_t));
template double dependentVariable<double>;
template ExplicitRecord dependentVariable<ExplicitRecord>;
template int literalVariable<std::int32_t>;
template int literalVariable<ExplicitRecord>;
template std::int32_t castVariable<std::int32_t>;
template std::int32_t castVariable<ExplicitRecord>;

template<typename T>
void initializeTemplateStatics()
{
  // NEGATIVE
  static auto dependent = T{1};

  // POSITIVE
  static auto literal = 1;

  // POSITIVE
  static auto cast = std::int32_t{1};

  // NEGATIVE
  static auto product = std::size_t{8} * 1024;

  // NEGATIVE
  static auto staticCast = static_cast<std::int32_t>(1);

  // NEGATIVE
  auto localDependent = T{1};

  // POSITIVE
  auto localLiteral = 1;
}

template void initializeTemplateStatics<std::int32_t>();
template void initializeTemplateStatics<ExplicitRecord>();

void initializeImplicitTemplateStatics()
{
  initializeTemplateStatics<double>();
}

// The written arithmetic type is fixed even while its argument is dependent.
template<typename T>
void initializeFixedArithmetic(T input)
{
  // POSITIVE
  auto braced = std::int32_t{input};

  // POSITIVE
  auto parenthesized = std::int32_t(input);

  // POSITIVE
  auto wrapped = (std::int32_t(input));

  // POSITIVE
  static auto staticBraced = std::int32_t{input};

  // POSITIVE
  static auto staticParenthesized = std::int32_t(input);

  // NEGATIVE
  auto product = std::int32_t{input} * 1024;

  // NEGATIVE
  static auto staticProduct = std::int32_t(input) * 1024;

  // NEGATIVE
  auto staticCast = static_cast<std::int32_t>(input);

  // NEGATIVE
  static auto staticStaticCast = static_cast<std::int32_t>(input);
}

template void initializeFixedArithmetic<std::int32_t>(std::int32_t);
template void initializeFixedArithmetic<std::int16_t>(std::int16_t);

template<typename T>
struct FixedArithmetic final
{
  // POSITIVE
  inline static auto braced = std::int32_t{T{1}};

  // POSITIVE
  inline static auto parenthesized = std::int32_t(T{1});
};

template struct FixedArithmetic<std::int32_t>;
template struct FixedArithmetic<std::int16_t>;

template<typename T>
// POSITIVE
inline auto fixedDependentVariable = std::int32_t{T{1}};

template std::int32_t fixedDependentVariable<std::int32_t>;
template std::int32_t fixedDependentVariable<std::int16_t>;

// Injected class names prove a record category in both primary and partial templates.
template<typename T>
struct SourceRecord final
{
  T value;

  SourceRecord() = default;

  explicit SourceRecord(T input)
    : value{input}
  {
  }

  void initialize()
  {
    // POSITIVE
    SourceRecord empty{};

    // POSITIVE
    SourceRecord defaulted;
  }
};

template<typename T>
struct SourceRecord<T*> final
{
  T* value;

  SourceRecord() = default;

  explicit SourceRecord(T* input)
    : value{input}
  {
  }

  void initialize()
  {
    // POSITIVE
    SourceRecord empty{};

    // POSITIVE
    SourceRecord defaulted;
  }
};

template<typename T>
using AliasRecord = SourceRecord<T>;

template<typename T>
using ChainedAliasRecord = AliasRecord<T>;

template<typename T>
void initializeSourceKnownRecords(T input)
{
  // POSITIVE
  SourceRecord<T> empty{};

  // POSITIVE
  SourceRecord<T> braced{input};

  // POSITIVE
  SourceRecord<T> parenthesized(input);

  // POSITIVE
  SourceRecord<T> defaulted;

  // POSITIVE
  AliasRecord<T> aliased{};

  // POSITIVE
  ChainedAliasRecord<T> chainedAlias{};

  using LocalRecord = SourceRecord<T>;

  // POSITIVE
  LocalRecord localAlias{};

  // NEGATIVE
  auto deduced = SourceRecord<T>{input};

  // Non-primitive static initialization stays outside the local-only rule.
  // NEGATIVE
  static SourceRecord<T> staticRecord{};

  // Source-known containers retain the parentheses recommendation.
  // POSITIVE
  std::vector<T> values(3, input);

  // POSITIVE
  std::vector<T> listed{input};

  // Preserve the single-nullptr constructor exemption before resolution.
  // NEGATIVE
  std::unique_ptr<T> parenthesizedNull(nullptr);

  // NEGATIVE
  std::unique_ptr<T> bracedNull{nullptr};
}

template struct SourceRecord<std::int32_t>;
template struct SourceRecord<std::int32_t*>;
template void initializeSourceKnownRecords<std::int32_t>(std::int32_t);
template void initializeSourceKnownRecords<std::int32_t*>(std::int32_t*);

template<typename T>
using UnknownAlias = T;

// Unknown categories must not acquire primitive or record rules at instantiation.
template<typename T>
void initializeUnknownCategories()
{
  // NEGATIVE
  T explicitValue{1};

  // NEGATIVE
  auto deducedValue = T{1};

  // NEGATIVE
  UnknownAlias<T> aliasValue{1};

  // NEGATIVE
  auto deducedAlias = UnknownAlias<T>{1};

  // NEGATIVE
  static UnknownAlias<T> staticAlias{1};

  using LocalAlias = T;

  // NEGATIVE
  LocalAlias localAlias{1};
}

template void initializeUnknownCategories<std::int32_t>();
template void initializeUnknownCategories<ExplicitRecord>();
template void initializeUnknownCategories<DependentMode>();

template<typename T>
struct TypeOwner final
{
  using Type = T;
};

template<typename T>
void initializeDependentMembers()
{
  // NEGATIVE
  typename T::Type member{1};

  // NEGATIVE
  auto deducedMember = typename T::Type{1};

  using MemberAlias = typename T::Type;

  // NEGATIVE
  MemberAlias aliasedMember{1};
}

template void initializeDependentMembers<TypeOwner<std::int32_t>>();
template void initializeDependentMembers<TypeOwner<ExplicitRecord>>();
template void initializeDependentMembers<TypeOwner<DependentMode>>();

// A template-template parameter may name a class or an alias of any category.
template<template<typename> typename Category, typename T>
void initializeUnknownTemplateCategory()
{
  // NEGATIVE
  Category<T> explicitValue{};

  // NEGATIVE
  auto deducedValue = Category<T>{};
}

template void initializeUnknownTemplateCategory<SourceRecord, std::int32_t>();
template void initializeUnknownTemplateCategory<UnknownAlias, std::int32_t>();
template void initializeUnknownTemplateCategory<UnknownAlias, DependentMode>();
template void initializeUnknownTemplateCategory<UnknownAlias, SourceRecord<std::int32_t>>();
