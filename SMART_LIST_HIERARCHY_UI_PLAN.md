# Smart List Hierarchy And UI Plan

## 目标

在 `SMART_LIST_ENGINE_PLAN.md` 完成后，给 smart list 增加父/子层级，并让 UI 围绕“parent membership + local expression”工作。

## 核心语义

1. 每个 list 持久化一个 `sourceListId`。
2. `ListId{0}` 表示虚拟根 `All Tracks`。
3. `filter` 只存当前 list 的 local expression。
4. 运行时语义等价于 `effective(child) = effective(parent) AND local(child)`。
5. 实际评估优先复用 parent membership，不依赖把整条 effective expression 重新拼接后再全量计算。
6. list graph 必须是 DAG，禁止循环依赖。
7. 当前不需要兼容旧 payload，整个工程直接切到新的单一 layout。

用于 UI 展示和日志时，可以这样拼 effective expression：

```cpp
std::string composeEffectiveExpression(std::string_view parent,
                                      std::string_view local)
{
  if (parent.empty())
  {
    return std::string{local};
  }
  if (local.empty())
  {
    return std::string{parent};
  }
  return std::string{"("} + std::string{parent} + ") and (" + std::string{local} + ")";
}
```

这个 helper 只用于展示，不作为 runtime 的唯一语义来源。

## 非目标

1. 不做 tree collapse / expand。
2. 不做 drag-and-drop 改 parent。
3. 不做 sibling combine、union、intersection。
4. 不把 hierarchy 直接做进 parser 语言。

## 实现步骤

### 1. Core schema

目标：让 list payload 直接表达 source。

1. 直接扩展 `ListHeader`，增加 `sourceListId`。
2. `ListView` 增加 `sourceListId()` 和 `isRootSource()`。
3. `ListRecord` 增加 `ListId sourceListId = ListId{0};`。
4. `ListBuilder` 增加 `.sourceListId(ListId)`，并让 `fromView()` / `fromRecord()` / `record()` / `serialize()` 全部 round-trip 这个字段。
5. 更新 `kListHeaderSize`、注释和相关测试。

建议结构：

```cpp
struct ListHeader final
{
  std::uint32_t trackIdsCount;
  std::uint16_t nameOffset;
  std::uint16_t nameLen;
  std::uint16_t descOffset;
  std::uint16_t descLen;
  std::uint16_t filterOffset;
  std::uint16_t filterLen;
  std::uint32_t sourceListId;
};
```

### 2. 创建流程

目标：让 app 创建 list 时能指定 parent/source。

1. `app/model/ListDraft.h` 增加 `sourceListId`。
2. `MainWindow` 创建 list 时把 `draft.sourceListId` 传给 `ListBuilder`。
3. smart list 的 `expression` 继续表示 local expression，不改语义。

### 3. SmartListEngine

目标：让 engine 能按 DAG 调度。

1. 为每个 smart list 记录 `listId`、`sourceListId`、`children`。
2. `sourceListId == 0` 的 smart list 作为根层。
3. rebuild 时按拓扑顺序计算：先 parent，再 child。
4. child 的 source 直接使用 parent membership。
5. parent membership 变化时，只重建 descendants。
6. 创建和注册时都做 cycle check。

### 4. NewListDialog

目标：让用户清楚看到 source、inherited expression、local expression、effective expression。

1. `Expression` 文案改成 `Local Expression`。
2. 新增 `Source` 选择器。
3. 新增只读的 `Inherited Expression` 和 `Effective Expression`。
4. `draft()` 返回 `sourceListId`。
5. preview 语义改为：先取 source list 的 membership，再用 local expression 过滤。
6. source 不存在、表达式非法、形成环时都阻止提交。

### 5. MainWindow 和 sidebar

目标：以最小成本展示 hierarchy。

1. 继续使用平面 list model。
2. 按前序遍历输出 sidebar 顺序。
3. `ListRow` 增加 `sourceListId` 和 `depth`。
4. 用 `depth` 缩进显示 child list。
5. 增加 `New Child List` 入口，默认把当前 list 设为 source。
6. 删除 list 前检查是否仍有 children；有的话直接阻止删除。

### 6. CLI 和测试

目标：让 hierarchy 可见、可测。

1. `tool/ListCommand.cpp` 输出 `source` 信息。
2. `ListLayoutTest` 覆盖新的 header size / alignment / offsets。
3. `ListBuilderTest` 覆盖 `sourceListId` round-trip。
4. `ListViewTest` 覆盖 `sourceListId()`、`isRootSource()`、smart/manual 判定。
5. `SmartListHierarchyTest` 覆盖 parent update 传播、cycle detection、effective 语义。

## 主要文件

1. `include/rs/core/ListLayout.h`
2. `include/rs/core/ListView.h`
3. `src/core/ListView.cpp`
4. `include/rs/core/ListRecord.h`
5. `include/rs/core/ListBuilder.h`
6. `src/core/ListBuilder.cpp`
7. `tool/ListCommand.cpp`
8. `app/model/ListDraft.h`
9. `app/NewListDialog.h`
10. `app/NewListDialog.cpp`
11. `app/ListRow.h`
12. `app/ListRow.cpp`
13. `app/MainWindow.h`
14. `app/MainWindow.cpp`
15. `app/model/SmartListEngine.h`
16. `app/model/SmartListEngine.cpp`
17. `test/unit/core/ListLayoutTest.cpp`
18. `test/unit/core/ListBuilderTest.cpp`
19. `test/unit/core/ListViewTest.cpp`
20. `test/unit/app/SmartListHierarchyTest.cpp`

## 验证

代码搜索：

```bash
rg -n "sourceListId|Inherited Expression|Effective Expression|New Child List" app include src tool test
```

构建：

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

测试：

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

手工检查至少覆盖：

1. 创建根 smart list，source = `All Tracks`。
2. 基于父 list 创建 child smart list，并确认 preview 只在 parent membership 内运行。
3. 修改父 list 命中的歌曲元数据，确认 child list 自动收敛。
4. 创建三级 hierarchy，确认 sidebar 顺序和缩进正确。
5. 尝试创建环，确认 UI 阻止提交。
6. 尝试删除仍有 child 的 parent，确认删除被阻止。

## 完成标准

1. persisted schema 能表达 `sourceListId`。
2. `SmartListEngine` 能按 DAG 处理 parent/child rebuild。
3. child list 语义是“parent membership + local expression”。
4. `NewListDialog` 能选择 source、展示 inherited/effective expression，并做正确 preview。
5. sidebar 能展示 hierarchy。
6. cycle/delete 保护可用。
7. debug build 和测试通过。

## 后续增强

1. `Gtk::TreeListModel` 真正树形展示。
2. 编辑 parent / local expression。
3. parent 删除时支持级联移动或级联删除。
4. 如果 profile 证明值得，再考虑 sibling 级别的表达式 fusion。
