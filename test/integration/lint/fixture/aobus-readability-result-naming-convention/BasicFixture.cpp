// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

namespace ao
{
  template<typename T = void>
  class Result
  {
  public:
    Result() = default;
  };
} // namespace ao

using ao::Result;
using IntResult = Result<int>;

template<typename T>
using GenericResult = Result<T>;

class TestClass
{
  // POSITIVE
  Result<int> _invalidMember;

  // NEGATIVE
  Result<int> _validMemberRes;

  // POSITIVE
  Result<int> _result;

  // NEGATIVE
  Result<int> _res;
};

struct TestStruct
{
  // NEGATIVE
  Result<int> res;

  // NEGATIVE
  Result<int> saveRes;
};

void acceptResult(
  // POSITIVE
  Result<int> badParam,
  // NEGATIVE
  Result<int> res,
  // NEGATIVE
  Result<int> goodParamRes)
{
  (void)badParam;
  (void)res;
  (void)goodParamRes;
}

template<typename T>
T passThrough(
  // NEGATIVE
  T value)
{
  return value;
}

template<typename T>
void inspectUninstantiatedResult(
  // POSITIVE
  Result<T> badParam,
  // NEGATIVE
  Result<T> inputRes)
{
  // POSITIVE
  [[maybe_unused]] Result<T> badLocal;

  // NEGATIVE
  [[maybe_unused]] GenericResult<T> localRes;

  (void)badParam;
  (void)inputRes;
}

template<typename T>
struct UninstantiatedResultState
{
  // POSITIVE
  Result<T> badField;

  // NEGATIVE
  GenericResult<T> stateRes;
};

template<typename T>
void inspectInstantiatedResult(
  // POSITIVE
  GenericResult<T> invalidParam,
  // NEGATIVE
  GenericResult<T> parsedRes)
{
  // POSITIVE
  [[maybe_unused]] GenericResult<T> invalidLocal;

  // NEGATIVE
  [[maybe_unused]] Result<T> parsedLocalRes;

  (void)invalidParam;
  (void)parsedRes;
}

template<typename T>
struct InstantiatedResultState
{
  // POSITIVE
  GenericResult<T> invalidField;

  // NEGATIVE
  Result<T> fieldRes;
};

void instantiateResultTemplates()
{
  inspectInstantiatedResult(Result<int>{}, Result<int>{});
  inspectInstantiatedResult(Result<double>{}, Result<double>{});
  [[maybe_unused]] auto state = InstantiatedResultState<int>{};
  [[maybe_unused]] auto otherState = InstantiatedResultState<double>{};
}

void inspectReferences(
  // POSITIVE
  IntResult& invalidReference,
  // POSITIVE
  Result<int> const& invalidConstReference,
  // POSITIVE
  Result<int>&& invalidRvalueReference,
  // NEGATIVE
  IntResult& res,
  // NEGATIVE
  Result<int> const& inputRes,
  // NEGATIVE
  Result<int>&& movedRes)
{
  // POSITIVE
  [[maybe_unused]] auto& invalidLocalReference = invalidReference;
  // POSITIVE
  [[maybe_unused]] auto const& invalidConstLocal = invalidConstReference;
  // NEGATIVE
  [[maybe_unused]] auto&& borrowedRes = movedRes;
}

struct ResultReferences
{
  // POSITIVE
  Result<int>& invalidFieldReference;
  // NEGATIVE
  Result<int> const& fieldRes;
};

template<typename T>
void inspectDependentReferences(
  // POSITIVE
  GenericResult<T> const& invalidDependentReference,
  // POSITIVE
  Result<T>&& invalidDependentRvalue,
  // NEGATIVE
  Result<T> const& inputRes)
{
  // POSITIVE
  [[maybe_unused]] auto invalidDependentConstruction = Result<T>{};
  // NEGATIVE
  [[maybe_unused]] auto constructedRes = GenericResult<T>{};
}

template<typename T>
Result<T> readValue(T)
{
  return {};
}

template<typename T>
struct GenericState
{
  // NEGATIVE
  T value;
};

template<typename T>
void inspectDeducedResults(
  // NEGATIVE
  T value)
{
  // POSITIVE
  [[maybe_unused]] auto invalidDeducedLocal = readValue(value);
  // POSITIVE
  [[maybe_unused]] auto const& invalidDeducedReference = readValue(value);
  // NEGATIVE
  [[maybe_unused]] auto readRes = readValue(value);
  // NEGATIVE
  [[maybe_unused]] T genericLocal = value;
  // NEGATIVE
  [[maybe_unused]] auto genericCopy = value;
  // NEGATIVE
  [[maybe_unused]] auto genericForwarded = passThrough(value);

  auto callback = [](auto input)
  {
    // POSITIVE
    [[maybe_unused]] auto invalidNestedCallbackLocal = readValue(input);
    // NEGATIVE
    [[maybe_unused]] auto genericCallbackCopy = input;
  };
  callback(value);
}

template<typename Callable>
void invokeGenericCallable(Callable callable)
{
  // NEGATIVE
  [[maybe_unused]] auto genericCall = callable();
}

void instantiateDeducedResults()
{
  inspectDeducedResults(1);
  inspectDeducedResults(2.0);
  inspectDeducedResults(Result<int>{});
  invokeGenericCallable([] { return Result<int>{}; });
  invokeGenericCallable([] { return 1; });
  [[maybe_unused]] auto state = GenericState<Result<int>>{};
  inspectDependentReferences(Result<int>{}, Result<int>{}, Result<int>{});
  inspectDependentReferences(Result<double>{}, Result<double>{}, Result<double>{});

  auto callback = [](
                    // NEGATIVE
                    auto value,
                    // POSITIVE
                    Result<int> const& invalidCallbackReference)
  {
    // POSITIVE
    [[maybe_unused]] auto invalidCallbackLocal = readValue(value);
    // NEGATIVE
    [[maybe_unused]] auto callbackRes = readValue(value);
  };
  callback(1, Result<int>{});
  callback(2.0, Result<int>{});

  auto genericCallback = [](
                           // NEGATIVE
                           auto value) { return value; };
  [[maybe_unused]] auto callbackRes = genericCallback(Result<int>{});

  auto concreteCallback = [](
                            // POSITIVE
                            Result<int> invalidConcreteParameter)
  {
    // POSITIVE
    [[maybe_unused]] auto invalidConcreteLocal = invalidConcreteParameter;
  };
  concreteCallback(Result<int>{});
}

void testResultNaming()
{
  // POSITIVE
  [[maybe_unused]] auto invalidLocal = Result<int>{};

  // NEGATIVE
  [[maybe_unused]] auto validLocalRes = Result<int>{};

  // NEGATIVE
  [[maybe_unused]] Result<void> res;

  // POSITIVE
  [[maybe_unused]] Result<void> result;

  // NEGATIVE
  [[maybe_unused]] Result<void> openRes;

  // POSITIVE
  [[maybe_unused]] Result<void> features;

  // POSITIVE
  [[maybe_unused]] Result<void> invalidValue;

  // NEGATIVE
  [[maybe_unused]] IntResult aliasRes;

  // POSITIVE
  [[maybe_unused]] auto aliasValue = IntResult{};

  [[maybe_unused]] auto const genericResultRes = passThrough(Result<int>{});
}
