# src/tag 直接返回 TrackBuilder 迁移计划

## 目标

把 `src/tag` 的输出从 `ParsedTrack` 改成直接返回 `rs::core::TrackBuilder`，减少一次中间转存。

最终目标如下：

1. `tag::File::loadTrack()` 直接返回 `TrackBuilder`。
2. 能直接借用文件 `mmap` 内容的字段，使用 `std::string_view` 直接写入 `TrackBuilder`。
3. 需要转码或需要构造新字符串的字段，不再落到 `ParsedTrack::record`，而是暂存在 `tag::File` 的成员 `std::deque<std::string>` 中，再把 `string_view` 指向这些字符串。
4. embedded cover art 直接写入 `TrackBuilder::metadata().coverArtData(...)`，由 `TrackBuilder::serialize(...)` 统一提交到 `ResourceStore`。
5. 删除 `ParsedTrack` 类型和相关调用链。

## 为什么选 `std::deque<std::string>`

不要用 `std::vector<std::string>` 做 `File` 的临时字符串池。原因如下：

1. `vector` 追加时可能整体扩容搬迁，之前取出的 `string_view` 会失效。
2. `deque` 追加时不会像 `vector` 一样整体搬迁已有元素，更适合“不断追加、然后借用已有字符串地址”的场景。
3. 这个字符串池只是解析期间的临时持有容器，不需要连续内存。

本方案默认用：

```cpp
mutable std::deque<std::string> _ownedStrings;
```

## 必须牢记的生命周期规则

这是整个迁移里最重要的规则。

`loadTrack()` 返回的 `TrackBuilder` 内部会借用两类内存：

1. `tag::File` 持有的 `mmap` 文件内容。
2. `tag::File::_ownedStrings` 里的转码字符串。

因此必须满足下面两条：

1. `TrackBuilder` 必须在 `tag::File` 仍然存活时完成 `serialize()`。
2. 同一个 `File` 对象如果再次调用 `loadTrack()` 并清空 `_ownedStrings`，前一次返回的 builder 就不再安全。

绝对不要保留一个 builder，然后让对应的 `tagFile` 先析构，再去使用 builder。

## 非目标

以下内容不在这次改动范围内：

1. 不重构 `TrackBuilder` 的整体序列化逻辑。
2. 不删除 `TrackRecord`。`TrackRecord` 仍然用于 `builder.record()`、测试和其他非 tag 场景。
3. 不做 UI 或查询层变更。

## 最终应当修改的文件

需要修改的文件应当包含：

1. `include/rs/tag/File.h`
2. `include/rs/tag/flac/File.h`
3. `include/rs/tag/mpeg/File.h`
4. `include/rs/tag/mp4/File.h`
5. `include/rs/tag/ParsedTrack.h`
6. `src/tag/flac/File.cpp`
7. `src/tag/flac/VorbisCommentDispatch.gperf`
8. `src/tag/mpeg/File.cpp`
9. `src/tag/mpeg/id3v2/Reader.h`
10. `src/tag/mpeg/id3v2/Reader.cpp`
11. `src/tag/mpeg/id3v2/FrameDispatch.gperf`
12. `src/tag/mp4/File.cpp`
13. `src/tag/mp4/AtomDispatch.gperf`
14. `src/tag/Decoder.h`
15. `app/ImportWorker.cpp`
16. `tool/TrackUtils.h`
17. `tool/TrackUtils.cpp`
18. `tool/TrackCommand.cpp`
19. `tool/InitCommand.cpp`
20. `test/integration/tag/TagTest.cpp`

根据实际实现，也可能需要小范围调整对应的 include。

## 总体执行顺序

严格按下面顺序做，不要跳步骤。

1. 先改公共 API 和 `File` 的字符串池。
2. 再改 FLAC。
3. 再改 MP4。
4. 最后改 MP3/ID3v2。
5. 再改调用方。
6. 最后改测试和跑验证。

这样做的原因：

1. FLAC 和 MP4 大多是直接 `string_view`，难度最低。
2. MP3 需要转码，最复杂，放最后更容易收敛。
3. 调用方只有在 API 稳定后再改最省事。

---

## Step 0: 改动前的检查

### 本步目标

确认当前代码里 `ParsedTrack` 和 `TrackBuilder` 的使用点，避免漏改。

### 要执行的命令

```bash
rg -n "\bParsedTrack\b|loadTrack\(|fromRecord\(std::move\(parsed\.record\)\)" include src app tool test
```

### 本步完成标准

1. 找到所有 `ParsedTrack` 定义和引用。
2. 找到所有 `loadTrack()` 调用点。
3. 找到所有 `parsed.record` 和 `parsed.embeddedCoverArt` 调用点。

### 如果结果不符合预期

如果搜索结果比本计划列出的文件更多，把新增命中项补进本计划的待改列表后再继续。

---

## Step 1: 修改公共 API，给 `tag::File` 增加字符串池

### 本步目标

完成所有 tag parser 的基础设施改造，但先不动格式解析细节。

### 需要修改的文件

1. `include/rs/tag/File.h`
2. `include/rs/tag/flac/File.h`
3. `include/rs/tag/mpeg/File.h`
4. `include/rs/tag/mp4/File.h`
5. `src/tag/mpeg/id3v2/Reader.h`

### 具体改法

#### 1. `include/rs/tag/File.h`

做下面几件事：

1. 删除对 `ParsedTrack.h` 的依赖。
2. include `rs/core/TrackBuilder.h`。
3. include `<deque>`、`<string>`、`<string_view>`。
4. 把 `loadTrack()` 返回值改成 `rs::core::TrackBuilder`。
5. 在 `protected:` 里加 `_ownedStrings` 和辅助函数。

### 推荐代码示例

```cpp
// before
#include <rs/tag/ParsedTrack.h>

virtual ParsedTrack loadTrack() const = 0;
```

```cpp
// after
#include <rs/core/TrackBuilder.h>

#include <deque>
#include <string>
#include <string_view>

virtual rs::core::TrackBuilder loadTrack() const = 0;

protected:
  void clearOwnedStrings() const
  {
    _ownedStrings.clear();
  }

  std::string_view stashString(std::string value) const
  {
    _ownedStrings.push_back(std::move(value));
    return _ownedStrings.back();
  }

  mutable std::deque<std::string> _ownedStrings;
```

### 注释要求

务必在 `loadTrack()` 的注释里明确写出：

1. 返回的 builder 可能借用 `mmap` 数据。
2. 返回的 builder 也可能借用 `_ownedStrings`。
3. 这些借用数据仅在该 `File` 对象存活且未再次 `loadTrack()` 前有效。

#### 2. `include/rs/tag/flac/File.h` / `include/rs/tag/mpeg/File.h` / `include/rs/tag/mp4/File.h`

统一把：

```cpp
ParsedTrack loadTrack() const override;
```

改成：

```cpp
rs::core::TrackBuilder loadTrack() const override;
```

#### 3. `src/tag/mpeg/id3v2/Reader.h`

把：

```cpp
ParsedTrack loadFrames(HeaderLayout const& header, void const* buffer, std::size_t size);
```

改成至少能访问 owner storage 的形式。推荐：

```cpp
rs::core::TrackBuilder loadFrames(rs::tag::File const& owner,
                                  HeaderLayout const& header,
                                  void const* buffer,
                                  std::size_t size);
```

### 本步验证命令

```bash
rg -n "ParsedTrack loadTrack|virtual ParsedTrack loadTrack|ParsedTrack loadFrames" include src
```

### 本步完成标准

搜索结果中不应再出现：

1. `ParsedTrack loadTrack`
2. `virtual ParsedTrack loadTrack`
3. `ParsedTrack loadFrames`

### 注意事项

此时编译大概率还是会失败，因为实现和调用方还没跟上。这是正常的，不要在这一步试图修完所有错误。

---

## Step 2: 删除 `ParsedTrack`，先让编译错误集中暴露

### 本步目标

删除旧中间类型，让剩余错误全部变成显式待办。

### 需要修改的文件

1. `include/rs/tag/ParsedTrack.h`
2. 所有 include 它的文件

### 具体改法

1. 删除 `include/rs/tag/ParsedTrack.h`。
2. 把所有 `#include <rs/tag/ParsedTrack.h>` 改成需要的真实头文件。
3. 如果某个文件只是为了用 `ParsedTrack` 而 include 它，现在应改成 include `TrackBuilder.h` 或 `File.h`。

### 搜索命令

```bash
rg -n "ParsedTrack.h|\bParsedTrack\b" include src app tool test
```

### 本步完成标准

搜索结果中不应再有 `ParsedTrack`。

### 注意事项

如果 gperf 生成代码也间接依赖旧签名，不要手工修改生成文件，而是修改 `.gperf` 源文件。

---

## Step 3: 迁移 FLAC，优先完成最简单的一条链路

### 本步目标

让 FLAC 解析直接构建 `TrackBuilder`，不再经过 `TrackRecord`。

### 需要修改的文件

1. `src/tag/flac/File.cpp`
2. `src/tag/flac/VorbisCommentDispatch.gperf`

### 迁移原则

FLAC 是最适合零拷贝的格式：

1. Vorbis comment 的 `key/value` 已经是 `std::string_view`。
2. 这些 view 直接指向 `mmap`。
3. 数字字段本来就是解析成整数。
4. picture block 本来就是 span。

### `src/tag/flac/File.cpp` 要做什么

把下面这类写法：

```cpp
auto parsed = ParsedTrack{};
parsed.record.property.sampleRate = view.sampleRate();
parsed.record.custom.pairs.emplace_back(key, value);
parsed.embeddedCoverArt = PictureBlockView{iter->data()}.blob();
```

改成：

```cpp
auto builder = rs::core::TrackBuilder::createNew();
clearOwnedStrings();

builder.property().sampleRate(view.sampleRate());
builder.custom().add(key, value);
builder.metadata().coverArtData(PictureBlockView{iter->data()}.blob());
```

### 推荐完整骨架

```cpp
rs::core::TrackBuilder File::loadTrack() const
{
  if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
  {
    RS_THROW(rs::Exception, "unrecognized flac file content");
  }

  clearOwnedStrings();
  auto builder = rs::core::TrackBuilder::createNew();

  auto iter = MetadataBlockViewIterator{
    static_cast<char const*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
  auto end = MetadataBlockViewIterator{};

  for (; iter != end; ++iter)
  {
    switch (iter->type())
    {
      case MetadataBlockType::StreamInfo:
      {
        auto view = StreamInfoBlockView{iter->data()};
        builder.property()
          .sampleRate(view.sampleRate())
          .channels(view.channels())
          .bitDepth(view.bitDepth());
        break;
      }

      case MetadataBlockType::VorbisComment:
      {
        VorbisCommentBlockView{iter->data()}.visitComments([&](std::string_view comment) {
          auto pos = comment.find('=');
          if (pos == std::string_view::npos) { return; }

          auto key = comment.substr(0, pos);
          auto value = comment.substr(pos + 1);

          if (auto const* entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size()))
          {
            entry->handler(builder, value);
          }
          else
          {
            builder.custom().add(key, value);
          }
        });
        break;
      }

      case MetadataBlockType::Picture:
      {
        builder.metadata().coverArtData(PictureBlockView{iter->data()}.blob());
        break;
      }

      default:
        break;
    }
  }

  return builder;
}
```

### `.gperf` 怎么改

当前 handler 签名是面向 `TrackRecord::Metadata&` 的。把它改成面向 `TrackBuilder&`。

例如从：

```cpp
using VorbisHandler = void (*)(rs::core::TrackRecord::Metadata&, std::string_view);
```

改成：

```cpp
using VorbisHandler = void (*)(rs::core::TrackBuilder&, std::string_view);
```

然后把 `assignTextField` 这类函数改成直接调用 `builder.metadata().title(...)` 之类的 setter。

### 本步验证命令

```bash
rg -n "parsed\.record|embeddedCoverArt|ParsedTrack" src/tag/flac
```

### 本步完成标准

`src/tag/flac` 下不应再出现：

1. `ParsedTrack`
2. `parsed.record`
3. `embeddedCoverArt`

---

## Step 4: 迁移 MP4，尽量直接借用 atom 文本

### 本步目标

把 MP4 解析改成直接写 `TrackBuilder`，避免无意义的字符串复制。

### 需要修改的文件

1. `src/tag/mp4/File.cpp`
2. `src/tag/mp4/AtomDispatch.gperf`
3. `src/tag/Decoder.h`

### 关键原则

当前 MP4 的 `decodeString()` 只是把字节拷贝到 `std::string`。如果字段本身不需要转码，就不应该走这次复制。

### 推荐新增的 helper

在 `src/tag/mp4/File.cpp` 里增加：

```cpp
std::string_view atomTextView(AtomView const& view)
{
  auto data = atomData(view);
  return {reinterpret_cast<char const*>(data.data()), data.size()};
}
```

### 典型替换

把：

```cpp
void setText(std::string& field, AtomView const& view)
{
  field = decodeString(atomData(view));
}
```

改成直接给 builder：

```cpp
template<auto Setter>
void assignTextField(rs::core::TrackBuilder& builder, AtomView const& view)
{
  (builder.metadata().*Setter)(atomTextView(view));
}
```

如果成员函数指针模板太绕，不要强行炫技。可以写普通函数，例如：

```cpp
void assignTitle(rs::core::TrackBuilder& builder, AtomView const& view)
{
  builder.metadata().title(atomTextView(view));
}
```

### custom field 的处理

把：

```cpp
parsed.record.custom.pairs.emplace_back(std::string{type}, decodeString(atomData(view)));
```

改成：

```cpp
builder.custom().add(type, atomTextView(view));
```

### cover art 的处理

把：

```cpp
parsed.embeddedCoverArt = atomData(view);
```

改成：

```cpp
builder.metadata().coverArtData(atomData(view));
```

### `src/tag/Decoder.h` 怎么处理

如果 `decodeString()` 只剩 MP4 旧逻辑使用，就删掉它或缩小它的使用范围。不要为了兼容旧思路保留无用 helper。

### 本步验证命令

```bash
rg -n "decodeString\(|parsed\.record|embeddedCoverArt|ParsedTrack" src/tag/mp4 src/tag/Decoder.h
```

### 本步完成标准

1. `src/tag/mp4` 下不再出现 `ParsedTrack`。
2. MP4 主路径不再依赖 `decodeString()` 做普通文本复制。

---

## Step 5: 迁移 MP3/ID3v2，转码结果写入 `File::_ownedStrings`

### 本步目标

完成最复杂的一条链路：ID3v2 文本 frame 经过转码后，字符串由 `tag::File` 持有，builder 只借用 `string_view`。

### 需要修改的文件

1. `src/tag/mpeg/File.cpp`
2. `src/tag/mpeg/id3v2/Reader.h`
3. `src/tag/mpeg/id3v2/Reader.cpp`
4. `src/tag/mpeg/id3v2/FrameDispatch.gperf`

### 核心原则

1. `V23TextFrameView::text()` 返回 `std::string`，这是转码结果。
2. 这个 `std::string` 必须马上放进 `owner.stashString(...)`。
3. builder 保存的是 `stashString(...)` 返回的 `std::string_view`。

### `src/tag/mpeg/File.cpp` 应当怎么写

在 `loadTrack()` 开头先：

```cpp
clearOwnedStrings();
auto builder = rs::core::TrackBuilder::createNew();
```

解析 ID3v2 时，调用新的 reader：

```cpp
builder = id3v2::loadFrames(*this, *id3v2Header, id3v2Header + 1, id3v2BodySize);
```

剩余音频属性仍然继续写到这个 builder 上。

### `Reader.cpp` 的典型写法

把旧写法：

```cpp
parsed.record.metadata.*Member = view.text();
```

改成：

```cpp
builder.metadata().title(owner.stashString(view.text()));
```

### 推荐 helper 形态

```cpp
template<auto Setter>
void handleText(rs::core::TrackBuilder& builder,
                rs::tag::File const& owner,
                void const* data,
                std::size_t size)
{
  V23TextFrameView view{data, size};
  (builder.metadata().*Setter)(owner.stashString(view.text()));
}
```

如果模板太复杂，写多个普通函数也可以，优先保证容易维护。

### `TXXX` 的处理

把旧逻辑：

```cpp
parsed.record.custom.pairs.emplace_back(std::string{key}, std::string{value});
```

改成：

```cpp
auto text = owner.stashString(view.text());
auto nullPos = text.find('\0');
if (nullPos != std::string_view::npos)
{
  auto key = text.substr(0, nullPos);
  auto value = text.substr(nullPos + 1);

  if (key == "rating")
  {
    builder.metadata().rating(static_cast<std::uint8_t>(std::atoi(value.data())));
  }
  else
  {
    builder.custom().add(key, value);
  }
}
```

### 非常重要的检查点

`V23TextFrameView::text()` 当前会 trim 末尾 `\0`。这意味着 `TXXX` 里依赖 `description\0value` 的逻辑可能本来就不牢靠。

执行这一步时必须重新检查：

1. `view.text()` 返回值里是否还保留中间的 `\0`。
2. 如果没有保留，就不能继续用 `text.find('\0')`。
3. 这时必须改成按 frame 原始布局解析 `description` 和 `value`。

不要假设旧逻辑一定正确。

### APIC cover art 的处理

把：

```cpp
parsed.embeddedCoverArt = viewBytes(std::span{...});
```

改成：

```cpp
builder.metadata().coverArtData(viewBytes(std::span{...}));
```

### 本步验证命令

```bash
rg -n "ParsedTrack|parsed\.record|embeddedCoverArt" src/tag/mpeg
```

### 本步完成标准

`src/tag/mpeg` 下不应再出现：

1. `ParsedTrack`
2. `parsed.record`
3. `embeddedCoverArt`

---

## Step 6: 修改调用方，保证 builder 不会晚于 `tagFile` 使用

### 本步目标

确保所有调用方都遵守新的生命周期规则。

### 需要修改的文件

1. `app/ImportWorker.cpp`
2. `tool/TrackUtils.h`
3. `tool/TrackUtils.cpp`
4. `tool/TrackCommand.cpp`
5. `tool/InitCommand.cpp`

### `app/ImportWorker.cpp`

把旧流程：

```cpp
auto parsed = tagFile->loadTrack();
parsed.record.property.uri = ...;
parsed.record.property.fileSize = ...;
parsed.record.property.mtime = ...;

if (!parsed.embeddedCoverArt.empty())
{
  parsed.record.metadata.coverArtId = resourceWriter.create(parsed.embeddedCoverArt).value();
}

auto builder = rs::core::TrackBuilder::fromRecord(std::move(parsed.record));
auto [hotData, coldData] = builder.serialize(txn, dict, _ml.resources());
```

改成：

```cpp
auto builder = tagFile->loadTrack();

builder.property()
  .uri(std::filesystem::relative(path, _ml.rootPath()).string())
  .mtime(std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count());

if (std::filesystem::exists(path))
{
  builder.property().fileSize(std::filesystem::file_size(path));
}

auto [hotData, coldData] = builder.serialize(txn, dict, _ml.resources());
```

注意：不要再手工创建 cover art 资源。让 `TrackBuilder::serialize(...)` 自己提交。

### `tool/TrackUtils.*` 的处理原则

当前 helper 返回 `TrackBuilder`，这是危险的，因为函数结束时 `tagFile` 已经销毁。

有两种安全方案，推荐第一种：

#### 方案 A，推荐：删掉 `TrackUtils` helper

把逻辑直接内联到：

1. `tool/TrackCommand.cpp`
2. `tool/InitCommand.cpp`

让 `tagFile` 在同一个作用域里活到 `serialize()` 完成。

#### 方案 B，不推荐：helper 直接返回 hot/cold bytes

这样虽然也安全，但会让 helper 变复杂，不如直接内联直观。

### 推荐内联代码示例

```cpp
auto tagFile = rs::tag::File::open(path);
if (!tagFile)
{
  // 按原有逻辑处理 unsupported file
}

auto builder = tagFile->loadTrack();
builder.property()
  .uri(path.string())
  .fileSize(std::filesystem::file_size(path))
  .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

auto [hotData, coldData] = builder.serialize(txn, dict, ml.resources());
```

### 本步验证命令

```bash
rg -n "fromRecord\(std::move\(parsed\.record\)\)|embeddedCoverArt|parsed\.record" app tool
```

### 本步完成标准

`app` 和 `tool` 下不应再出现：

1. `parsed.record`
2. `embeddedCoverArt`
3. `fromRecord(std::move(parsed.record))`

---

## Step 7: 修改测试，改成围绕 builder 验证

### 本步目标

把 tag 集成测试从 `ParsedTrack` 断言迁移到 `TrackBuilder`。

### 需要修改的文件

1. `test/integration/tag/TagTest.cpp`

### 基本替换规则

把：

```cpp
auto parsed = file->loadTrack();
CHECK(parsed.record.metadata.title == "Test Title");
```

改成：

```cpp
auto builder = file->loadTrack();
auto record = builder.record();
CHECK(record.metadata.title == "Test Title");
```

### cover art 测试怎么改

旧测试：

```cpp
CHECK(!parsed.embeddedCoverArt.empty());
```

新测试不要再检查 parser 暴露的 span，因为该类型已经不存在。

推荐做法：

1. 新建临时 LMDB 环境。
2. 调用 `builder.serialize(...)`。
3. 再检查 `builder.record().metadata.coverArtId > 0`。

示例：

```cpp
auto builder = file->loadTrack();

auto temp = TempDir{};
auto env = Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
auto wtxn = WriteTransaction{env};
auto dict = rs::core::DictionaryStore{rs::lmdb::Database{wtxn, "dict"}, wtxn};
auto resources = rs::core::ResourceStore{rs::lmdb::Database{wtxn, "resources"}};

auto [hotData, coldData] = builder.serialize(wtxn, dict, resources);
CHECK(!hotData.empty());
CHECK(!coldData.empty());
CHECK(builder.record().metadata.coverArtId > 0);
```

### 本步验证命令

```bash
rg -n "ParsedTrack|parsed\.record|embeddedCoverArt" test/integration/tag
```

### 本步完成标准

`test/integration/tag` 下不应再出现 `ParsedTrack` 相关代码。

---

## Step 8: 全量编译和测试

### 本步目标

确认改动真的能编译、链接，并且关键测试通过。

### 必须执行的命令

按仓库要求，在 `nix-shell` 里执行：

```bash
nix-shell --run "cmake --preset linux-debug"
```

```bash
nix-shell --run "cmake --build /tmp/build --parallel"
```

### 建议执行的测试命令

```bash
nix-shell --run "ctest --test-dir /tmp/build --output-on-failure -R 'TagTest|TrackBuilderTest'"
```

如果 `TagCommand` / `InitCommand` / `TrackCommand` 有集成用例，也把它们补跑。

### 本步完成标准

1. Debug build 成功。
2. `TagTest` 通过。
3. `TrackBuilderTest` 通过。
4. 不出现因生命周期导致的崩溃或 use-after-free。

---

## 额外检查清单

完成全部步骤后，再跑下面的搜索，确保没有漏网之鱼。

### 搜索旧类型

```bash
rg -n "\bParsedTrack\b|ParsedTrack.h" include src app tool test
```

预期：无结果。

### 搜索旧字段路径

```bash
rg -n "parsed\.record|embeddedCoverArt" include src app tool test
```

预期：无结果。

### 搜索可疑的危险返回

```bash
rg -n "TrackBuilder loadTrackRecord|return .*TrackBuilder" tool
```

预期：不要再有“helper 返回借用 `tagFile` 生命周期的 `TrackBuilder`”这种模式。

---

## 常见错误与禁止事项

### 禁止事项 1

不要把 `TrackBuilder` 返回出一个已经销毁了 `tagFile` 的函数。

错误示例：

```cpp
rs::core::TrackBuilder loadTrackRecord(...)
{
  auto tagFile = rs::tag::File::open(path);
  auto builder = tagFile->loadTrack();
  return builder; // 错误：tagFile 即将析构
}
```

### 禁止事项 2

不要在 MP3 转码路径里把 `view.text()` 的临时返回值直接转成悬空 view。

错误示例：

```cpp
auto text = view.text();
builder.metadata().title(text); // 如果 text 立刻销毁，这个 view 就悬空
```

正确示例：

```cpp
builder.metadata().title(stashString(view.text()));
```

### 禁止事项 3

不要继续手工提交 embedded cover art 到 `ResourceStore`，除非你明确重写了 `TrackBuilder` 的既有机制。

正确方式应当是：

```cpp
builder.metadata().coverArtData(span);
auto [hotData, coldData] = builder.serialize(txn, dict, resources);
```

### 禁止事项 4

不要手改 gperf 生成产物。只改 `.gperf` 源文件和正常的 `.cpp/.h` 文件。

---

## 建议提交拆分

为了降低回滚成本，建议拆成下面几次提交：

1. `tag: change loadTrack api to return TrackBuilder`
2. `tag/flac: build TrackBuilder directly from mmap views`
3. `tag/mp4: remove intermediate string copies`
4. `tag/mpeg: store transcoded strings in File deque`
5. `app/tool/test: update callers and tests for builder-based tag loading`

如果必须减少提交数，可以合并成 3 个：

1. API + FLAC
2. MP4 + MP3
3. callers + tests

---

## 最终验收标准

全部工作完成后，必须同时满足以下条件：

1. 代码库中不再存在 `ParsedTrack`。
2. `tag::File::loadTrack()` 统一返回 `TrackBuilder`。
3. FLAC 和 MP4 文本路径默认直接借用 `mmap`。
4. MP3 转码文本由 `tag::File::_ownedStrings` 的 `std::deque<std::string>` 持有。
5. 调用方不再手工处理 embedded cover art。
6. 不再有任何 helper 返回一个脱离 `tagFile` 生命周期的 `TrackBuilder`。
7. Debug build 成功。
8. `TagTest` 与 `TrackBuilderTest` 通过。

如果这 8 条里有任意一条不满足，就不要宣布任务完成。
