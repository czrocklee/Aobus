# Smart List Hierarchy And UI Plan

## 目标

在 `SMART_LIST_ENGINE_PLAN.md` 完成之后，给 list 增加父/子层级能力，并让 UI 能围绕“父 list 作为 source，子 list 追加自己的 local expression”来工作。

本计划的核心原则如下：

1. 持久化的是“parent/source 关系 + local expression”，不是把父子表达式预先拼成一条大字符串存库。
2. 运行时的有效语义等价于：`effective(child) = effective(parent) AND local(child)`。
3. 实际评估时优先复用 parent membership 作为 child source，而不是每次重新解析整条拼接后的大 expr。
4. UI 第一版先做清晰可用，不追求复杂的树控件交互。

## 为什么这样设计

如果直接把父子表达式拼成一条完整 filter 存进去，会有几个问题：

1. 父 list 改 expr 后，所有子 list 的持久化字符串都要同步改写。
2. UI 很难区分“继承来的条件”和“子 list 自己新增的条件”。
3. engine 很难做依赖传播，因为 schema 没有显式 parent/source 概念。

因此本计划采用下面这个持久化语义：

1. 每个 list 存一个 `sourceListId`。
2. 根 smart list 的 `sourceListId = ListId{0}`，表示来自虚拟根“所有歌曲”。
3. child smart list 的 `sourceListId = parent list id`。
4. `filter` 字段存 local expression，不存整个 effective expression。

## 非目标

本阶段先不做下面这些事情：

1. 不做 tree collapse / expand。
2. 不做 drag-and-drop 改 parent。
3. 不做 list union / intersection / sibling combine。
4. 不把 hierarchy 直接做进 parser 语言本身。

## 计划修改的文件

建议至少涉及下面这些文件：

1. `include/rs/core/ListLayout.h`
2. `include/rs/core/ListView.h`
3. `src/core/ListView.cpp`
4. `include/rs/core/ListRecord.h`
5. `include/rs/core/ListBuilder.h`
6. `src/core/ListBuilder.cpp`
7. `tool/ListCommand.cpp`
8. `test/unit/core/ListLayoutTest.cpp`
9. `test/unit/core/ListBuilderTest.cpp`
10. `test/unit/core/ListViewTest.cpp`
11. `app/model/ListDraft.h`
12. `app/ListRow.h`
13. `app/ListRow.cpp`
14. `app/NewListDialog.h`
15. `app/NewListDialog.cpp`
16. `app/MainWindow.h`
17. `app/MainWindow.cpp`
18. `app/model/SmartListEngine.h`
19. `app/model/SmartListEngine.cpp`
20. `test/unit/app/SmartListHierarchyTest.cpp`

## Step 0：确定语义和迁移策略

### 本步目标

在真正动 schema 之前，先把 hierarchy 的持久化语义定死，避免后续 UI 和 engine 各做一套。

### 本步必须确定的规则

1. `ListId{0}` 保留给虚拟根 source，表示 All Tracks。
2. persisted `filter` 只表示当前 list 的 local expression。
3. effective expression 只在运行时或 UI 展示时拼接，不直接持久化。
4. child list 的真实 source 是 parent list 的 membership。
5. list graph 必须是 DAG，禁止循环依赖。

### 推荐的表达式拼接规则

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

这个 helper 主要用于 UI 展示和日志，不应成为 runtime 的唯一语义来源。

## Step 1：给 list 持久化格式增加 `sourceListId`

### 本步目标

让 list schema 能显式表达 parent/source，同时保持对已有持久化数据的读取兼容。

### 关键约束

当前 `ListHeader` 是 16 字节，现有库里的 list payload 已经落库。因此不能简单改 header 大小后让新 `ListView` 直接读旧 payload，否则会把旧数据全部读坏。

### 推荐方案：引入 V2 Header，并保留 V1 读取兼容

建议：

1. 保留现有 `ListHeader` 作为 V1 定义。
2. 新增 `ListHeaderV2`，带 `magic/version + sourceListId`。
3. `ListView` 构造时先看 payload 头部是不是 V2 magic；是就按 V2 解析，否则按 V1 解析。
4. `ListBuilder::serialize()` 从这一步开始统一写 V2。

### 建议结构

```cpp
constexpr std::uint32_t kListMagicV2 = 0x3254534c; // 'LST2'

struct ListHeaderV2 final
{
  std::uint32_t magic;
  std::uint32_t sourceListId;
  std::uint32_t trackIdsCount;
  std::uint16_t nameOffset;
  std::uint16_t nameLen;
  std::uint16_t descOffset;
  std::uint16_t descLen;
  std::uint16_t filterOffset;
  std::uint16_t filterLen;
};
```

### 具体改法

1. `ListLayout.h` 增加 V2 header 和常量。
2. `ListView` 增加 `sourceListId()` 和 `isRootSource()`。
3. `ListRecord` 增加 `ListId sourceListId = ListId{0};`。
4. `ListBuilder` 增加 `.sourceListId(ListId id)` setter。
5. `ListBuilder::fromView()` / `record()` / `serialize()` 全部带上 `sourceListId`。

### 对旧数据的语义约定

旧 V1 payload 一律按下面规则解释：

1. manual list：`sourceListId = ListId{0}`。
2. smart list：`sourceListId = ListId{0}`。

也就是所有旧数据都视为根 list，不自动推断 hierarchy。

## Step 2：扩展 `ListDraft` 和创建流程

### 本步目标

让 app 层在创建 list 时能表达“这是哪个 parent/source 的 child list”。

### 需要修改的文件

1. `app/model/ListDraft.h`
2. `app/MainWindow.cpp`

### 建议结构

```cpp
struct ListDraft
{
  ListKind kind = ListKind::Smart;
  rs::core::ListId sourceListId = rs::core::ListId{0};
  std::string name;
  std::string description;
  std::string expression; // local expression
  std::vector<rs::core::TrackId> trackIds;
};
```

### 创建时的 builder 代码应改成

```cpp
auto builder = rs::core::ListBuilder::createNew()
                 .name(draft.name)
                 .description(draft.description)
                 .sourceListId(draft.sourceListId);

if (draft.kind == app::model::ListKind::Smart)
{
  builder.filter(draft.expression);
}
```

## Step 3：把 engine 升级为 DAG 调度器

### 本步目标

让 `SmartListEngine` 不仅能按 source 分桶，还能理解 list graph，并按拓扑顺序 rebuild / propagate。

### 推荐数据结构

```cpp
struct SmartListNode
{
  rs::core::ListId listId;
  rs::core::ListId sourceListId; // 0 = All Tracks
  std::vector<rs::core::ListId> children;
  RegistrationId registrationId;
};
```

### engine 需要新增的能力

1. 根据 `sourceListId` 找到 parent registration / parent membership。
2. 在 rebuild 时做拓扑排序：先根 list，再 child。
3. 当 parent membership 变化时，只重建它的 descendants，不触碰无关分支。
4. 当 graph 有环时，在创建前直接拒绝。

### rebuild 顺序建议

1. 所有 `sourceListId == 0` 的 smart list 作为第一层。
2. 第二层使用第一层已算出的 membership 作为 source。
3. 继续按层推进，直到 DAG 结束。

### 为什么这里仍然不需要复合 AST

因为 hierarchy 的关键收益来自：

1. parent membership 可以直接缩小 child 的扫描集合。
2. parent 变化可以只影响 descendants。
3. DAG 的结构信息天然比大字符串拼接更适合调度。

## Step 4：定义 parent/child expression 的 UI 语义

### 本步目标

让用户在新建 child list 时，能清楚看到“继承了什么”和“自己新增了什么”。

### UI 规则建议

1. `Expression` 文案改成 `Local Expression`。
2. 新增 `Source` / `Parent List` 选择器。
3. 当选择 parent 后，显示只读的 `Inherited Expression` 文本。
4. 预览时使用 parent membership 作为 source，再用 local expression 过滤。
5. 同时显示只读的 `Effective Expression`，方便用户确认最终逻辑。

### 推荐控件改动

在 `NewListDialog` 中新增：

1. `Gtk::DropDown _sourceListDropDown;`
2. `Gtk::Label _inheritedExprLabel;`
3. `Gtk::Label _effectiveExprLabel;`
4. `std::vector<ListChoice> _sourceChoices;`

### 交互草图

```text
Name
Description
Source: [ All Tracks v ]
Inherited Expression:
  (none)
Local Expression:
  $genre = Rock
Effective Expression:
  $genre = Rock
Preview: 42 tracks
```

如果选择 parent，例如 `Favorites`：

```text
Source: [ Favorites v ]
Inherited Expression:
  $rating >= 4
Local Expression:
  @bitrate >= 320k
Effective Expression:
  ($rating >= 4) and (@bitrate >= 320k)
```

## Step 5：扩展 `NewListDialog`

### 本步目标

让创建 smart list 的对话框能选择 parent/source，并正确做 preview。

### 需要修改的文件

1. `app/NewListDialog.h`
2. `app/NewListDialog.cpp`

### 具体改法

1. 构造函数多传入可选的默认 source 以及可选 source 列表。
2. `setupUi()` 里在 expression 区域前插入 source selector。
3. `updatePreview()` 改成根据当前选中的 source list 构造 preview source。
4. `draft()` 把 `sourceListId` 一起返回。

### preview 语义

preview 不应该通过把 parent 和 local expression 拼成字符串再跑 `_allTrackIds` 得出，而应该：

1. 先定位当前 source list。
2. 把该 source list 的 membership 作为 preview source。
3. 对 local expression 做过滤。

这样 preview 才和 engine 的真实运行方式一致。

### 错误展示规则

1. parent/source 不存在：显示不可提交错误。
2. local expression 非法：沿用现有 error label。
3. 选择 parent 会形成环：在对话框内阻止提交。

## Step 6：扩展 `MainWindow` 和 sidebar

### 本步目标

让 hierarchy 能在主界面里可见、可创建、可导航。

### 建议的最小可用方案

第一版不要直接切 GTK tree model，先继续用平面 `ListStore`，但以“前序遍历 + depth 缩进”的方式展示。

### 需要修改的文件

1. `app/ListRow.h`
2. `app/ListRow.cpp`
3. `app/MainWindow.h`
4. `app/MainWindow.cpp`

### `ListRow` 建议新增字段

```cpp
class ListRow final : public Glib::Object
{
public:
  ListId getSourceListId() const;
  int getDepth() const;
  bool isSmart() const;
};
```

### sidebar 显示策略

1. 根 list depth = 0。
2. child list depth = parent depth + 1。
3. label 前面加固定宽度缩进或树形前缀，例如 `"  " * depth`。
4. 继续保持单选，不在本阶段加入折叠。

### 新增入口

在 sidebar 右键菜单里新增：

1. `New Child List`

行为如下：

1. 如果当前选中 `All Tracks`，等价于普通 `New List`。
2. 如果当前选中普通 list，则打开 `NewListDialog` 并默认把它设为 `sourceListId`。

## Step 7：删除和循环依赖策略

### 本步目标

给 hierarchy 加上最小可用的一致性保护。

### 推荐规则

第一版采用最保守策略：

1. 不允许删除仍有 child 的 parent list。
2. 创建或修改 source 时，只要形成环就拒绝。

### 为什么先阻止删除

1. 比 cascade delete 风险小。
2. 不需要额外确认对话框和撤销逻辑。
3. 先把 hierarchy 跑通更重要。

### 需要增加的检查

1. `MainWindow::onDeleteList()` 删除前查 children。
2. `createList()` 提交前调用 graph validator。
3. engine 注册节点前再次做一次防御性 cycle check。

## Step 8：工具和测试补齐

### 工具侧建议

`tool/ListCommand.cpp` 至少要更新只读输出，否则 CLI 无法看出 hierarchy 信息。

建议输出格式：

```text
12  Favorites
     [smart] source: all-tracks
     [smart] filter: "$rating >= 4"

13  Favorites / Lossless
     [smart] source: 12
     [smart] filter: "@bitDepth >= 16"
```

### 建议测试

1. `ListLayoutTest`：V1 / V2 header 都能识别。
2. `ListBuilderTest`：`sourceListId` round-trip。
3. `ListViewTest`：V1 旧 payload 仍可读，`sourceListId()` 返回 0。
4. `SmartListHierarchyTest`：parent membership 变化时 child 自动收敛。
5. `SmartListHierarchyTest`：cycle detection。
6. `SmartListHierarchyTest`：空 parent expr + local expr 的 effective 语义正确。

## 验证步骤

### 代码搜索验证

```bash
rg -n "sourceListId|Inherited Expression|Effective Expression|New Child List" app include src tool test
```

完成标准：

1. core schema、app UI、tool 输出、测试中都已经接入 `sourceListId`。
2. hierarchy UI 文案已经落地。

### 构建验证

```bash
nix-shell --run "cmake --preset linux-debug"
nix-shell --run "cmake --build /tmp/build --parallel"
```

### 自动测试验证

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure"
```

### 手工验证

至少覆盖下面这些场景：

1. 创建一个根 smart list，source 选 `All Tracks`，确认 preview 和正式页面一致。
2. 基于该根 list 创建 child smart list，确认 preview 只在 parent 命中集合内运行。
3. 修改父 list 命中的歌曲元数据，确认 child list 也随之收敛。
4. 创建三级嵌套 list，确认 sidebar 顺序和缩进正确。
5. 尝试创建环，确认 UI 阻止提交。
6. 尝试删除仍有 child 的 parent，确认删除被阻止且提示清晰。
7. 打开已有旧库数据，确认旧 list 仍可正常显示，且被视为 root list。

## 完成标准

当下面条件都满足时，说明 hierarchy + UI 这一步完成：

1. persisted list schema 能表达 `sourceListId`，并兼容读取旧数据。
2. `SmartListEngine` 能按 DAG 处理 parent/child rebuild 和 update。
3. child list 语义是“parent membership + local expression”。
4. `NewListDialog` 能选择 source、展示 inherited/effective expression，并正确 preview。
5. sidebar 能以最小成本展示 hierarchy。
6. cycle/delete 保护可用。
7. debug build、自动测试、手工验证通过。

## 这一步之后再考虑的增强项

本计划完成后，再决定是否做这些增强：

1. `Gtk::TreeListModel` 真正树形展示。
2. list 重命名 / 改 parent / 编辑 local expression。
3. parent 删除时支持级联移动或级联删除。
4. 如果 profile 证明值得，再考虑 sibling 级别的表达式 fusion。
