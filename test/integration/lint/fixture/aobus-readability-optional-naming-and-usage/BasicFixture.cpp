// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <optional>

#define OPTIONAL_HAS_VALUE(value) (value).has_value()
#define OPTIONAL_STATIC_CAST(value) static_cast<bool>(value)
#define PASSTHROUGH(value) (value)

class TestClass
{
  // POSITIVE
  std::optional<int> _invalidMemberName;

  // NEGATIVE
  std::optional<int> _optValidMemberName;

  // POSITIVE
  std::optional<int> _fieldWithOptInMiddle;
};

void testOptionalUsage()
{
  // POSITIVE
  [[maybe_unused]] auto noMarker = std::optional<int>{42};

  // NEGATIVE
  [[maybe_unused]] auto optValidName = std::optional<int>{42};

  // POSITIVE
  [[maybe_unused]] auto hasOptSuffix = std::optional<int>{42};
}

std::optional<int> maybeValue();

struct OptionalOwner
{
  std::optional<int> optMember;
};

OptionalOwner makeOwner();
void acceptBool(bool value);

void testHasValueUsage(OptionalOwner const& owner, std::optional<int> const& optParameter)
{
  auto optLocal = std::optional<int>{42};

  // POSITIVE
  if (optLocal.has_value())
  {
  }

  // POSITIVE
  if (optParameter.has_value())
  {
  }

  // POSITIVE
  if (owner.optMember.has_value())
  {
  }

  // NEGATIVE
  if (maybeValue().has_value())
  {
  }

  // NEGATIVE
  [[maybe_unused]] bool const hasTemporaryValue = std::optional<int>{42}.has_value();

  // NEGATIVE
  [[maybe_unused]] bool const hasLocalValue = optLocal.has_value();

  // NEGATIVE
  [[maybe_unused]] bool const hasMemberValue = owner.optMember.has_value();

  // NEGATIVE
  acceptBool(optParameter.has_value());

  // POSITIVE
  if (makeOwner().optMember.has_value())
  {
  }

  // NEGATIVE
  if (OPTIONAL_HAS_VALUE(optLocal))
  {
  }

  // POSITIVE
  if (PASSTHROUGH(optLocal.has_value()))
  {
  }

  // POSITIVE
  if (static_cast<bool>(optLocal))
  {
  }

  // POSITIVE
  [[maybe_unused]] bool const hasLocal = static_cast<bool>(optLocal);

  // POSITIVE
  [[maybe_unused]] bool const hasMember = static_cast<bool>(owner.optMember);

  // POSITIVE
  [[maybe_unused]] bool const hasTemporary = static_cast<bool>(maybeValue());

  // NEGATIVE
  if (optLocal)
  {
  }

  // NEGATIVE
  [[maybe_unused]] bool const hasTemporaryValueMaterialized = maybeValue().has_value();

  // NEGATIVE
  [[maybe_unused]] bool const hasNumber = static_cast<bool>(42);

  // NEGATIVE
  if (OPTIONAL_STATIC_CAST(optLocal))
  {
  }

  // POSITIVE
  if (PASSTHROUGH(static_cast<bool>(optLocal)))
  {
  }
}
