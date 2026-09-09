// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <type_traits>

using Boolean = bool;

// POSITIVE
Boolean aliasedValue();
// POSITIVE
auto trailingValue() -> Boolean;
// POSITIVE
auto concreteDeclared();
// NEGATIVE
auto concreteDeclared()
{
  return true;
}

// NEGATIVE
bool& referenceValue();
// NEGATIVE
bool const& constReferenceValue();

template<typename T>
// POSITIVE
bool explicitValue(T);

template<typename T>
// POSITIVE
auto trailingTemplate(T) -> Boolean;

template<typename T>
// POSITIVE
Boolean aliasedTemplate(T);

template<typename T>
// POSITIVE
auto uninstantiatedValue(T)
{
  return true;
}

template<typename T>
// NEGATIVE
T extract(T value)
{
  return value;
}

template<typename T>
// NEGATIVE
auto copyIntFirst(T value)
{
  return value;
}

template<typename T>
// NEGATIVE
auto copyBoolOnly(T value)
{
  return value;
}

template<typename Callable>
// NEGATIVE
auto invokeValue(Callable callable)
{
  return callable();
}

template<typename T>
// NEGATIVE
typename T::Value payload(T value)
{
  return value.value;
}

template<typename T>
// NEGATIVE
auto mixedValue(T value)
{
  if constexpr (std::is_same_v<T, bool>)
  {
    return true;
  }
  else
  {
    return value;
  }
}

template<typename T>
// POSITIVE
auto convertedValue(T value)
{
  return static_cast<bool>(value);
}

template<typename T>
// POSITIVE
auto allBranches(T value)
{
  if constexpr (sizeof(T) > 1)
  {
    return static_cast<bool>(value);
  }
  else
  {
    return false;
  }
}

template<typename T>
bool isKnown(T);

template<typename T>
// POSITIVE
auto knownCall(T value)
{
  return ::isKnown(value);
}

template<typename T>
// NEGATIVE
auto genericCall(T value)
{
  return ::extract(value);
}

template<typename T>
// NEGATIVE
auto comparisonValue(T value)
{
  return value == value;
}

template<typename T>
// POSITIVE
auto declaredValue(T value);

template<typename T>
// NEGATIVE
auto declaredValue(T value)
{
  // These returns belong to other functions, not this source contract.
  [[maybe_unused]] auto lambda = [] { return 1; };
  struct Local
  {
    int value() { return 1; }
  };
  return static_cast<bool>(value);
}

template<typename T>
// NEGATIVE
auto nestedPredicateOnly(T value)
{
  [[maybe_unused]] auto lambda = [] { return true; };
  return value;
}

template<typename T>
// NEGATIVE
auto& referenceTemplate(T& value)
{
  return value;
}

template<typename T>
struct Payload
{
  using Value = T;
  T value;

  // NEGATIVE
  T load() const { return value; }

  // NEGATIVE
  auto copy() const { return value; }

  // POSITIVE
  bool evaluate() const { return true; }

  // POSITIVE
  auto converted() const { return static_cast<bool>(value); }
};

template<typename T>
// NEGATIVE
T specializedValue(T value)
{
  return value;
}

template<>
// POSITIVE
bool specializedValue<bool>(bool value)
{
  return value;
}

template<typename T>
struct DeclaredMember
{
  // POSITIVE
  auto evaluate(T value);
};

template<typename T>
// NEGATIVE
auto DeclaredMember<T>::evaluate(T value)
{
  return static_cast<bool>(value);
}

template<typename T>
struct SpecializedMember
{
  // NEGATIVE
  T load() const { return {}; }
};

template<>
// POSITIVE
bool SpecializedMember<bool>::load() const
{
  return true;
}

void instantiateSourceContracts()
{
  [[maybe_unused]] auto firstBool = extract(true);
  [[maybe_unused]] auto thenInt = extract(1);
  [[maybe_unused]] auto firstInt = copyIntFirst(1);
  [[maybe_unused]] auto thenBool = copyIntFirst(true);
  [[maybe_unused]] auto onlyBool = copyBoolOnly(true);
  [[maybe_unused]] auto callableBool = invokeValue([] { return true; });
  [[maybe_unused]] auto callableInt = invokeValue([] { return 1; });
  [[maybe_unused]] auto boolPayload = payload(Payload<bool>{true});
  [[maybe_unused]] auto intPayload = payload(Payload<int>{1});
  [[maybe_unused]] auto mixedBool = mixedValue(true);
  [[maybe_unused]] auto mixedInt = mixedValue(1);
  [[maybe_unused]] auto conversion = convertedValue(1);
  [[maybe_unused]] auto wide = allBranches(1);
  [[maybe_unused]] auto narrow = allBranches(true);
  [[maybe_unused]] auto known = knownCall(1);
  [[maybe_unused]] auto generic = genericCall(true);
  [[maybe_unused]] auto comparison = comparisonValue(true);
  [[maybe_unused]] auto declaredBool = declaredValue(true);
  [[maybe_unused]] auto declaredInt = declaredValue(1);
  [[maybe_unused]] auto nested = nestedPredicateOnly(true);
  [[maybe_unused]] auto loadedBool = Payload<bool>{true}.load();
  [[maybe_unused]] auto loadedInt = Payload<int>{1}.load();
  [[maybe_unused]] auto copiedBool = Payload<bool>{true}.copy();
  [[maybe_unused]] auto copiedInt = Payload<int>{1}.copy();
  [[maybe_unused]] auto evaluatedBool = Payload<bool>{true}.evaluate();
  [[maybe_unused]] auto evaluatedInt = Payload<int>{1}.evaluate();
  [[maybe_unused]] auto convertedBool = Payload<bool>{true}.converted();
  [[maybe_unused]] auto convertedInt = Payload<int>{1}.converted();
  [[maybe_unused]] auto specialized = specializedValue(true);
  [[maybe_unused]] auto primary = specializedValue(1);
  [[maybe_unused]] auto declaredMemberBool = DeclaredMember<bool>{}.evaluate(true);
  [[maybe_unused]] auto declaredMemberInt = DeclaredMember<int>{}.evaluate(1);
  [[maybe_unused]] auto memberSpecialized = SpecializedMember<bool>{}.load();
  [[maybe_unused]] auto memberPrimary = SpecializedMember<int>{}.load();
  bool value = false;
  [[maybe_unused]] bool& reference = referenceTemplate(value);
}

// NEGATIVE
bool IsReady();
// NEGATIVE
bool HasItems();
// NEGATIVE
bool CanOpen();
// NEGATIVE
bool ShouldRetry();
// NEGATIVE
bool SupportsSeeking();
// NEGATIVE
bool NeedsRefresh();
// NEGATIVE
bool MatchesTrack();
// NEGATIVE
bool AcceptsValue();
// NEGATIVE
bool CoversRange();
// NEGATIVE
bool covers();
// NEGATIVE
bool coversRevision();
// NEGATIVE
bool TryOpen();
// NEGATIVE
bool ContainsValue();
// NEGATIVE
bool StartsWithText();
// NEGATIVE
bool EndsWithText();
// NEGATIVE
bool asBool();
// NEGATIVE
bool readBoolOr();
// POSITIVE
bool asBoolValue();
// POSITIVE
bool readBoolOrDefault();
// POSITIVE
bool Island();
// POSITIVE
bool Isready();
// POSITIVE
bool HASItems();
// POSITIVE
bool coverslip();
