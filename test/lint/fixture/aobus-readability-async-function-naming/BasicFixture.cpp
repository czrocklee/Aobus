// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Framework.inc"

using ao::async::Task;
using DirectAwaitable = boost::asio::awaitable<int>;

#define DECLARE_TASK(Name) Task<void> Name();

// POSITIVE
DECLARE_TASK(macroTask)

// NEGATIVE
DECLARE_TASK(macroTaskAsync)

// POSITIVE
Task<void> sleepFor()
{
  co_return;
}

// NEGATIVE
Task<void> sleepForAsync()
{
  co_return;
}

// POSITIVE
Task<int> makeReadyTask()
{
  co_return 1;
}

// NEGATIVE
Task<int> makeReadyTaskAsync()
{
  co_return 1;
}

// POSITIVE
Task<void> whenAll();

// NEGATIVE
Task<void> whenAllAsync();

// POSITIVE
DirectAwaitable loadAlias();

// NEGATIVE
DirectAwaitable loadAliasAsync();

// POSITIVE
Task<int> templateForward();

template<typename T>
// POSITIVE
Task<T> transform(T value);

template<typename T>
// NEGATIVE
Task<T> transformAsync(T value);

template<typename T>
// POSITIVE
auto makeTemplateTask(T value)
{
  return Task<T>{};
}

template<typename T>
// NEGATIVE
auto makeTemplateTaskAsync(T value)
{
  return Task<T>{};
}

template<typename T>
class TemplateTaskFactory
{
public:
  // POSITIVE
  auto create(T value) { return Task<T>{}; }

  // NEGATIVE
  auto createAsync(T value) { return Task<T>{}; }
};

void instantiateTaskTemplates()
{
  [[maybe_unused]] auto intTask = makeTemplateTask(1);
  [[maybe_unused]] auto doubleTask = makeTemplateTask(2.0);
  [[maybe_unused]] auto compliantTask = makeTemplateTaskAsync(1);
  [[maybe_unused]] auto intMemberTask = TemplateTaskFactory<int>{}.create(1);
  [[maybe_unused]] auto doubleMemberTask = TemplateTaskFactory<double>{}.create(2.0);
  [[maybe_unused]] auto compliantMemberTask = TemplateTaskFactory<int>{}.createAsync(1);
}

// POSITIVE
auto deducedForwarder()
{
  return makeReadyTaskAsync();
}

// NEGATIVE
auto deducedForwarderAsync()
{
  return makeReadyTaskAsync();
}

void consumeTask(Task<void> task)
{
  (void)task;
}

namespace other
{
  template<typename T>
  class Task
  {};
} // namespace other

// NEGATIVE
other::Task<int> launch();

using TaskHandle = int;

// NEGATIVE
TaskHandle launchTask();

// NEGATIVE
void startTask();

// POSITIVE
Task<void> on_save();

// POSITIVE
Task<void> property_state();

// NEGATIVE
auto taskLambda = []() -> Task<void> { co_return; };

class DerivedFramework : public AsyncFramework
{
public:
  // NEGATIVE
  Task<void> execute() override { co_return; }

  // POSITIVE
  Task<void> on_activate() { co_return; }

  // POSITIVE
  Task<void> refresh() { co_return; }
};

class TransitiveFrameworkOverride final : public DerivedFramework
{
public:
  // NEGATIVE
  Task<void> execute() override { co_return; }
};

class GtkBindingShape final
{
public:
  // POSITIVE
  Task<void> on_activate() { co_return; }
};
