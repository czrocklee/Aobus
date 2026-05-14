# StyleManager: CSS 统一管理 + 设计 token

## 目标

1. 所有应用 CSS 集中在一个地方，用 CSS 变量替代魔法数字
2. `ao-` 前缀命名约定，用户可精准覆盖任何组件
3. 信任系统主题（颜色/字体/图标），只定义 Aobus 特有的东西
4. 用户自定义 CSS 最高优先级，hot-reload
5. 不做跨平台抽象，只做 GTK4 内部整理

## 不做的事

- 不建 `lib/appearance/` 跨平台层（等有第二个平台再说）
- 不定义 preset 系统（先做好变量体系，preset 是未来换一套变量值而已）
- 不碰 Gtk::Settings 的字体/图标设置（系统接管，不做中转）

## 背景：外观层级模型

```
⑧ 用户 CSS              USER priority, 最高            用户控制
⑦ GTK Settings          gtk-font-name, gtk-theme-name  用户桌面选择
⑥ GTK 主题 CSS           Adwaita/Breeze 内部样式        系统控制
⑤ 应用 CSS 变量          :root { --ao-* }               App 控制
④ 应用全局 CSS           columnview { }, * { }
③ Widget name CSS       #now-playing-title { }
② 父容器级联             .playing-row label { }
① Widget class CSS      .track-title { }
```

原则：能交给下层的就不要自己定义，能抽象的不要写死。

## 架构

```
StyleManager (单例, app/StyleManager.h/cpp)
│
├── 系统主题同步
│   ├── syncGtkSettings()        从 ~/.config/gtk-4.0/settings.ini 读取并写入 GSettings/Gtk::Settings
│   └── 监听的 key: gtk-theme-name, gtk-application-prefer-dark-theme
│
├── 统一 app CSS provider        (APPLICATION priority, 600)
│   ├── :root { --ao-* }         设计 token
│   └── 所有组件 CSS             引用 var(--ao-*)
│
├── 用户 CSS provider            (USER priority, 800)
│   └── ~/.config/aobus/user.css
│
├── 文件监听（两个来源，缺一不可）
│   ├── ~/.config/gtk-4.0/       settings.ini / gtk.css 变化 → reload()（NixOS/Stylix 主题切换）
│   └── ~/.config/aobus/          user.css 变化 → reloadUserCss()
│
├── DBus portal 订阅
│   └── org.freedesktop.portal.Settings.SettingChanged → reload()
│
├── SIGUSR1 处理
│   └── kill -SIGUSR1 → reload()
│
├── 动态 CSS 注册                (per-widget, 如 TrackColumnController 的 --ao-title-x)
│   └── registerWidgetProvider() / unregisterWidgetProvider()
│
└── signalRefreshed()            热重载信号，替代 ThemeBus::signalThemeRefresh()
```

**初始化时机**: `StyleManager::initialize()` 必须在 `Gdk::Display::get_default()` 可用之后调用（`add_provider_for_display()` 依赖 display）。建议在 `Gtk::Application::signal_activate` handler 中，window 创建之前调用。

## 间距控制分层

### 问题的本质

GTK4 的间距有两种控制方式：C++ API（`Gtk::Box::set_spacing()`）和 CSS（`margin`/`padding`）。两者能力不同：

| 能力 | C++ API | GTK4 CSS |
|------|---------|----------|
| Box 子控件间隙 | `set_spacing()` ✅ | ❌ 不支持 `gap` |
| Widget 四边 margin | `set_margin_*()` ✅ | ✅ 完整支持 |
| Widget 四边 padding | ❌ 无直接 API | ✅ 完整支持 |
| 用户热覆盖 | ❌ 需改布局文件 | ✅ `user.css` 即可 |

关键限制：**GTK4 CSS 没有 `gap` 属性**，Box 的 `spacing` 只能用 C++ API 控制。所以 spacing 不能全部交给 CSS。

### 分层策略

```
┌──────────────────────────────────────────────────┐
│  Box spacing（子控件间隙）                        │
│  → layout editor 管理                            │
│  原因: GTK4 CSS 不支持 gap                       │
│  取值: --ao-gap-* token，不写死数字               │
├──────────────────────────────────────────────────┤
│  Widget margin / padding                         │
│  → CSS 管理（StyleManager）                      │
│  原因: GTK4 CSS 完整支持，用户可覆盖              │
│  layout editor 的 margin prop 退役               │
├──────────────────────────────────────────────────┤
│  颜色 / 字体 / 圆角 / 动画                        │
│  → CSS 管理（StyleManager）                      │
│  原因: CSS 最擅长的领域                          │
└──────────────────────────────────────────────────┘
```

### 容器 spacing

保留 layout editor 的 `spacing` prop，但值改为引用 token：

```yaml
# layout 文档
playback-row:
  type: box
  spacing: md        # → 从 token 查找 --ao-gap-md
  children: [...]
```

C++ 端 `BoxComponent` 从 token 解析 spacing 值，不再用 `kSpacingMedium = 6` 这种硬编码常量。

两个按钮需要 0 间距紧贴时，嵌套子 Box：

```yaml
transport-group:
  type: box
  spacing: 0
  children:
    - play-button
    - next-button
```

### Widget margin / padding

归 CSS 管理。每个组件的默认 margin/padding 定义在 StyleManager 里：

```css
/* 全局默认: 组件无外边距 */
* { margin: 0; padding: 0; }

/* 具体组件按需定义 */
.ao-playback-button {
  padding: var(--ao-spacing-sm) var(--ao-spacing-md);
  margin: 0;
}

.ao-trackview-row {
  padding: var(--ao-spacing-sm) var(--ao-spacing-md);
}

.ao-tag-chip {
  padding: var(--ao-spacing-xs) var(--ao-spacing-md);
}
```

用户覆盖：

```css
/* ~/.config/aobus/user.css */
.ao-playback-button {
  margin-start: 8px;
  padding: 2px 16px;
}
```

### 迁移计划

- **Phase 2**: 各组件的 C++ `set_margin_*()`、`set_padding()` 调用移除，对应样式迁入 StyleManager CSS
- **Phase 2**: `applyCommonProps()` 中的 `margin` prop 处理标记 deprecated（不删代码，只是 layout 文档不再使用）
- **Phase 4**: `LayoutConstants.h` 中的 `kMargin*` 系列标记 deprecated，CSS token 作为间距的唯一数据源
- **不迁移**: `spacing` prop 保留在 layout system，值改为从 token 查询

## 设计 token（从现有代码提取的实际值）

```css
:root {
  /* 容器间隙（Box spacing） */
  --ao-gap-none: 0px;    /* 紧贴 */
  --ao-gap-sm: 4px;      /* 紧凑 */
  --ao-gap-md: 6px;      /* 默认 */
  --ao-gap-lg: 8px;      /* 宽松 */
  --ao-gap-xl: 12px;     /* 极宽 */

  /* margin/padding 间距 — 与 LayoutConstants.h 一致（kSpacingXSmall=2 将在 Phase 1 同步补充） */
  --ao-spacing-xs: 2px;
  --ao-spacing-sm: 4px;
  --ao-spacing-md: 6px;
  --ao-spacing-lg: 8px;
  --ao-spacing-xl: 12px;

  /* 圆角 */
  --ao-radius-sm: 4px;
  --ao-radius-md: 8px;
  --ao-radius-full: 100px;

  /* 音频质量语义色 */
  --ao-quality-perfect: #A855F7;
  --ao-quality-lossless: #10B981;
  --ao-quality-intervention: #F59E0B;
  --ao-quality-lossy: #6B7280;
  --ao-quality-clipped: #EF4444;

  /* 透明度 */
  --ao-opacity-dim: 0.4;
  --ao-opacity-muted: 0.7;

  /* 过渡 */
  --ao-transition-fast: 200ms;
  --ao-transition-normal: 250ms;

  /* 进度条 */
  --ao-seek-trough-height: 6px;
  --ao-seek-slider-size: 14px;

  /* 字号 */
  --ao-font-size-sm: 0.85rem;
  --ao-font-size-md: 0.9rem;
}
```

所有变量都可以在 `~/.config/aobus/user.css` 里覆盖。

## CSS 命名约定

### 有 CSS 定义的 class（需要重命名 + CSS 迁移）

| 旧 | 新 | 来源文件 |
|----|----|----------|
| `tag-chip` | `ao-tag-chip` | ShellLayoutController, TagEditor |
| `tag-remove-button` | `ao-tag-remove` | ShellLayoutController |
| `tags-entry` | `ao-tags-entry` | ShellLayoutController, TagEditor |
| `tags-section` | `ao-tags-section` | ShellLayoutController |
| `inspector-handle` | `ao-inspector-handle` | ShellLayoutController |
| `now-playing-label` | `ao-nowplaying` | NowPlayingStatusLabel |
| `clickable-label` | `ao-clickable` | NowPlayingStatusLabel |
| `device-row` | `ao-device-row` | OutputSelector |
| `menu-header` | `ao-menu-header` | OutputSelector |
| `menu-description` | `ao-menu-description` | OutputSelector |
| `selected-row` | `ao-selected-row` | OutputSelector |
| `output-button-logo` | `ao-output-logo` | OutputSelector |
| `rich-list` | `ao-rich-list` | OutputSelector |
| `sink-status-*` | `ao-sink-*` | PlaybackDetailsWidget |
| `soul-window` | `ao-soul-window` | AobusSoulWindow |
| `playing-row` | `ao-playing-row` | TrackColumnFactoryBuilder |
| `playing-title` | `ao-playing-title` | TrackColumnFactoryBuilder |
| `playing-dim` | `ao-playing-dim` | TrackColumnFactoryBuilder |
| `inline-editor-stack` | `ao-inline-editor-stack` | TrackColumnFactoryBuilder |
| `inline-editor-label` | `ao-inline-editor-label` | TrackColumnFactoryBuilder |
| `inline-editor-entry` | `ao-inline-editor-entry` | TrackColumnFactoryBuilder |
| `track-tags-cell` | `ao-track-tags-cell` | TrackColumnFactoryBuilder |
| `status-bar` | `ao-status-bar` | StatusComponents |
| `status-message` | `ao-status-message` | StatusNotificationLabel |
| `error` | `ao-error` | TrackFilterController, SmartListDialog |
| `playback-button` | `ao-playback-button` | TransportButton |
| `playback-button-small` | `ao-playback-button-small` | TransportButton |
| `playback-button-large` | `ao-playback-button-large` | TransportButton |
| `playback-title` | `ao-playback-title` | NowPlayingFieldLabel |
| `playback-artist` | `ao-playback-artist` | NowPlayingFieldLabel |
| `custom-view-editor` | `ao-custom-view-editor` | TrackCustomViewDialog |
| `custom-view-main-box` | `ao-custom-view-main-box` | TrackCustomViewDialog |
| `custom-view-row` | `ao-custom-view-row` | TrackCustomViewDialog |
| `custom-view-section-title` | `ao-custom-view-section-title` | TrackCustomViewDialog |
| `custom-view-list` | `ao-custom-view-list` | TrackCustomViewDialog |

### 纯语义标记 class（目前无 CSS 定义，仅重命名，未来可补充样式）

| 旧 | 新 | 来源文件 |
|----|----|----------|
| `hero-section` | `ao-hero-section` | TrackInspectorPanel |
| `hero-cover` | `ao-hero-cover` | TrackInspectorPanel |
| `metadata-section` | `ao-metadata-section` | TrackInspectorPanel |
| `property-label` | `ao-property-label` | TrackInspectorPanel |
| `property-editable` | `ao-property-editable` | TrackInspectorPanel |
| `audio-section` | `ao-audio-section` | TrackInspectorPanel |
| `technical-label` | `ao-technical-label` | TrackInspectorPanel |
| `technical-value` | `ao-technical-value` | TrackInspectorPanel |
| `section-header` | `ao-section-header` | TrackInspectorPanel |
| `inspector-sidebar` | `ao-inspector-sidebar` | TrackInspectorPanel |

### GTK 内置 class（不重命名）

| class | 说明 |
|-------|------|
| `dim-label` | GTK/Adwaita 内置样式 |
| `flat` | GTK 内置 |
| `boxed-list` | Adwaita 内置 |
| `suggested-action` | GTK 内置 |

## SeekControl 改造（验证设计的例子）

现状：零 CSS，纯默认 Gtk::Scale。

改造后：
```cpp
// SeekControl 构造函数加一行
_scale.add_css_class("ao-seekbar");
```

```css
/* StyleManager 里加 */
.ao-seekbar { padding: var(--ao-spacing-sm) 0; }
.ao-seekbar trough {
  min-height: var(--ao-seek-trough-height);
  border-radius: 3px;
  background: alpha(currentColor, 0.12);
}
.ao-seekbar highlight {
  background: @theme_selected_bg_color;
  border-radius: 3px;
}
.ao-seekbar slider {
  min-width: var(--ao-seek-slider-size);
  min-height: var(--ao-seek-slider-size);
  border-radius: 50%;
}
```

用户覆盖：`.ao-seekbar trough { min-height: 10px; }`

## 实现步骤

### Phase 1: StyleManager 骨架 + 设计 token

**新文件**: `app/StyleManager.h`, `app/StyleManager.cpp`

```cpp
// StyleManager API
namespace ao::gtk {

class StyleManager final {
public:
    static StyleManager& instance();

    // 初始化：注册 provider + 启动文件监听 + DBus 订阅 + SIGUSR1
    // 前提：Gdk::Display::get_default() 必须已可用
    void initialize();

    // 完整重载：syncGtkSettings + reloadAppCss + reloadUserCss + signalRefreshed
    void reload();

    sigc::signal<void()>& signalRefreshed();

    // 动态 CSS provider（保留 TrackColumnController 的用法）
    void registerWidgetProvider(Gtk::Widget&, Glib::RefPtr<Gtk::CssProvider>, guint priority);
    void unregisterWidgetProvider(Gtk::Widget&, Glib::RefPtr<Gtk::CssProvider> const&);

    Glib::RefPtr<Gtk::CssProvider> const& appProvider() const;

private:
    StyleManager() = default;

    void loadAppCss();        // APPLICATION priority, :root token + 全部组件 CSS
    void loadUserCss();       // USER priority, ~/.config/aobus/user.css
    void reloadUserCss();     // 仅重载用户 CSS（user.css 文件变化时）

    // 从 ThemeBus 迁入的系统主题同步逻辑
    void syncGtkSettings();   // 读取 ~/.config/gtk-4.0/settings.ini → GSettings/Gtk::Settings
    void reloadGtkUserCss();  // 重载 ~/.config/gtk-4.0/gtk.css（NixOS/Stylix 全局 CSS）

    void setupFileMonitors(); // 同时监听 ~/.config/gtk-4.0/ + ~/.config/aobus/
    void setupDBusMonitor();  // org.freedesktop.portal.Settings
    void setupSignalHandler();// SIGUSR1

    Glib::RefPtr<Gtk::CssProvider> _appProvider;
    Glib::RefPtr<Gtk::CssProvider> _userProvider;
    Glib::RefPtr<Gtk::CssProvider> _gtkUserCssProvider;  // ~/.config/gtk-4.0/gtk.css
    sigc::signal<void()> _refreshedSignal;
};

} // namespace ao::gtk
```

**关键设计决策**：

- `syncGtkSettings()` 完整保留原 `ThemeBus::syncSettingsFromFile()` 的逻辑——读取 `gtk-theme-name` 和 `gtk-application-prefer-dark-theme`，写入 GSettings（有 schema 时）或 `Gtk::Settings`（无 schema 时）。这是 NixOS/Stylix 主题切换的关键路径，不可丢弃。
- 两个独立的 CSS 文件加载：
  - `~/.config/gtk-4.0/gtk.css` → `_gtkUserCssProvider`（USER priority）— 系统级用户 CSS
  - `~/.config/aobus/user.css` → `_userProvider`（USER priority）— Aobus 专用覆盖
  两者都是 USER priority，CSS 级联中后加载的优先（`loadUserCss()` 在 `reloadGtkUserCss()` 之后调用）。
- Phase 1 同步补充 `LayoutConstants.h` 的 `kSpacingXSmall = 2`，与 `--ao-spacing-xs` 对齐。

### Phase 2: 迁移现有 CSS 到 StyleManager

**顺序**（从最独立的组件开始，每次迁移测试一次）：

**有 CSS 定义的组件（9 个 `ensure*Css` 函数）：**

1. `StatusNotificationLabel` — 空的 `.status-message` → `ao-status-message`
2. `StatusComponents` — `.status-bar` → `ao-status-bar`
3. `AobusSoulWindow` — `.soul-window` → `ao-soul-window`
4. `NowPlayingStatusLabel` — `.now-playing-label` + `.clickable-label` → `ao-nowplaying` + `ao-clickable`
5. `OutputSelector` — `.device-row` + `.menu-header` + `.selected-row` + `.menu-description` → `ao-*`
6. `PlaybackDetailsWidget` — `.sink-status-*` → `ao-sink-*`
7. `TrackColumnFactoryBuilder` — `.playing-row` + `.inline-editor-*` + `.track-tags-cell` → `ao-*`（最大块）
8. `ShellLayoutController` — `.tag-chip` + `.inspector-handle` + `.tags-entry` + `.tag-remove-button` + `.tags-section` → `ao-*`（第二大块）

**纯 class 重命名（无 CSS 定义，仅更新 `add_css_class` 字符串）：**

9. `TrackInspectorPanel` — 10 个 class 重命名（`hero-section` → `ao-hero-section` 等）
10. `TrackCustomViewDialog` — 5 个 class 重命名（`custom-view-*` 系列）
11. `TransportButton` — 3 个 class 重命名（`playback-button*` 系列）
12. `TagEditor` — `tags-entry`, `tag-chip`, `dim-label`（`dim-label` 保持）
13. `NowPlayingFieldLabel` — `playback-title`, `playback-artist`
14. `TrackFilterController` — `error` → `ao-error`
15. `SmartListDialog` — `error` → `ao-error`

**每个 CSS 迁移的模式**：
- 删除 `ensure*Css()` 函数体
- 删除 `add_provider_for_display()` 调用
- CSS 内容移到 `StyleManager::loadAppCss()`
- `add_css_class()` / `remove_css_class()` 的字符串参数更新

### Phase 3: 替换 ThemeBus

- `main.cpp` SIGUSR1 → `StyleManager::instance().reload()`
- `main.cpp` DBus portal 订阅 → 移入 `StyleManager::setupDBusMonitor()`
- `main.cpp` FileMonitor（`~/.config/gtk-4.0/`）→ 移入 `StyleManager::setupFileMonitors()`
- `main.cpp` ThemeBus include → 改为 StyleManager include
- `TrackViewPage` → `StyleManager::instance().signalRefreshed()` 替代 `signalThemeRefresh()`
- `TrackSelectionController` → 删除 stale `#include "app/ThemeBus.h"`（该文件不调用任何 ThemeBus API）
- `ThemeBus` 变为空壳转发，标记 deprecated

### Phase 4: 验证 + 收尾

- SeekControl 加 `ao-seekbar` class + CSS
- **6 个 priority 降级验证**：以下组件从 `GTK_STYLE_PROVIDER_PRIORITY_USER` 降级到 `APPLICATION`，必须在 Adwaita dark / light 等多种主题下确认外观无回归：
  - `StatusNotificationLabel`, `TrackColumnFactoryBuilder`, `StatusComponents`, `PlaybackDetailsWidget`, `OutputSelector`, `NowPlayingStatusLabel`
- 确保无遗漏：grep `add_provider_for_display`、`load_from_data`、`CssProvider::create` 确认只剩 StyleManager 和 TrackColumnController 的动态 provider
- 跑 build + test

## 涉及文件

| 文件 | 变更 |
|------|------|
| `app/StyleManager.h` | **NEW** |
| `app/StyleManager.cpp` | **NEW** |
| `app/CMakeLists.txt` | 加 `app/StyleManager.cpp` |
| `app/ThemeBus.h` | 标记 deprecated |
| `app/ThemeBus.cpp` | 转发到 StyleManager |
| `main.cpp` | SIGUSR1/DBus/Monitor → StyleManager |
| `layout/LayoutConstants.h` | 补充 `kSpacingXSmall = 2`；`kMargin*` 系列标记 deprecated（CSS token 作为唯一数据源） |
| `layout/components/Containers.cpp` | `applyCommonProps()` 的 `margin` prop 处理标记 deprecated；BoxComponent `spacing` 改为从 token 查询 |
| `layout/editor/LayoutEditorDialog.cpp` | margin prop 标记 deprecated（保留代码但不推荐使用） |
| `track/TrackViewPage.cpp` | signalRefreshed 换源 |
| `track/TrackSelectionController.cpp` | 删除 stale `#include "app/ThemeBus.h"` |
| `app/ShellLayoutController.cpp` | 删 `setupCss()`，更新类名 |
| `track/TrackColumnFactoryBuilder.cpp` | 删 `ensureTrackPageCss()`，更新类名 |
| `track/StatusNotificationLabel.cpp` | 删 `ensureStatusNotificationCss()`，更新类名 |
| `playback/NowPlayingStatusLabel.cpp` | 删 `ensureNowPlayingCss()`，更新类名 |
| `playback/OutputSelector.cpp` | 删 `ensureOutputSelectorCss()`，更新类名 |
| `playback/PlaybackDetailsWidget.cpp` | 删 `ensurePlaybackDetailsCss()`，更新类名 |
| `playback/AobusSoulWindow.cpp` | 删 `ensureCss()`，更新类名 |
| `playback/TransportButton.cpp` | 更新类名 |
| `playback/NowPlayingFieldLabel.cpp` | 更新类名 |
| `layout/components/StatusComponents.cpp` | 删 `ensureStatusBarContainerCss()`，更新类名 |
| `playback/SeekControl.cpp` | 加 `add_css_class("ao-seekbar")` |
| `inspector/TrackInspectorPanel.cpp` | 更新类名 |
| `track/TrackCustomViewDialog.cpp` | 更新类名 |
| `tag/TagEditor.cpp` | 更新类名 |
| `track/TrackFilterController.cpp` | 更新类名 |
| `list/SmartListDialog.cpp` | 更新类名 |

## 验证

1. `./build.sh debug` 编译通过 + 测试全绿
2. 启动 app：曲目列表、tag chip、播放器、进度条、inspector、soul window、custom view dialog 外观与改造前一致
3. 切换系统明暗模式：所有组件跟随
4. 确认 6 个 priority 降级组件（StatusNotificationLabel, TrackColumnFactoryBuilder, StatusComponents, PlaybackDetailsWidget, OutputSelector, NowPlayingStatusLabel）在 Adwaita dark 下外观正确
5. 创建 `~/.config/aobus/user.css` 写入 `.ao-seekbar trough { min-height: 12px; }`，进度条变粗
6. 修改 `~/.config/gtk-4.0/settings.ini` 切换主题，app 跟随（NixOS/Stylix 场景）
7. `kill -SIGUSR1 $(pidof aobus)` 热重载生效
8. 切换 track presentation（Songs ↔ Albums）：ColumnView 正常重建，样式不丢失
