# SmartListEngine 实施计划

## 目标

把当前“每个 smart list 各自观察 source、各自遍历、各自读取 track、各自评估 expr”的模式，收口成一个集中式 `SmartListEngine`。

本计划的目标不是重写 parser / compiler，也不是做复合 AST，而是先解决下面这几个已经确定存在的热点：

1. 同一个 source list 被多个 smart list 重复遍历。
2. 同一个 `TrackId` 在多个 smart list 中被重复打开 LMDB 事务、重复读取 `TrackView`。
3. smart list 的 rebuild / incremental update 没有统一调度点，不利于后续 hierarchy。

最终效果应当是：

1. 每个 smart list 仍然保留自己的 `ExecutionPlan`。
2. 同一 source 下的多个 smart list 可以在一次遍历里得到全部 membership。
3. 单 track 更新时，只读一次 track，然后把结果分发到所有相关 smart list。
4. `FilteredTrackIdList` 继续保持 `TrackIdList` 接口，GTK 侧 `TrackListAdapter` 不需要感知 engine 细节。

## 非目标

本阶段明确不做下面这些事情：

1. 不改 `rs::expr::Parser`。
2. 不把多个 list 的 expr 融合成一个多输出 `ExecutionPlan`。
3. 不引入 hierarchy 语义。
4. 不修改 list 的持久化格式。

## 现状问题总结

当前实现的关键瓶颈主要集中在 `app/model/FilteredTrackIdList.cpp`：

1. `rebuild()` 对 source 中的每个 `TrackId` 都调用一次 `evaluate(id)`。
2. `evaluate(id)` 每次都会新建 `ReadTransaction`、新建 `TrackStore::Reader`，再 `get(id, loadMode)`。
3. `MainWindow` 为每个 smart list 都创建一个独立的 `FilteredTrackIdList`，因此上面的开销会按 list 数量线性放大。

这意味着目前最值得先优化的是“共享事务、共享 reader、共享 track load”，而不是先做 compiler fusion。

## 目标架构

### 核心思路

引入 `app/model/SmartListEngine`，作为所有 smart list 的集中调度器。

建议结构如下：

```cpp
class SmartListEngine final
{
public:
  using RegistrationId = std::uint64_t;

  explicit SmartListEngine(rs::core::MusicLibrary& ml);

  RegistrationId registerList(TrackIdList& source);
  void unregisterList(RegistrationId id);

  void setExpression(RegistrationId id, std::string expr);
  void rebuild(RegistrationId id);

  std::size_t size(RegistrationId id) const;
  TrackId trackIdAt(RegistrationId id, std::size_t index) const;
  std::optional<std::size_t> indexOf(RegistrationId id, TrackId trackId) const;

  bool hasError(RegistrationId id) const;
  std::string const& errorMessage(RegistrationId id) const;

private:
  struct SmartListState;
  struct SourceBucket;
};
```

### 设计原则

1. `ExecutionPlan` 仍然是“一个 list 一份 plan”。
2. `SmartListEngine` 负责 compile、batch rebuild、incremental update。
3. `FilteredTrackIdList` 退化成 facade，只负责对外暴露 `TrackIdList` 行为和 observer 通知。
4. engine 需要按 `source` 分桶，因为后续 hierarchy 会让 child list 的 source 不再总是 `_allTrackIds`。
5. engine 需要按 `accessProfile` 分组，避免所有 list 都被迫读取 cold data。

## 计划修改的文件

第一阶段建议修改或新增这些文件：

1. `app/model/SmartListEngine.h`
2. `app/model/SmartListEngine.cpp`
3. `app/model/FilteredTrackIdList.h`
4. `app/model/FilteredTrackIdList.cpp`
5. `app/MainWindow.h`
6. `app/MainWindow.cpp`
7. `app/NewListDialog.h`
8. `app/NewListDialog.cpp`
9. `CMakeLists.txt`
10. `test/unit/app/SmartListEngineTest.cpp`

如果实现里需要提炼帮助函数，也可以在 `app/model/` 下增加 1 个小型私有 helper，但不要先做公共抽象。

## Step 0：建立基线和约束

### 本步目标

确认现有 smart list 的接入点、重建路径和更新路径，避免改完后遗漏 preview 或存量 page。

### 要执行的命令

```bash
rg -n "FilteredTrackIdList|setExpression\(|reload\(|onInserted\(|onUpdated\(|onRemoved\(" app
```

### 完成标准

需要确认下面三个入口：

1. `MainWindow::buildPageForStoredList()` 的正式 smart list 创建路径。
2. `NewListDialog::setupPreview()/updatePreview()` 的预览路径。
3. `FilteredTrackIdList` 当前观察 source 并在 source 更新时自我重算的路径。

## Step 1：新增 `SmartListEngine`

### 本步目标

先把 engine 的状态模型和最小 API 放好，但先不改 UI 层。

### 建议放置位置

新增：

1. `app/model/SmartListEngine.h`
2. `app/model/SmartListEngine.cpp`

### 建议状态结构

```cpp
struct SmartListState
{
  RegistrationId id;
  TrackIdList* source = nullptr;
  std::string expression;

  bool hasError = false;
  std::string errorMessage;

  std::unique_ptr<rs::expr::ExecutionPlan> plan;
  rs::expr::PlanEvaluator evaluator;
  std::vector<TrackId> members;

  FilteredTrackIdList* owner = nullptr; // 非 owning，仅用于通知 facade
  bool dirty = true;
};

struct SourceBucket
{
  TrackIdList* source = nullptr;
  std::vector<RegistrationId> registrations;
  std::unique_ptr<TrackIdListObserver> observer;
};
```

### 关键实现点

1. engine 构造时持有 `MusicLibrary*`。
2. `registerList(source)` 时创建 `SmartListState`，按 `source` 加入 bucket。
3. 每个 bucket 只对 `source` attach 一次 observer，不要恢复成“每个 list attach 一次”。
4. `setExpression()` 里负责编译和错误状态落盘。
5. 对空表达式统一编译成 `true`，保持现有行为。

### 编译逻辑建议

```cpp
void SmartListEngine::compileState(SmartListState& state)
{
  try
  {
    auto expr = state.expression.empty() ? rs::expr::parse("true")
                                         : rs::expr::parse(state.expression);
    rs::expr::QueryCompiler compiler{&_ml->dictionary()};
    state.plan = std::make_unique<rs::expr::ExecutionPlan>(compiler.compile(expr));
    state.hasError = false;
    state.errorMessage.clear();
  }
  catch (std::exception const& e)
  {
    state.hasError = true;
    state.errorMessage = e.what();
    state.plan.reset();
    state.members.clear();
  }
}
```

## Step 2：把 `FilteredTrackIdList` 改成 facade

### 本步目标

保留现有 `TrackIdList` 接口和 observer 语义，但把 compile / evaluate / membership 状态迁移给 engine。

### 需要修改的文件

1. `app/model/FilteredTrackIdList.h`
2. `app/model/FilteredTrackIdList.cpp`

### 具体改法

把当前这些成员从 `FilteredTrackIdList` 中移除：

1. `_source`
2. `_filteredIds`
3. `_expression`
4. `_plan`
5. `_evaluator`
6. `_hasError`
7. `_errorMessage`

改成下面这种结构：

```cpp
class FilteredTrackIdList final : public TrackIdList
{
public:
  FilteredTrackIdList(TrackIdList& source,
                      rs::core::MusicLibrary& ml,
                      SmartListEngine& engine);
  ~FilteredTrackIdList() override;

  void setExpression(std::string expr);
  void reload();

  std::size_t size() const override;
  TrackId trackIdAt(std::size_t index) const override;
  std::optional<std::size_t> indexOf(TrackId id) const override;

  bool hasError() const;
  std::string const& errorMessage() const;

private:
  friend class SmartListEngine;
  void notifyEngineReset();
  void notifyEngineInserted(TrackId id, std::size_t index);
  void notifyEngineUpdated(TrackId id, std::size_t index);
  void notifyEngineRemoved(TrackId id, std::size_t index);

  SmartListEngine* _engine = nullptr;
  SmartListEngine::RegistrationId _registrationId = 0;
};
```

### 注意事项

1. facade 只转发 `size()/trackIdAt()/indexOf()` 到 engine。
2. observer 通知仍然从 facade 发出，因为 `TrackListAdapter` 已经依赖这套接口。
3. 不要让 GTK 层直接依赖 engine。

## Step 3：实现 batch rebuild

### 本步目标

同一 source 下多个 smart list 的 full rebuild 只做有限次遍历。

### 推荐算法

对于一个 `SourceBucket`：

1. 先筛出无错误、且已编译完成的 state。
2. 按 `plan->accessProfile` 分成两组：
   - `HotOnly`
   - `ColdOnly + HotAndCold`，统一按 `Both` 跑
3. 对每一组只创建一个 `ReadTransaction` 和一个 `TrackStore::Reader`。
4. 遍历 source 的 `TrackId` 一次。
5. 每个 `TrackId` 只 `reader.get(id, loadMode)` 一次。
6. 拿到 `TrackView` 后，依次把该 view 喂给组内所有 state 的 `PlanEvaluator`。
7. 命中就 append 到对应 `members` 暂存数组。
8. 扫描结束后统一 swap 到各 state，并对对应 facade 发 `notifyReset()`。

### 核心伪代码

```cpp
void SmartListEngine::rebuildBucket(SourceBucket& bucket)
{
  auto hotOnly = collectStates(bucket, rs::expr::AccessProfile::HotOnly);
  auto needsBoth = collectStates(bucket, rs::expr::AccessProfile::ColdOnly,
                                         rs::expr::AccessProfile::HotAndCold);

  rebuildGroup(bucket.source, hotOnly, rs::core::TrackStore::Reader::LoadMode::Hot);
  rebuildGroup(bucket.source, needsBoth, rs::core::TrackStore::Reader::LoadMode::Both);
}

void SmartListEngine::rebuildGroup(TrackIdList& source,
                                   std::span<SmartListState*> states,
                                   LoadMode mode)
{
  rs::lmdb::ReadTransaction txn(_ml->readTransaction());
  auto reader = _ml->tracks().reader(txn);

  std::vector<std::vector<TrackId>> nextMembers(states.size());

  for (std::size_t i = 0; i < source.size(); ++i)
  {
    auto id = source.trackIdAt(i);
    auto view = reader.get(id, mode);
    if (!view)
    {
      continue;
    }

    for (std::size_t stateIndex = 0; stateIndex < states.size(); ++stateIndex)
    {
      auto& state = *states[stateIndex];
      if (state.evaluator.matches(*state.plan, *view))
      {
        nextMembers[stateIndex].push_back(id);
      }
    }
  }

  applyRebuildResults(states, nextMembers);
}
```

### 为什么先按两组做

1. 逻辑简单，容易验证正确性。
2. 能先避免“只查 hot 字段的 list 被 cold list 拖慢”。
3. 后续如果 profile 还不够理想，再细化更多 batch 策略。

## Step 4：实现 source 增量更新分发

### 本步目标

把 source 的 `insert/update/remove/reset` 集中到 engine 里处理。

### 设计方式

engine 内部给每个 `SourceBucket` 配一个 observer：

```cpp
class SourceObserver final : public TrackIdListObserver
{
public:
  explicit SourceObserver(SmartListEngine& engine, TrackIdList& source);

  void onReset() override;
  void onInserted(TrackId id, std::size_t index) override;
  void onUpdated(TrackId id, std::size_t index) override;
  void onRemoved(TrackId id, std::size_t index) override;
};
```

### 各事件的处理规则

1. `onReset()`：直接对整个 bucket 跑 `rebuildBucket()`。
2. `onInserted(id)`：只读一次 track，然后对 bucket 内每个 state 算一次是否命中；命中则插入其 membership。
3. `onUpdated(id)`：只读一次 track，拿新结果和旧 membership 做 diff。
4. `onRemoved(id)`：不再读 track，直接在所有 state 中删掉该 `id`。

### 关键 diff 逻辑

```cpp
void SmartListEngine::handleUpdated(SmartListState& state, TrackId id, TrackView const& view)
{
  bool nowMatches = state.evaluator.matches(*state.plan, view);
  auto it = std::find(state.members.begin(), state.members.end(), id);
  bool wasPresent = it != state.members.end();

  if (nowMatches && !wasPresent) { ... notifyInserted ... }
  else if (!nowMatches && wasPresent) { ... notifyRemoved ... }
  else if (nowMatches && wasPresent) { ... notifyUpdated ... }
}
```

这里可以先保留 `std::find`，因为这一步的重点是把重复 LMDB load 消掉。只有当 profile 证明 membership 查找成热点时，再把 `members` 旁边补一个索引结构。

## Step 5：把 `MainWindow` 接到 engine 上

### 本步目标

正式页面中的 smart list 全部由共享 engine 驱动。

### 需要修改的文件

1. `app/MainWindow.h`
2. `app/MainWindow.cpp`

### 具体改法

1. 在 `MainWindow` 中新增一个长期存活的 `std::unique_ptr<app::model::SmartListEngine> _smartListEngine;`。
2. 当 `_musicLibrary` 可用且 `_allTrackIds` 已经建立后初始化 engine。
3. `buildPageForStoredList()` 里创建 smart list 时，把 engine 传进去。

### 目标代码形态

```cpp
if (view.isSmart())
{
  auto filtered = std::make_unique<app::model::FilteredTrackIdList>(
    *_allTrackIds,
    *_musicLibrary,
    *_smartListEngine);

  filtered->setExpression(std::string(view.filter()));
  filtered->reload();
  membershipList = std::move(filtered);
}
```

### 注意事项

1. engine 的生命周期必须长于所有 `FilteredTrackIdList`。
2. `clearTrackPages()` 前后要保证 smart list facade 先析构，再析构 engine 或切换库。

## Step 6：处理 `NewListDialog` 预览

### 本步目标

预览逻辑继续工作，但不要把对话框里的临时 preview 注册进主窗口的正式 engine 状态里。

### 推荐方案

让 `NewListDialog` 自己持有一个 preview 专用 engine：

```cpp
std::unique_ptr<app::model::SmartListEngine> _previewEngine;
```

对话框初始化时：

1. `_previewEngine = std::make_unique<app::model::SmartListEngine>(musicLibrary);`
2. `_previewFilteredList = std::make_unique<FilteredTrackIdList>(*_allTrackIds, *_musicLibrary, *_previewEngine);`

### 为什么不用共享主 engine

1. 避免对话框输入半成品 expression 污染正式状态。
2. 避免 dialog 关闭时还要处理 transient registration 清理。
3. 逻辑上更容易验证。

## Step 7：补测试

### 本步目标

至少加一个高价值回归测试，覆盖“单 source、多 smart list、一次 source 更新”的行为边界。

### 建议新增文件

1. `test/unit/app/SmartListEngineTest.cpp`

### 建议测试内容

1. 两个 smart list 共享同一个 source，full rebuild 后 membership 正确。
2. source 发 `onUpdated(id)` 时，两个 smart list 都能得到正确 diff。
3. 一个 `HotOnly`、一个 `HotAndCold`，都能拿到正确结果。
4. 非法 expression 不影响同 bucket 其他 smart list。

### 测试方式建议

不要硬测“reader.get 调了几次”，而是测行为：

1. membership 是否正确。
2. observer 通知次序是否正确。
3. 错误 list 是否被隔离。

如果确实需要验证共享加载次数，再在测试里引入一个很小的 fake source / fake reader，而不是把 LMDB 调用次数耦死到单元测试里。

## 验证步骤

### 代码搜索验证

```bash
rg -n "ReadTransaction txn\(_ml->readTransaction\(\)\)|_source.attach\(this\)|_source.detach\(this\)" app/model/FilteredTrackIdList.cpp
```

完成标准：

1. `FilteredTrackIdList.cpp` 中不再直接开 LMDB 事务。
2. `FilteredTrackIdList` 不再自己 attach/detach source。

### 构建验证

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

### 测试验证

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

如果只想先聚焦新测试，也可以单独跑：

```bash
nix-shell --run "/tmp/build/rs_test \"SmartListEngine\""
```

### 手工验证

建议最少做下面这几组：

1. 启动 `/tmp/build/RockStudio`，创建 3 个 smart list，其中 2 个只用 hot 字段，1 个用 `%custom`。
2. 校验 3 个 list 首次打开都能正常显示结果。
3. 导入一首会命中其中 2 个 list 的新歌，确认两个页面自动更新，另一个不受影响。
4. 修改一首歌的 tag / metadata，确认 membership 能正确进出并刷新页面。
5. 在新建 list 对话框中输入非法 expression，确认只影响 preview，不影响已有页面。

## 完成标准

当下面条件都满足时，说明本计划完成：

1. 正式 smart list 的 rebuild/update 全部经过 `SmartListEngine`。
2. `FilteredTrackIdList` 不再自己观察 source，也不再自己读取 track。
3. 同一 source 下多个 smart list 可以在 batch 中共享一次遍历。
4. GTK 页面和 preview 行为保持现有用户可见语义不变。
5. debug build 和测试通过。

## 本阶段结束后再做什么

本计划完成后，下一步就可以在这个 engine 之上叠加 hierarchy：

1. source 从“总是 `_allTrackIds`”扩展到“可以是另一个 list”。
2. engine 的 `SourceBucket` 直接变成 DAG 节点调度基础。
3. UI 只需要选择 parent/source，而不需要先重写 expr IR。
