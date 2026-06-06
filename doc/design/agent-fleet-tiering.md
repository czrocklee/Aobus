# Aobus:能力分层工作流与跨 Harness Model 路由设计

> 状态:设计草案 v13 / 讨论用。日期:2026-06-06。
> 范围:**通用、不绑定任何单一 harness 或厂商**。把工作流阶段抽象成能力等级,再把异构 agent 舰队
> (Opus / GPT-5.5 / Gemini 3 Pro·Flash / DeepSeek V4 Pro·Flash / …)按等级路由。
>
> v6 变更:Step D 的"候选并行 + 确定性排序"从设计落为代码——`ROUTE_C1_CANDIDATES` 候选集 + `agent_rank_candidates`
> (文件少→churn 小→id 稳定排序)+ `lint_phase.sh` 每轮 fan-out/guard/rank/验证 top-K;新增 `AOBUS_LINT_TIDY`/
> `AOBUS_AGENT_REPO` 测试 seam 与离线确定性 e2e `run_lint_fanout_test.sh`(见 §9)。
>
> v7 变更:C1 fan-out 拿到**真正的跨厂商第二候选**——Google **Gemini 3.5 Flash via `agy` CLI**(替掉死掉的
> `gemini -p` 路径),实测 9/9 清零、0 silent-wrong。过程中坐实 §10.3 的"隔离 ≠ 沙箱":agy 在 steam-run 下
> 可达真实仓库树(曾按仓库相对路径"逃逸"去改真文件),用"sandbox-unique 扁平路径 + canary"缓解(见 §9 agy 块、§10.3)。
>
> v8 变更:把跨厂商 fan-out 从默认改回 **opt-in**。`ROUTE_C1_CANDIDATES` 默认只含单 worker(ds4f via opencode):
> 零 fan-out、最低延迟、不触发 `steam-run`/agy 冷启动、也不背 §10.3 的软隔离权衡。多候选(加 `route_c1_worker_gflash`
> 跨厂商,或 `route_c1_worker_pro` 同厂商加强)是 routing.env 里注释好的一行 opt-in;运行器与 Phase Contract 不变,
> 仍只验证排序后的 top-K。理由:把"是否值得为更高一轮清零率付延迟 + 软隔离成本"留给操作者按场景决定,默认走最稳最快的路。
>
> v9 变更:补齐 §5.3 的 **per-arg enum/type 契约**(Step C 收尾)——`validation.env` 给每个 validation 声明
> `VALIDATION_ARGSPEC[id]="<type> <min> <max>"`,`agent_validation_args_ok` 在 runner 之前按 arity + 每参类型拒掉
> 类型/个数不符的 packet;dispatch 与 test_phase 均接线。修一处真 bug:两 env 由 loader 函数 source,数组须 `declare -gA`
> 否则函数局部丢失。另把 dispatcher/commit_flow/test_phase 的离线回归补全(dispatch/test_phase/commit_flow 三套新套件),
> 并把 C1 worker 标签收进 `ROUTE_C1_LABELS` 映射(fan-out 日志显示模型名)。离线套件 5 套共 **136** 断言。
>
> v10 变更:充实路由表 roster——C2 加**跨厂商备选 Gemini 3.1 Pro (high) via agy**(真 worker,非纯文档),用
> `ROUTE_C2_WORKER` 选择器在当时默认 GPT-5.5 与备选间一行切换(C2 是单 worker + 反馈轮,不像 C1 fan-out,故用选择器而非
> 候选数组);agy 调用抽成共享 `_route_agy_edit`(C1 gflash 与 C2 gpro 共用),并修 `test_phase` 未导出 `AGENT_REL`
> 的契约缺口(agy worker 按它 stage)。C3 加文档化备选 **GPT-5.5 (high) via codex**(C3 不自动派发,仅 roster)。
> 离线套件 5 套共 **139** 断言。
>
> v11 变更:C3 从"纯文档 roster"升级为**可主动召集的议事会(council,§11)**。新增 `council.sh`(C0 编排)+ routing.env
> 的 `ROUTE_C3_MEMBERS`(四个**只读**成员:Claude Opus via claude、GPT-5.5 high via codex、Gemini 3 Pro via gemini、
> DeepSeek V4 Pro via opencode)+ common.sh `agent_tree_hash`。协议四轮:R1 盲 draft → R2 相互 challenge → R3
> 自我 revise → R4 chair(当前 in-loop agent)验证并综合。council.sh 只做 C0 管线(fan-out / 收集 / 只读 canary / 拼 dossier),**不做判断、也不做
> R4 综合**;成员只读、产出**意见**而非 patch,各自在一份可弃的仓库副本里跑,前后内容哈希抓出任何改树的成员并丢弃+标记。
> plan 与 review 两入口都接(`run-council` skill + diagnose-issue 注脚)。**自审固化**:用本机制对它自身跑了一次真实
> review council(chair=in-loop agent + GPT-5.5 high via codex + Gemini 3 Pro via gemini),跨 challenge 抓出 `printf '--'` 解析、
> `run_one` 漏判退出码、R3 自评注入、R2 违规未隔离、OUT-in-repo 破盲等真 bug,逐条修复并补测;成员 prompt 改走 stdin 以避开
> ARG_MAX。离线套件 `run_council_test.sh`(63 断言);全部六套共 **208** 断言。
>
> v12 变更:C2 暂不扩成 production implementation,先把现有 test phase 硬化成**单文件、已注册 Catch2 测试
> augment**。新增/收紧 C0 gate:request packet strict schema(`kind: request`)、positive path classifier +
> registered-test 检查、baseline filtered test pass、`target_anchor` baseline-absence + Catch2 list-output
> source binding、assertion-count delta risk marker、C2 keep review dossier、audit log、`record_review.sh`。
> `write-unit-test/C2` 正式使用同一 `test_phase.sh` contract;
> `diagnose-issue`/`develop-lint-checker` 只记录未来 body-fill/checker-phase 条件,当前仍 C3-only。C2 fan-out
> 继续禁用:单 worker + feedback rounds,passing test 只表示 review-ready,不表示 commit-ready。离线六套 agent
> integration tests 共 **259** 断言。
>
> v13 变更:C2 默认 worker 从 GPT-5.5/Codex 切到 **Gemini 3.1 Pro (high) via agy**。保留 Codex 为
> `route_c2_worker_codex` 备选;`route_c2_worker` fallback alias 也指向 Gemini,所以旧 runner 在 selector 缺失时仍使用
> 新默认。C2 复用 agy 的 HOME-backed staging + flat-path contract;`GEMINI_C2_MODEL` 是当前 override 名,旧
> `AGY_C2_MODEL` 仍兼容。离线六套 agent integration tests 共 **262** 断言。
>
> v4 变更:① 把"低成本模型 token 成本近似可忽略"和"`run-clang-tidy.sh --fix` 禁用"列为事实前提,
> 不再作为开放问题讨论;② C1 路由允许多低成本模型并行产候选 patch,只把 guard 后最优候选送入慢验证;
> ③ rollback 从 `git restore` 改为"scope clean check + 反向应用本次 patch";④ eval 改为小样本 0 silent-wrong
> gate + 滚动统计;⑤ skill 发现不强依赖 harness 自动读取 `.agents/skills/`,packet 可携带展开后的 contract。
>
> v5 变更:① 候选排序拆成"确定性优先 + frontier 兜底",并定义"按排名验证至多 K 个再 escalate";
> ② 补"目标文件需先 clean"运营约束 + out-of-tree build 利好;③ eval 加 rule-of-three 说明 + 生产首次
> silent-wrong 熔断;④ 把候选并行的速率/延迟纳入"真正成本";⑤ packet 内 contract 由源 skill 生成 + hash 戳。

## 0. 事实前提(非讨论项)

- **低成本模型 token 成本近似可忽略**。本设计的主要成本不是 Gemini Flash / DeepSeek Flash 之类模型的
  token,而是慢 validation 时间、frontier 审 patch/review 的注意力、状态管理复杂度、静默错修风险,
  以及候选并行带来的各 CLI 速率/并发/延迟约束(多候选 fan-out 会先撞 rate limit,而不是 token 预算)。
- **`run-clang-tidy.sh --fix` 不进入自动化路径**。实践已经证明它会损坏文件或生成不可接受的机械改动;
  v1 只使用 tidy 诊断作为输入,不使用自动 replacement 作为 C0 修复手段。

## 1. 背景:可移植的底座已经存在

Aobus 仓库**已经在走 harness 中立路线**,设计要在此之上扩展而非另起炉灶:
- **`.agents/skills/`**:vendor-neutral 的 skill 目录(不是 `.claude/skills/`)。现有 6 个 skill:
  `develop-lint-checker`、`diagnose-issue`、`improve-test-coverage`、`manage-git-flow`、
  `use-clang-tidy`、`write-unit-test`。
- **单一指令源 + 多 harness 命名**:`CLAUDE.md` 和 `GEMINI.md` 都是指向 `AGENTS.md` 的符号链接。
  一份规范,多个 harness 各按自己的约定名去读。
- **舰队已落盘**:`~/.claude`(Opus)、`~/.codex`(GPT-5.5)、`~/.gemini` + Antigravity
  (Gemini 3 Pro/Flash)、`~/.config/opencode`(DeepSeek V4)。

**问题**:今天所有阶段——"补一个 clang-format 空行"到"判断该用 `ao::Result` 还是 `std::optional`"
——都跑在同一个 frontier loop 里。机械苦工稀释了 frontier 的 token/loop;同时舰队里更便宜或更
合适的 model(Flash 系、DeepSeek)闲置。

**目标**:把阶段抽象成**能力等级(capability class)**,让每一级由最划算的执行体承担——可能是
零成本脚本,可能是 Gemini Flash,可能是 Opus——且整套机制**任何 harness 都能参与**。

## 2. 两条核心原则

**原则 A:Tier 是能力等级,不是某个 model。**
按"工作*要求*什么能力"分级,再用一张**路由表**把舰队里的具体 (harness, model) 填进去。换 model、
加厂商,只改路由表,不改 skill、不改工作流。

**原则 B:Skill 是可移植的契约,编排在 harness 之上。**
单个 CLI 的 subagent 机制(如 Claude Code 的 `Agent` 工具)**只能拉起本厂商的 model**,无法跨厂商
路由。所以编排不能依赖任何一家的 tool API,必须落在**所有 harness 的最小公约数**上:
- 触发:每个 CLI 的**非交互调用**(`claude -p` / `codex exec` / `gemini -p` / `opencode run`);
- 交接:**YAML 头 + markdown 正文**的 phase packet;
- 验收:**allowlist validation**(固定命令 ID + 枚举参数 + 退出码),在**一棵已配置的树**里执行
  (见 §5「为什么验证不能在隔离 worktree 里跑」),不依赖任何 harness 的内建判定。

## 3. 能力等级(vendor-neutral)

按三个判据定级:**可否脚本判对错(确定性)/ 决策需要的上下文范围(局部 vs 全局)/ 错判的语义风险**。

| 等级 | 能力要求 | 典型工作 | 现有 skill |
|---|---|---|---|
| **C0 机械·确定性** | 无需推理,脚本可判对错 | format、跑 lint、跑测试、构建、**diff guard、validation 执行、dispatch 路由查表** | `manage-git-flow` 的*执行*面、`build.sh`、`run-clang-tidy.sh` |
| **C1 有界·机械** | 局部改写,有确定性验收 gate 兜底 | 按报错清单修 tidy 残差、铺测试样板/fixture、生成脚手架 | `use-clang-tidy`(修)、`develop-lint-checker`(脚手架)、测试样板 |
| **C2 有范围·实现** | 在既定设计内写代码/测试,要能力不要新颖推理 | 实现一个已定方案、补一组边界用例 | `write-unit-test`/`improve-test-coverage` 的实现面 |
| **C3 前沿·推理** | 全局/语义/架构判断,错判代价高 | plan、root-cause diagnose、design review、**签名/ABI 与语义等价审查**、错误契约选择(§5) | `diagnose-issue`、`code-review`、plan、`develop-lint-checker` 的 matcher 设计 |

你最初的例子落位:"跑 lint" = **C0**,"修 lint 报错" = **C1**,"review/plan/diagnose" = **C3**。
把"执行"和"修复/判断"切开,是这套模型的主要收益点。

> **注意 C0 包含 dispatch / guard / validation 的*执行*。** 它们是确定性逻辑,只是 v1 里**由 frontier
> 顺手执行**(见 §6),不代表它们是 C3 工作。这也说明把 dispatcher 抽出 frontier(Step E)不是"降级",
> 只是把 C0 逻辑挪出 reasoning loop。
>
> **C2 是最未验证的一层**:它与 C1(局部机械)、C3(推理)的边界最模糊,deterministic guard 很难裁定。
> C1 跑通前不要投入 C2,它很可能塌缩成"大 scope 的 C1"或"窄 scope 的 C3"。
>
> **C3 既可单跑也可召集 council(§11)**:plan/review 既能由 chair(当前 in-loop frontier agent)独自产出,也能召集
> 跨厂商议事会(draft → challenge → revise → chair 综合)。council 是 opt-in、贵(真 frontier token × 成员 × 轮次),
> 只用于高风险判断;它**没有确定性 gate**,这正是 C3 与 C1/C2 的根本区别。

## 4. 舰队路由表(把能力等级映射到具体 model)

每级给**首选 + 备选**(跨厂商),按 成本 / 能力 / 延迟 / 上下文窗口 / 工具可用性 取舍。示例:

| 等级 | 首选 | 备选 | 选择理由 |
|---|---|---|---|
| C0 | 显式脚本(0 token) | — | 确定性的不该烧任何 model;v1 不做 always-on hook |
| C1 | Gemini 3 Flash + DeepSeek V4 Flash 多候选 | Haiku / 其他低成本 fast model | token 成本近似可忽略;用多候选提高修对率,但只验证 guard 后最优 patch |
| C2 | Gemini 3.1 Pro via agy | GPT-5.5(Codex) / Sonnet | 强 coding、能照设计落地 |
| C3 | chair(当前 in-loop agent) | Claude Opus / GPT-5.5(high)/ Gemini 3 Pro / DeepSeek V4 Pro | 深推理、架构与语义判断 |

> 这张表已**外置为 `script/agent/routing.env`**(可委托能力用 `route_<class>_worker` / `route_c3_member_*`
> 函数和对应 label 映射;C3 chair 是当前 in-loop agent,身份不进路由表):换 model / 加厂商只改这一个文件,runner 与 phase contract 不动。测试时把
> `AOBUS_ROUTING_ENV` 指向一个 mock(只替换 `route_c1_worker`)即可,这也正是"换路由表、其余不变"的验证。
> 这张表是**唯一需要随舰队变动维护**的东西。skill 与 phase contract 不动。C1 的默认形态不是
> "首选失败再备选",而是**可并行生成多个候选 patch**;昂贵的部分是后续 validation 和 C3 review,
> 因此 dispatcher/frontier 必须先用 deterministic guard 和 patch ranking 过滤,不要给每个候选都跑慢验证。

> **实测 headless 调用(Step 0,§9)**:`claude -p` / `codex exec -s read-only` / `gemini -p --approval-mode plan` /
> `opencode run -m opencode-go/deepseek-v4-flash`。opencode 默认是本地 `ollama/gemma4:31b`,**必须显式钉云端模型**;
> Pro 档为 `opencode-go/deepseek-v4-pro`。

> **C3 备选不再只是文档**:routing.env 的 `ROUTE_C3_MEMBERS` 把可调用模型落成**只读 council 成员**
> (claude / codex / gemini / opencode 各一,§11),各自跑在自己厂商的只读 headless 模式。Claude/Opus 若参与
> R1/R2/R3 也是普通 member;chair 不 draft,只在 R4 验证并综合。换厂商 / 换模型只改 routing.env,council.sh 与 phase contract 不动。
>
> ⚠️ **隔离限制**:虽然成员的仓库副本通过 `agent_tree_hash` 实现了内容隔离,但所有成员共享同一个议事会产出目录
> (`$AGENT_COUNCIL_OUT`)。R1 的"盲起草"隔离目前依赖于成员模型不主动探测其他文件以及 CLI 工具的调用边界,而非硬性的
> 文件系统隔离。

## 5. Phase Contract(harness-agnostic 阶段契约)

每个可委托 skill 加一段统一头,使它能被**任意 harness 的任意 model**当独立阶段执行:

```
## Phase Contract
- Capability:    C0 | C1 | C2 | C3
- Inputs:        最小输入(diff / 文件清单 / 失败日志 / 设计链接)
- Scope:         允许动什么、禁止动什么(文件清单)
- Validation:    allowlist 中的 validation_id + 枚举参数,退出码即判据
- Output:        patch + 结构化结果(YAML/JSON:status / changed_files /
                 validation / escalate_reason / residual_risks) + markdown 说明
- Escalate when: 命中即停并上交更高等级
                 (需改公共 API / 需选错误契约 / 需改架构 / 验收反复不过)
```

### 5.1 安全地基:patch + guard + 时间隔离

廉价 model **绝不直接改主 worktree**,只在**隔离 scratch/worktree** 里改文件;**patch 由 dispatcher 用
`git diff`(scratch vs base)生成,不采信 model 自写的 unified diff**(§9 Step 0 实测:codex 自写 diff 的
hunk header 损坏、`git apply` 报 corrupt;让 harness 生成可消除整类失败)。**鲁棒做法(Step B 实测确定)**:worker 在一个**隔离 sandbox cwd**
(只放目标文件的副本)里**就地编辑副本**,dispatcher 直接 `diff` 该副本——既不采信 model 自写 diff
(codex/ds4f 的 hunk 常损坏),也不依赖 sentinel stdout(agentic CLI 会叙述、或改去 Read/Edit 文件而非回显;
gemini 把"我的修复计划……"写进文件、ds4f 在隔离 cwd 下试图 Read 真实路径被自身权限拦成 0 输出——都实测到了)。
sandbox + CLI 自身的 external-dir 权限拦截**双重**保证 worker 碰不到真实树。安全闸门按下面顺序卡:

1. **Deterministic guard(apply 前,跑在 diff 上,不需要 build)** —— 见 §5.2;过滤所有越界候选;
2. **候选排序(先确定性,后 frontier 兜底)**:对通过 guard 的候选,先用**确定性代理**排序
   (churn 最小、触及文件最少、最贴合 scope);**仅当多个候选确定性维度并列、难分时**,才由 frontier 做
   语义排序(最小意图、report 质量)。能确定性排就不烧 frontier 注意力;
3. **取仓库锁**(同一时刻只允许一个 build 类阶段动主树);
4. **scope clean check**:patch 触碰的路径在主树中不能有预先存在的 staged/unstaged/untracked 改动;
5. **把排名最高的候选 apply 到主树**(主树有已配置的 `/tmp/build/...`),记录本次 patch 作为唯一 rollback 单元;
6. **跑 Validation**(allowlist,§5.3),退出码即判据;
7. **通过 → 留下**(交 C3 review);**失败 → 反向应用本次 patch 回滚,按排名取下一个候选重试,至多验证 K 个
   (K 小,如 2,给分钟级慢验证封顶);K 个皆败或命中 escalate → 升级 C3**;
8. **释放锁、清理临时 worktree(`git worktree remove`)**。

> **禁止裸 `git restore` rollback**。主 worktree 可能已有用户改动;`git restore` 会误删不属于该 phase 的状态。
> v1 要么要求 patch scope 在 apply 前干净,要么拒绝执行;失败时只用 `git apply -R` 反向应用**本次 patch**。
> 如果反向应用失败,立即停下交 C3/人工处理,不能扩大回滚范围。

> **运营约束:目标文件需先 clean/committed。** scope clean check 会(正确地)拒绝触碰用户 WIP 文件
> ——本仓库当前就有十几个未提交的 M 文件,所以 build 依赖的委托阶段在与 WIP 重叠时会直接被拒。这是安全
> 特性而非 bug;跑 pilot 前先把目标文件 commit 或 stash。**利好**:本仓库 build 是 out-of-tree
> (`binaryDir: /tmp/build`),validation 的构建产物不落源码树,因此 `git apply -R` 反向回滚是干净的。

> **为什么验证不能在隔离 worktree 里跑(已核实)**:`run-clang-tidy.sh` 把 `BUILD_DIR` 固定为
> `/tmp/build/debug-clang-tidy`,其 `compile_commands.json` 里全是**指向主源码树绝对路径**的条目。
> 在另一路径的 worktree 里跑 tidy,会照 compile_commands 去分析**主树未修改的文件**,无视 worktree 改动
> ——结果是**静默 green(假通过)**,比报错更危险。build/test 同样依赖一棵已 configure 的树。
> 因此:**模型在哪改(可在 scratch/worktree 产 patch)与验证在哪跑(必须在已配置的主树)解耦。**
> 隔离靠的是**时间**(apply→验证→留/弃,加锁保证可回滚),不是**空间**(独立 worktree)。
> 空间 worktree 隔离只对 *build-independent* 的验证(纯格式检查、grep 类)才成立。

### 5.2 Diff guard:确定性部分 vs C3 review 部分(必须分开)

**Deterministic guard(C0,跑在 diff 上,路径 + 体量判定,v1 就做)** —— 命中即拒绝并 escalate:
- 改动触碰 `include/**`(public header 目录);
- 改动任何 `*/CMakeLists.txt`、CMake 配置、`.clang-tidy`、lint 配置或 `script/**`;
- 改动 `doc/design/**`;
- 出现 packet `scope.files` 之外的文件变更;
- churn 超阈值:改动行数 / 删除行数 / 触及文件数超过 packet 设定上限(代理"大规模重排")。

**C3 review checklist(语义判断,diff 判不了,交 frontier,别挂在 deterministic guard 名下)**:
- 是否改变了函数/类型签名或 ABI;
- 是否引入用户可见行为变化;
- 改动是否语义等价于"修掉这条诊断"这一最小意图。

**铁律(C1/C2)**:只在 scope 内改 → 只产 patch/report → 过 deterministic guard → 加锁 apply 到主树
→ 过 allowlist validation 且退出码 0 → 交 C3 review → 才算落定。命中 Escalate 立即停、不自作主张。
自报 `escalate_reason` 仍保留,但只是**额外信号**;真正的闸门是 deterministic guard + validation + C3 review。

### 5.3 Validation allowlist

packet 不能携带任意 shell 字符串,只能引用固定 ID;**参数也必须枚举或结构化校验**,
例如 `validation_id: clang_tidy_folder` 的 `folder` 只能从 `{lib, app, include, test, lint}` 取值,
`clang_tidy_files` 的 `files` 必须落在 repo 内且属于 packet scope。这样注入面不会从命令字符串转移到参数字符串。

> **已落地(Step C per-arg 契约,2026-06-05)**:`validation.env` 给每个 `v_<id>` 声明 `VALIDATION_ARGSPEC[id]="<type> <min> <max>"`
> (`tidy`=`path 1 -`、`test-core`/`test-gtk`=`filter 1 1`、`build-debug`=`any 0 -`)。`agent_validation_args_ok` 在跑慢验证
> **之前**按 arity + 每参类型(`agent_argtype_re`:`path`/`filter`/`any`,enum 即字面值 alternation)拒掉**类型/个数不符**的 packet。
> dispatch 与 test_phase 都在路由 runner 前先过这道门;未声明 spec 的 id 回退到仅 `agent_arg_safe` charset 门(向后兼容、mock 友好)。
> 对 `use-clang-tidy/C1`,即使 packet 带 `validation_args`,它也必须与 `inputs` 文件集完全一致;否则就是验证了错误 scope,
> dispatcher 在 runner 前拒绝。`v_tidy` 还必须保留 `run-clang-tidy.sh` 的非零退出状态:工具/配置失败不是 clean gate。
> 实现陷阱:两 env 由 loader 函数 `source`,故数组用 `declare -gA`——函数内 `declare -A` 会变函数局部、返回即丢失(plain 赋值与函数定义不受影响)。

> **validation 不是廉价退出码**:`run-clang-tidy.sh` 要建 plugin + 进 nix-shell + 分析,是**分钟级**;
> `build.sh` 更久。每次 escalate-retry 都要重跑这条慢验证——成本模型(§10)必须把它算进去。

## 6. 编排层:v1 先由 frontier 兼任 Dispatcher

跨厂商无法用任一 CLI 的内建 subagent,因此用两件中立原语。

**Phase Packet(交接单,YAML 头 + markdown 正文)**:
```
---
skill: .agents/skills/use-clang-tidy
capability: C1
scope:
  files:
    - lib/audio/Foo.cpp
  max_changed_lines: 80
inputs:
  tidy_log: /tmp/aobus-phase/<id>/in/tidy.log
validation:
  id: clang_tidy_files
  files:
    - lib/audio/Foo.cpp
artifacts:
  in: /tmp/aobus-phase/<id>/in/
  out: /tmp/aobus-phase/<id>/out/     # 模型把 patch + report 写这里
escalate_to: C3
---

Markdown body:背景、诊断摘录、允许/禁止事项、期望 done signal。
```

**v1 编排方式**:frontier 主 loop 直接扮演 dispatcher,不先写独立程序。注意:**dispatch 路由查表、guard、
validation 本身是 C0 确定性逻辑**,这里只是由 frontier 顺手执行,不是 C3 工作:
1. 生成 packet 和输入 artifacts(scratch/worktree 仅作模型工作区,产 patch,不在此验证);
2. frontier 查路由表(§4),用一个或多个低成本目标 harness 的非交互模式调用:
   `claude -p` / `codex exec` / `gemini -p` / `opencode run`(各家 flag 名不同,路由表里记);
3. 廉价 model 产出一个或多个 **patch + 结构化 report**(不碰主树);
4. frontier 跑 **deterministic guard(C0)** on diff,过滤越界候选,再按最小 patch / 最小 churn / report 质量排序;
5. 加锁 → 确认 patch scope 干净 → apply 最优候选到主树 → 跑 **allowlist validation(C0)**;
6. 失败即反向应用本次 patch 回滚;通过 → frontier 做 **C3 review**(签名/行为/语义等价)→ 落定或 escalate;
7. 释放锁、清理临时 worktree。

这样 v1 仍然 **harness 无关**,但不先承担独立 dispatcher、并发执行、队列、锁框架、失败恢复等基础设施成本。
独立 dispatcher 是第二阶段需求:当流程稳定、需要无人值守或并发运行时再抽出来。

**示例:提交前流程(commit-flow)** —— 每行标注的是**该步需要的能力**,v1 全部由 frontier 编排执行:
```
C0  脚本        显式运行 clang-format / build / test                    [0 token]
C0  脚本        run-clang-tidy.sh(改动文件,仅诊断,不 --fix)→ 报错清单  [0 token]
C1  Flash*      scratch 中并行产多个 tidy 修复 patch/report(不碰主树)
C0  脚本/frontier guard + ranking → 加锁 → scope clean → apply 一个候选 → validation → 失败反向 patch
C3  chair       签名/行为/语义等价 review + 写 commit message
```

## 7. 现有 skill 的等级归属与改造

- **manage-git-flow**:把 format / validation 的*执行*下沉为显式 C0 脚本;v1 不做 hook。skill 退化为编排 +
  commit 规范(仍 BLOCKING)。**已接线(2026-06-05)**:skill §3「Mechanical Pre-Commit Delegation」把机械前半
  (C0 format + C1 lint via `commit_flow.sh` → `dispatch.sh`)显式委托出去,skill 自己留作 **C3**——commit 决定 /
  message / 语义 review。接线遵守两条硬约束:① commit_flow 会跑 clang-tidy,故是 **opt-in**(仅当本次 commit 明确
  含 lint 清理时用,默认 commit 路径不跑);② commit_flow **绝不 commit/stage**,READY→继续 Commit Procedure、
  NEEDS C3→先处理 escalation packet / guarded 路径。
- **use-clang-tidy**:C1;加 Phase Contract,Escalate 覆盖"改公共 API / 选错误契约 / 跨文件重构"。
  **v1 绝不使用 `run-clang-tidy.sh --fix`(见 §10,已证实会搞烂文件)**。C0 只负责跑 tidy 产出诊断清单;
  C1 只处理这些诊断中**明确、局部、可验证**的修复,且一律走 patch + guard + validation 流程。
  **两条 Step B 实测铁律**:(i)**迭代到 fixpoint**——修一轮可能引出新 warning(实测:修 magic-`7`→`constexpr addend`
  →又触发 `addend` 须为 `kAddend`),必须 修→复跑 tidy→把新诊断喂回→再修,直到 0 warning 或预算/无进展才停;
  (ii)**进程隔离**——agentic CLI(opencode `run`)会直接 Read/Edit 工作树文件,故 worker 必须在**非仓库的隔离 cwd**
  跑、patch 只取 sentinel stdout,真实树只由 dispatcher 在时间隔离下改。
- **write-unit-test / improve-test-coverage**:C3 决定*测什么/边界* + C1/C2 铺样板与 fixture。
- **diagnose-issue / code-review**:C3;补清晰 Inputs(可复现命令 / 失败日志 / 范围)。
- **develop-lint-checker**:C3 设计 matcher 逻辑 + C1 生成脚手架/fixture/CMake 接线。

## 8. Skill 跨 harness 可移植性

各 harness 发现 skill 的方式不同(Claude 读 SKILL.md frontmatter;Codex/Gemini 主要读 AGENTS.md;
OpenCode 另有约定)。v1 不把"自动发现 `.agents/skills/`"当硬依赖:
- skill body 用**纯 markdown + 标准 Phase Contract 头**,不掺任一 harness 专有语法;
- packet 可以携带**展开后的 skill contract 摘要**作为主路径,确保任何能读 prompt/文件的 headless CLI 都能执行;
  该摘要**在 dispatch 时从源 skill `.agents/skills/<name>` 生成并打 version/hash 戳**,而非手抄——单一真相源
  仍是源 skill,避免 packet 副本漂移;副作用利好:自带 contract 让廉价 model 不必有仓库级 skill 读权限,
  缩小委托面、利于 sandbox;
- 沿用现有 **`AGENTS.md` 符号链接**手法:在 `AGENTS.md` 里挂一节"Skills 索引 + Phase Contract 规范",
  各 harness 经各自的 `*.md` 软链能读到时就作为优化路径;
- harness 专有的触发元数据(如 Claude 的 SKILL.md frontmatter)作为**薄 adapter**,统一指向
  `.agents/skills/<name>`,正文不复制。

## 9. 落地路线(增量、可独立验收/回滚)

0. **Step 0:harness headless 能力探针(v1 前置阻塞,先于 eval)**。逐个确认候选 CLI 能否
   (a)完全无头跑、(b)指向 repo + 工作区、(c)读取 packet 中展开后的 skill contract 或直接读取
   `.agents/skills/`、(d)在不越 scope 下把 patch 写到 `artifacts.out`、(e)鉴权/速率/上下文窗口可用。
   任一候选不满足,就从路由表(§4)对应等级移除。

   **Step 0 实测结果(2026-06-04,`/tmp/agent-fleet-pilot/probe.sh`)**:4 个 CLI 均已安装、headless 可调用,
   实测的非交互入口与 §2 假设一致(claude `-p` / codex `exec` / gemini `-p` / opencode `run`)。
   任务:headless 修一个 `std::endl → '\n'` 并产 patch:
   - **gemini**(默认模型,14s):**PASS** —— diff 干净可 apply、in-scope。
   - **claude**(`-p`,10s):**PASS**。
   - **codex**(`exec -s read-only`,8s):语义正确,但**自写 unified diff 的 hunk header 损坏**
     (`@@ -3,5 +3,5 @@` 行数不符 → `git apply` 报 corrupt);改用"model 写文件 + dispatcher `git diff`"即通过。
     → **设计修正已并入 §5.1:patch 一律由 harness 生成,不采信 model 自写 diff。**
   - **opencode**(默认 `ollama/gemma4:31b` 本地模型 → 180s 挂起、0 输出;**钉 `-m opencode-go/deepseek-v4-flash`
     后 3s 返回**):语义正确,但与 codex 同样**自写 diff 的 hunk header 损坏**(`@@ -4,5` 越界)→ harness-diff
     契约下通过。→ 教训:路由表须为每个 CLI **显式钉云端廉价模型**,默认可能是本地小模型。
   结论:4 个候选在 **harness-diff 契约**下均可进 C1(gemini/claude 自写 diff 也能用;codex/opencode 必须靠
   harness 生成 patch);self-diff 在 **2/4 厂商**上损坏,进一步坐实 §5.1 的"patch 一律 harness 生成"。
1. **Step A:手工 eval(信任边界 gate)**。挑 5–10 条真实 `run-clang-tidy.sh` 诊断,用统一 eval packet 手动喂给
   候选廉价 CLI。低成本模型 token 近似免费,所以 eval 不是证明"值不值得调用",而是证明"能不能进入自动
   C1 路由并减少 frontier 手写量"。记录四个指标并设硬 bar:
   - **修对率 ≥ 80%**(改对且通过 validation);
   - **小样本 silent wrong = 0**——5–10 条 pilot 中不允许出现 validation 通过(tidy clean + build green)
     但语义仍错的案例。注意 0/N 不等于"低"(rule of three:0/10 在 95% 置信下真实率仍可能达 ~26%),
     故小样本 0 只是**准入门槛**,真实风险率由后续滚动统计确立;且滚动统计必须配**熔断**:生产中**首次**
     出现 silent-wrong 即暂停该候选的 C1 路由并复盘,不能只当看板;
   - **escalation 率 ≤ 阈值**——若大半诊断都得 escalate C3,C1 这层不省事;
   - **越 scope 次数 = 0**(被 deterministic guard 拦下不算失败,但应统计模型越界倾向)。
   任一不达标 → 暂停该候选的 C1 路由,不投入 contract/dispatcher 基建。

   **Step A 实测结果(2026-06-04,种入 9 条真实诊断到 `lib/tag/Open.cpp`,harness-diff 契约,2 候选)**:
   - **DeepSeek V4 Flash**(`opencode-go/deepseek-v4-flash`):**9/9 清零、0 silent-wrong、in-scope** —— 且
     **符合 Aobus 习惯**:`int→std::int32_t` 并补 `<cstdint>`、`std::endl→'\n'`、magic `7`→`constexpr kAddend`
     (k 前缀)、把 optional 声明 + `.has_value()` 一次合并成 `if (const auto optX = std::optional<...>{}; optX)`
     (一举清掉 use-if-init / const-correctness / local-init / optional 四条)。→ **C1 可用,首选。**
   - **Gemini 3 Flash**(`gemini-3-flash-preview -p`):**失败,但属契约问题而非能力**。无视"只输出文件"约束,
     先吐了一段"我的修复计划……"并尝试 `read_file` 工具 → 文件被叙述污染、首行 `I have analyzed…` → 编译 error。
   → **设计修正(已并入 §5.1)**:整文件输出必须夹在显式 **sentinel**(`<<<BEGIN_FILE>>>`/`<<<END_FILE>>>`)间、
   harness 只取其间;对 agentic 型 headless CLI(gemini `-p`)还需钉"纯转换、禁叙述/禁工具"的调用形态后复测。
   **净结论**:cheap-model 做 C1 机械修复在 Aobus 上**可行**(DeepSeek Flash 单 case 全清零且地道);
   瓶颈不在模型能力,而在**输出契约的鲁棒性**。下一步:加 sentinel 契约,把样本扩到多个 case 跑滚动 silent-wrong。

   **跨厂商第二候选 = Gemini 3.5 Flash via `agy`(2026-06-05,replaces the dead `gemini -p` path)**:用
   `steam-run agy -p` 而非 `gemini -p`。三个实测要点:
   - **契约干净(probe)**:agy 在 `-p` 模式是 agentic 的(用工具就地改文件),叙述只进 **stdout**,**不污染文件**
     ——正是旧 `gemini -p` 失败的那条(把"我的修复计划……"写进文件)现在没有了。4 个诊断类一次改对、地道。
   - **能力达标(real eval)**:经真实 `lint_phase.sh` + 真实 clang-tidy,种 9 条诊断到 `lib/tag/Open.cpp`,
     **9→0 一轮 FIXPOINT**(int→`std::int32_t`+`<cstdint>`、C-cast→`static_cast`、`std::endl`→`'\n'`、magic
     `7`→`constexpr kAddend`、optional 合进 if-init 且 const)。与 DeepSeek Flash 同分(9/9,0 silent-wrong,
     0 越界)。→ **C1 可用,作为 ds4f 之外的跨厂商第二候选**。注意:它进的是 routing.env 里的**可选**候选行,
     **默认仍是单 worker**(ds4f);跨厂商 fan-out 是 opt-in(见下方 v8 与 §10 的延迟/隔离权衡)。
   - **隔离漏洞 + 缓解(关键)**:见 §10.3。`steam-run` 把整个 `$HOME`(含真实仓库)bind-mount 进去,且给
     agy 一个**私有 /tmp**(故 `/tmp` 下的 AGENT_SANDBOX 对它不可见)。首跑时 agy 按提示里的**仓库相对路径**
     `lib/tag/Open.cpp` **逃逸**去改了真实仓库文件(它偏好"真正的 git 项目"而非 cwd 副本),sandbox 副本没动 →
     harness-diff 反向 → 9→9 无进展。带 canary 的对照实验定位:换**唯一文件名**(仓库里无同路径)后 agy 老老实实
     改 cwd 副本、真实仓库 sha256 不变。**缓解**:agy worker 把目标 stage 成 `$HOME` 下的**扁平唯一名**(不与任何
     仓库相对路径碰撞)、把 prompt 里的路径改写成该名、改完拷回 AGENT_SANDBOX;contract 新增导出 `AGENT_REL`。
     这是**对可信厂商模型的路径防碰撞**,不是硬沙箱;但每次编辑仍过 harness-diff + guard + 独立复验,逃逸编辑无法静默落地。
2. **Step B:`use-clang-tidy` Phase Contract pilot**。eval 过关后加 Phase Contract;**不用 `--fix`**;
   C0 只产诊断,C1 只处理明确、局部、可验证的 tidy 修复。

   **Step B 实测结果(2026-06-04,`/tmp/agent-fleet-pilot/lint_phase.sh`,种 9 诊断到 `lib/tag/Open.cpp`,worker=ds4f)**:
   端到端跑通 C0 诊断 → C1(ds4f,sentinel 输出)→ harness-diff → 确定性 guard → 时间隔离 apply → 复跑 tidy → keep/rollback。
   两条只有真跑才暴露的修正:
   - **迭代到 fixpoint**:v1 单轮把 magic-`7` 修成 `constexpr addend`,又触发 `addend→kAddend` 命名告警(1 残留);
     v2 改成 修→复跑→喂回 的循环,**1 个 fix round 后 0 warning,FIXPOINT**(给"常量 kCamelCase"提示后 ds4f 一轮到位)。
   - **进程隔离(更关键)**:v1 在仓库 cwd 跑 opencode,`worker.err` 显示它 `Read`+`Edit lib/tag/Open.cpp` **直接改了真实文件**
     ——prompt 级"只输出 patch"挡不住 agentic CLI。v2 把 worker 关进 `/tmp` 隔离 cwd、patch 只取 sentinel stdout,真实树
     仅由 dispatcher 在时间隔离下改、失败 rollback;复跑确认 repo 全程干净。
   **落地(Step B 末)**:runner 进 **`script/agent/lint_phase.sh`**(仓库基础设施,非 skill 内容);skill
   `use-clang-tidy/SKILL.md` 加 "Phase Contract — C1 delegation" 段引用它(skill = 可移植契约,runner = 执行机制)。
   worker 机制最终定为 **sandbox 副本就地编辑 + diff 副本**:sentinel-stdout 在隔离 cwd 下被 opencode 的 external-dir
   权限拦成 0 输出而失败,改用 sandbox-diff 后稳定 1 轮收敛到 0 warning。

   **Step C/D 加固实测(2026-06-04,`script/agent/{routing.env,common.sh,lint_phase.sh}`)**:把 pilot runner
   加固成可复用基础设施,落地 Step C/D 的四个具体项:
   - **路由外置**:routing 从 runner 抽到 `routing.env`(`route_c1_worker` = ds4f via opencode);`common.sh`
     提供 `agent_load_routing`/`agent_repo_lock`/`agent_harness_diff`/`agent_emit_packet`/`agent_guard_path`。
   - **仓库锁(Step D)**:`agent_repo_lock` 用 `flock` 串行化所有改树阶段;并发第二个实例在 `AGENT_LOCK_WAIT`
     超时后以 exit 4 退出,杜绝两个 phase 互相覆盖。
   - **多文件 scope**:接收多个文件,或 `--changed` 从 `git status` 推出改动的 C++ 集合;逐文件独立跑 fixpoint
     与 rollback,汇总 kept/escalated,任一 escalate → 进程 exit 2。
   - **C3 交接 packet(Step C 雏形)**:每条 escalation(forbidden 路径 / 无进展 / no-op / churn 超限 / 轮次耗尽)
     都把残留诊断 + 被拒 patch 写成 `escalate/<file>.packet.md` 给 frontier reviewer;真实树先 rollback 再写 packet。
   - **deterministic guard** 扩到含 `.agents/**`;churn/轮次预算不变。
   验证矩阵:① mock-good 单文件 → 9 诊断 1 轮收敛 FIXPOINT exit 0、树复原;② 多文件(允许 + forbidden)→
   1 kept / 1 escalate、packet 落地、forbidden 文件零改动、exit 2;③ mock-noop → no-op 识别 → rollback + escalate;
   ④ `--changed` 干净树 → nothing-to-do exit 0;⑤ 锁竞争 → exit 4;⑥ **真实 ds4f** 经 routing.env 跑通,1 轮清零 exit 0。

   **Step C 落地实测(2026-06-04,`script/agent/{validation.env,dispatch.sh}` + `common.sh` packet/allowlist)**:
   - **机读 packet schema**:Phase Packet 定为 **YAML frontmatter + markdown 正文**(`schema: aobus-phase-packet/v1`;
     字段 `kind/skill/capability/validation/escalate_to/inputs[]`)。`agent_emit_packet` 产出带 frontmatter 的 escalation
     packet;`agent_packet_scalar`/`agent_packet_list` 解析。**入站请求与出站 escalation 共用同一 schema**。
   - **validation allowlist**:`validation.env` 把允许的验证登记成 `v_<id>` 函数(`tidy`/`build-debug`/`test-core`/
     `test-gtk`);packet 的 `validation:` 只能是其中的 **ID**,绝不接受任意 shell 串。`agent_validate <id> [arg...]`
     校验 ID 存在 + 参数安全(`agent_arg_safe`:拒 flag 注入 `-*`、路径穿越 `..`、shell 元字符;放行 `[],:` 以支持
     Catch2 tag),并以**带引号的位置参数**调用——参数从不进入 shell 解析,关闭注入面。
   - **per-arg enum/type 契约(Step C 收尾,2026-06-05)**:allowlist 不止登记 ID,还给每个 validation 声明参数契约
     `VALIDATION_ARGSPEC`(`<type> <min> <max>`)。`agent_validation_args_ok` 在 runner 之前按 arity + 每参类型拒掉
     mistyped/mis-counted packet(把 Catch2 filter 喂给 `tidy`、或把文件路径喂给 `test-core`、参数个数不符),把注入/
     误路由面从 charset 收窄到类型。修一处真 bug:两 env 由 loader 函数 `source`,函数内 `declare -A` 会沦为函数局部、
     返回即丢失 → 改 `declare -gA`(plain 赋值与函数定义不受影响,故此前未暴露)。
   - **thin dispatcher**(§6):`dispatch.sh <packet>` 读 packet → 校验契约(capability 有 runner、validation 在
     allowlist、inputs 安全)→ 路由到 runner(`use-clang-tidy/C1` → `lint_phase.sh`)→ **独立**用 allowlist 复跑 gate
     (不采信 runner 自报)→ keep / escalate。本身是 C0 逻辑,无 model。
   验证矩阵:① packet 解析(scalar+list)正确;② 注入用例 `validation: rm -rf /` → reject、树零改动;③ flag 注入
   input `--all` → runner 前 reject;④ 未注册 `write-unit-test/C2` → escalate;⑤ **真实 ds4f 经 dispatch 端到端**:
   round1 修 9 条但引出新的 include-cleaner 告警 → round2 补 `<cstdint>` → round3 清零 **FIXPOINT(2 轮)**,独立
   `v_tidy` gate 通过 → PASS exit 0(实测印证"一轮修不完"的迭代必要性)。

   **commit-flow 链落地实测(2026-06-04,`script/agent/commit_flow.sh`)**:把 §6 的 commit-flow 串成一个 C0 编排:
   `C0 clang-format(改动 C++)` → `C1 lint phase(对改动集生成 Phase Packet → dispatch.sh,修到 fixpoint)` →
   `C0 dispatch 的独立 tidy gate`。**铁律:commit_flow 绝不提交**——无 `git commit/add/checkout/reset/stash`;
   过关后只打印交接摘要,提交决定 / commit message / 语义 review 留给 C3(`manage-git-flow`)。它不持仓库锁(format
   是快且幂等的;重活的串行化由它调用的 `lint_phase` 自己加锁,避免父子进程同文件锁死)。forbidden 路径(如改到
   `include/**` 头)直接归到 C3-only、不进 C1。实测:① 仅有非 C++ 改动的树 → no-op exit 0;② mock 种 1 个改动 →
   format + 生成 packet + dispatch 修到 fixpoint + gate 过 → **READY FOR C3 exit 0**、文件复原;③ **真实 ds4f 对真改动**:
   format → 2 轮 fixpoint(再次 fix→include-cleaner→补 `<cstdint>`)→ gate 过 → `git status` 仍 `M`(改动保留、已格式化且
   lint-clean、未提交)→ 交 C3。

   **回归覆盖**(六套**离线确定性**套件,共 208 断言,无 model / 无 clang-tidy,均进 CI):
   - `test/integration/agent/run_agent_fleet_test.sh`(63 断言):arg sanitizer、path guard、validation allowlist
     拒绝路径 + id 归一化(hyphen→underscore)、**per-arg 契约**(`agent_argtype_re` 的 path/filter/any 判型 +
     `agent_validation_args_ok` 的 arity/类型,对真实 spec)、harness-diff churn 计数、**候选排序**
     (`agent_rank_candidates` 按"文件少 → churn 小 → id"稳定排序;`agent_patch_files` 计 `+++` 头)、Phase Packet
     schema 的 emit→parse 往返(含 validation_args 与 body)。
   - `test/integration/agent/run_lint_fanout_test.sh`(11 断言):**端到端**跑真实 `lint_phase.sh`,把 worker
     mock 成 `AOBUS_ROUTING_ENV`、慢 tidy mock 成 `AOBUS_LINT_TIDY`、目标树指向 `AOBUS_AGENT_REPO` 下的临时树,
     确定性验证 Step D 的多候选路径:排序选中低 churn 的 surgical 候选(即便它在 fan-out 里**后**启动)而非正确但
     铺张的 rewrite、全 no-op → escalate + packet + 树复原、churn 超限 → escalate、旧 routing(无候选数组)回退单 worker。
   - `test/integration/agent/run_dispatch_test.sh`(22 断言):**端到端**跑真实 `dispatch.sh`(§6),mock 路由 +
     mock allowlist(`AOBUS_VALIDATION_ENV`)+ 临时树。覆盖 PASS 路径与**每条**拒绝/升级分支:非 allowlist 的
     validation(树不动)、缺必填字段(exit 64)、不安全输入路径(traversal)、(skill,capability) 无注册 runner →
     升级、**Step C 的 mistyped-arg 上游拒绝**(filter 喂给 tidy → 在 runner 之前 reject、树不动),以及**关键的独立门**
     ——runner 自报 rc 0 / 保留改动,但 dispatcher 自己的 allowlist 门红 → 仍升级(永不信任 runner 自报);并验证
     `validation_args` 而非 `inputs` 喂给独立门。
   - `test/integration/agent/run_test_phase_test.sh`(34 断言):**端到端**跑真实 `test_phase.sh`(C2),C2 worker
     mock 成 `AOBUS_ROUTING_ENV`(行为由 `C2_MODE` 切换)、validation mock 成 `AOBUS_VALIDATION_ENV`。覆盖单轮通过、
     每条拒绝/升级分支(非 allowlist、缺 inputs、空 plan body、不安全 validation_args、**Step C 的 arg 契约违例**、
     guarded 目标 + packet、目标不存在)、no-op / churn 超限 → 回滚 + 升级、**错误反馈轮**(worker 第一轮失败、
     拿到验证输出后第二轮通过),以及 **`ROUTE_C2_WORKER` 选择器**(默认 vs 备选 worker)。
   - `test/integration/agent/run_commit_flow_test.sh`(15 断言):**端到端**跑真实 `commit_flow.sh`(§6 commit 链),
     在一棵临时 **git** 树上(`clang-format` 用 PATH stub),验证 format→分流 guarded→C1 lint via dispatch→交接。
     关键安全性质:**commit_flow 永不 commit / stage**(即便全程通过,commit 数不变、暂存区为空、改动留在工作区);
     另覆盖无改动→空转、guarded 路径在改动集→NEEDS C3、lintable 无法收敛→C1 升级→NEEDS C3。
   - `test/integration/agent/run_council_test.sh`(49 断言):**端到端**跑真实 `council.sh`(§11),成员 mock 成
     `AOBUS_ROUTING_ENV`(行为由 `COUNCIL_MUTATE` / `COUNCIL_MUTATE_R2` / `COUNCIL_FAIL` / `COUNCIL_SILENT` /
     `COUNCIL_ROSTER` 切换)+ 临时树。覆盖 happy path(四成员 → 全 dossier、quorum ok)、**只读 canary**
     (改了自己副本的成员被丢弃 + 归因到具体成员、其余存活)、**R1 盲**(R1 prompt 无 peer 文本,R2/R3 有)、坏 mode → 拒
     (64)、unsafe input → 拒(3)、单成员 → quorum degraded + 跳过 challenge/revise、静默成员 → 记 absent 并以幸存者继续、
     plan/review 两 mode 选对 prompt 模板;**自审修复后新增**:成员非零退出 → 丢弃不就座、R2 阶段违规 → 整体隔离(连 R1
     draft 都不出)、Late-quarantine 更新 metadata (drafts/quorum);**二轮修复后新增**:
     `validation:` 字段拒绝(64)、OUT-in-repo 拒(3)、全员违规拒(2)、仓库 Staging 失败拒(3)。
   端到端 lint/dispatch/commit/test 链针对**真实 worker** 另行验证(见上);council 的真实 frontier 验证是 opt-in 手动
   smoke(frontier 不进 CI)。

   **C2 test phase 落地实测(2026-06-05,`script/agent/test_phase.sh`,worker=codex/GPT-5.5)**:Step E 的第一个推广。
   - **结构性发现(eval 先行的价值)**:Aobus 测试在 `test/CMakeLists.txt` 里**显式登记**(`add_executable(ao_test …)`,
     无 glob),故**新建测试文件**必须改 CMakeLists(guarded 路径)→ 是 **C3** 工作,不是 C1/C2。可干净委托给 C2 的是
     **在已登记的现有测试文件里增补 case**(校验无需动 CMake)。
   - **C2 eval**:给 codex 一个**上游已定的测试计划**(向 `Base64Test.cpp` 补一个覆盖 Base64 字母表 `+`/`/`(62/63)的
     SECTION),sandbox 隔离 + harness-diff,apply 后 `run-tests.sh --core [base64]` 真编译真跑:**一轮通过、in-scope
     (仅 +7 行)、零越界文件、风格地道**(27 assertions all passed)。
   - **runner + 编排**:`test_phase.sh` 是 **packet 驱动**的 C2 runner(比 lint 富:`inputs[0]`=现有测试文件、
     `validation`+`validation_args`=Catch2 filter、body=测试计划);迭代信号是"build+run 通过",失败把编译/测试输出喂回
     worker(round budget)。`dispatch.sh` 加路由 `improve-test-coverage/C2 → test_phase`,并把独立 gate 泛化为"有
     `validation_args` 用之、否则用 inputs"。`improve-test-coverage/SKILL.md` 加 C2 Phase Contract。
   - **端到端实测**:`dispatch <packet>` → test_phase → codex 写 SECTION → 内层 `v_test_core [base64]` build+run 通过 →
     独立 gate 复跑通过 → PASS exit 0、文件保留为可 review 改动。**链上发现并修掉一个真 bug**:validation id 用连字符
     (`test-core`)但 allowlist 函数是下划线(`v_test_core`),`type -t` 解析失败 → 加 `agent_validation_fn` 归一化
     (hyphen→underscore)统一到 `agent_validate`/dispatch/test_phase,并补 5 条回归断言。
   **Step D 多候选并行 + 确定性排序落地实测(2026-06-05,`script/agent/{routing.env,common.sh,lint_phase.sh}`)**:把
   §4/§5.1 的"C1 默认即多候选 fan-out,只验证最优"从设计变成代码。Step D 的另一半(锁 + guard + 时间隔离 + rollback)
   早已落地;这次补上**候选生成与排序**这半:
   - **路由表给候选集**:routing.env 加 `ROUTE_C1_CANDIDATES`(worker 函数名数组)+ 第二候选 `route_c1_worker_pro`
     (deepseek-v4-pro)。**一个条目 = 旧的单 worker 行为**(零 fan-out,最省);加 worker 即开启并行多候选。Gemini Flash
     因 Step A 的叙述/输出契约问题暂不入列。源旧 routing.env(无该数组)自动回退 `(route_c1_worker)`,向后兼容。
   - **确定性排序原语**:`common.sh` 加 `agent_patch_files`(数 `+++` 头 = 触及文件数)与
     `agent_rank_candidates`(读 `<files> <churn> <id>`,按"文件少 → churn 小 → id"稳定排序)。**纯函数、可离线单测**;
     语义并列时的 tie-break 才上交 frontier(§5.1 step 2),这里不烧 frontier 注意力。
   - **runner 改造**:`lint_phase.sh` 每个 fix round 现在:① 把候选集**并行** fan-out,各 worker 只改自己的 sandbox
     副本;② harness-diff 取各候选 patch,churn guard 过滤 no-op / 超限;③ `agent_rank_candidates` 排序;④ 只对
     **top-K**(`MAX_VALIDATE`,默认 2)按排名跑慢 tidy 验证,**第一个有进展的候选**被接受并进下一轮(fixpoint 不变),
     top-K 皆无进展 → escalate。失败候选回退到**轮起点**(非 phase 起点),保留前几轮已接受的进展;escalate 才整体回退。
   - **离线确定性 e2e**:`run_lint_fanout_test.sh` 用 `AOBUS_LINT_TIDY`(假 tidy)+ `AOBUS_ROUTING_ENV`(mock 候选)+
     `AOBUS_AGENT_REPO`(临时树)三个测试 seam 把整条控制流跑通,**不依赖任何 model 或 clang-tidy**:实证"排序而非启动
     顺序决定胜者"(rewrite 先启动仍输给后启动的 surgical)。这把 Step D 的新逻辑纳入 CI,而非只靠真实 worker 抽测。

3. **Step C:结构化 packet + validation allowlist —— 已落地并继续硬化(见上实测)**。packet = YAML frontmatter +
   markdown 正文;mutating request packet 要求 `schema: aobus-phase-packet/v1` + `kind: request` 并走 closed
   schema gate。validation 只允许 allowlist 中的固定 ID + 安全参数(支持 `validation_args`),不接受任意 shell
   字符串;逐参数 enum/type 契约已由 `VALIDATION_ARGSPEC` + `agent_validation_args_ok` 落地。C2 test validation
   额外限定为 `test-core` / `test-gtk`,并用 Catch2 high-verbosity list output 把 selected filter 绑定回目标
   source file + `target_anchor`。dispatcher 的 runner 注册表已含 C1(lint)+ C2(test)。**未尽/进行中**:
   C2 keep 的 post-C3 review outcome 统计。
4. **Step D:patch + deterministic guard + 时间隔离 + 多候选并行/排序 —— 已落地(见上实测)**。锁 + guard + 时间隔离 +
   rollback 早已落地;**候选并行 fan-out + 确定性排序 + 验证 top-K** 这半现也落地(`ROUTE_C1_CANDIDATES`、
   `agent_rank_candidates`/`agent_patch_files`、`lint_phase.sh` 的多候选 round、`run_lint_fanout_test.sh` 离线 e2e)。
   廉价 model 并行产多个 patch;apply 前跑 diff guard + ranking,只把 top-K 候选送入慢 validation,失败回退轮起点取下一个。
   **未尽**:K 的自适应、跨厂商第二候选的真实 silent-wrong 滚动统计、候选间的语义 tie-break(并列时才上交 frontier)。
5. **Step E:C2 推广与抽象化(收窄版)**。已推广到 `improve-test-coverage/C2` + `write-unit-test/C2`,但共同
   contract 仅限**一个现有、已注册 Catch2 测试文件**里的 C3-decided test augmentation。C2 request 必须带
   focused validation filter 与 baseline 中不存在且 list-output 可见的 `target_anchor`;runner 在 worker 前跑
   baseline filtered test,keep 时确认 selected filter 列出了目标 source + anchor,并产 review dossier + audit,
   由 C3 语义 review 并用 `record_review.sh` 记录 accept/reject。生产代码 body-fill、lint checker
   body、multi-file helper edits 都暂不路由,必须等该 C0 safety envelope 的统计证明风险可控后再另建 runner。
   当确实需要无人值守、并发或队列化时,再把 dispatcher(本就是 C0 逻辑)进一步独立成工具。
6. **Step F:C3 council(已落地,2026-06-05)**。把 C3 从纯文档 roster 升级为可召集的多模型议事会。落地件:
   `council.sh` 四轮(R1 盲 draft → R2 challenge → R3 自我 revise → R4 chair 验证并综合;脚本只跑 R1–R3,R4 留给 in-loop
   chair)、只读成员 + per-member 仓库副本 + `agent_tree_hash` 前后哈希 canary(改树即丢弃 + 归因)、quorum(默认 ≥2
   member draft 才有辩论,不足则 `quorum: degraded` 仍出 dossier)、dossier 拼装;routing.env `ROUTE_C3_MEMBERS`(四个
   只读成员,其中 Claude/Opus 也是普通 member)+ `ROUTE_C3_MEMBER_LABELS`;`run-council` skill 统一 plan/review 两 mode 契约,review 经 code-review /
   diagnose-issue 召集;离线套件 `run_council_test.sh`(63 断言)。详见 §11。

## 10. 固定事实、成本模型与开放风险

### 10.1 固定事实

- **低成本模型 token 成本近似可忽略**。设计优化目标不是省 Flash/DeepSeek token,而是减少 frontier 手写量,
  同时控制 validation 次数、review 注意力和静默错修风险。
- **`--fix` 已证实禁用**:`run-clang-tidy.sh` 确实有 `--fix`(走 `-export-fixes` 批量 apply),但实践证明
  目前还不成熟,会搞烂文件(批量 overlapping replacement 冲突、破坏未覆盖区域)。**v1 一律不进入任何
  自动化路径**;只用 tidy 诊断作为输入。即便将来重试,也必须在 patch + deterministic guard + validation +
  C3 review 的完整流程内,不得旁路。

### 10.2 成本模型

- **真正成本**:`delegate ≈ headless 调用冷启动 + 候选并行的速率/延迟约束 + patch guard/ranking +
  apply/锁/rollback 管理 + 慢 validation(× 至多 K 次重试) + frontier C3 review + 静默错修风险`。
  其中低成本模型 token 不是主要项;多候选先受 rate limit 约束,慢 validation 受 K 预算约束。
- **策略变化**:小批量也可以先让低成本模型产候选 patch,因为生成候选几乎免费;但不能给每个候选都跑
  `run-clang-tidy.sh` / `build.sh`。必须先 guard + ranking,只验证最小、最可信的候选。
- **净收益判据**:不是"省了多少低成本 token",而是"候选 patch 是否减少 frontier 手写/搜索时间,且没有增加
  silent wrong、validation 重跑和人工回滚成本"。pilot 要量这个阈值。

### 10.3 开放风险

- **能力边界要 eval**:Flash/DeepSeek 对 Aobus C++26 规范 + `aobus-*` tidy 的实际修复率,由 Step A 的四个指标
  量化后才能定 C1 可信边界与备选顺序。
- **隔离 ≠ 沙箱(已被 agy 坐实)**:git 时间隔离/worktree 只隔离**git 状态**,不隔离**进程能力**。无头跑的廉价
  CLI 若其 harness 授予 exec/网络/宽文件系统权限,边界拦不住它在树外乱跑。**实例**:`agy` 在 `steam-run`(把整个
  `$HOME`、含真实仓库 bind-mount 进沙箱)下,曾按提示里的仓库相对路径**直接改了真实仓库文件**,而非 cwd 里的
  sandbox 副本(它偏好真实 git 项目)。opencode worker 没这问题:它跑在 `/tmp` cwd 且其自身 external-dir 权限会拦
  截树外访问;agy 的"沙箱"只是 cwd 约定,被 `$HOME` 全量挂载击穿。**当前缓解**(对**可信**厂商模型够用):把目标
  stage 成**不与任何仓库相对路径碰撞的扁平唯一名**,prompt 只提该名 → agy 留在 cwd 副本里改,canary 证实真实树
  sha256 不变;且所有编辑仍过 harness-diff + guard + 独立复验。**但这不是硬沙箱**:足够 agentic 的 CLI 仍可能去
  content-search `$HOME`。若要上**不完全信任**的模型,真正的沙箱(容器/firejail/受限 bind mount,且仅挂载 stage +
  该 CLI 自己的鉴权配置)是必须单列的另一层——这条仍是开放项。
- **非交互调用与认证**:各 CLI 的 headless flag、鉴权、速率限制、上下文窗口差异在 Step 0 探针中确认并登记进路由表。
- **可观测性**:phase packet + validation 退出码 + patch/report artifacts 天然可审计;C2 keep 还必须留 review
  dossier、manifest/audit,并由 C3 在 commit 前记录 accept/reject。没有 post-C3 review outcome 的 pass-rate 统计
  会系统性高估 C2 质量。
- **与 RTK 正交叠加**:RTK 压缩*输出* token,本设计压缩*model 用量*,二者可叠加。

## 11. C3 议事会(council):plan/review 的多模型对抗-综合

C1/C2 靠"廉价 worker + 确定性 gate"收敛;**C3 没有确定性 oracle**——plan 与 review 是 prose,"对不对"是判断,
没有退出码能裁。所以 C3 不走 ranking+validation,而是召集一个**跨厂商议事会**:多个 frontier 模型先各自起草,再
**相互 challenge**,再自我修订,最后由 **chair(运行此流程的 in-loop frontier agent)综合**出唯一答案。这是 C1 fan-out
在 C3 的类比,但收敛机制是**对抗式交叉质询 + chair 综合**,不是确定性排序。

### 11.1 协议(四轮)

```
R1  盲 draft      每个 routed member 独立起草,无任何 peer 上下文(保多样性)
R2  challenge     每个成员看到其他人的 draft 并逐条质询(对抗、具体)
R3  自我 revise    每个成员看过针对自己的批评后修订自己的 draft
R4  综合          chair 读 dossier,独立验证关键 claim,写最终 plan/review,显式裁定共识 vs 分歧
```

- **R1 盲是关键质量属性**:成员在 R1 不能看到彼此,否则相互锚定、多样性塌缩(召集议事会的全部价值就在多样性)。
  离线套件断言 R1 prompt 不含任何 peer 文本,R2/R3 才注入。
- **chair 只做 R4 验证与综合**,不再作为 R1 drafter 参赛。若 chair 的同模型也需要产出 R1/R2/R3 意见,它必须作为
  `ROUTE_C3_MEMBERS` 里的普通 member 被外派调用。
- **R4 是 evidence-driven synthesis**:chair 读 dossier 后回到代码、diff、测试和设计文档独立验证关键 claim,再裁定哪些
  member 观点采纳、哪些拒绝,以及依据是什么。

### 11.2 C0/C3 切分

`council.sh` 是 **C0 管线**:fan-out、收集、拼 prompt、跑只读 canary、拼 `dossier.md`——**不做任何判断**,且**故意
不做 R4 综合**。R4 是唯一不可外包的 frontier 动作,留给 chair 在 in-loop 读 dossier 完成。这与 `dispatch.sh` 同构
(一个 C0 runner 路由 C3 工作),也让脚本保持确定性、可离线单测;真正的 in-loop frontier 注意力集中在 R4 验证与
最终综合。

### 11.3 只读安全模型(与 C1/C2 不同)

council 成员是**只读**的:它产出**意见**而非 patch,永不改树——所以**没有** sandbox-copy / guard / churn / rollback
那套(什么都不会被 apply)。安全闸门换成:
1. **只读调用**:各厂商的只读 headless 模式(`codex exec -s read-only`、`gemini -p --approval-mode plan`、
   `opencode run` 查询、只读 `claude -p`)。尽力而为,各 CLI 保真度不一。
2. **树不变 canary(硬兜底)**:每个成员在**自己的一份可弃仓库副本**里跑(cwd = `AGENT_COUNCIL_CWD`),council.sh 用
   `agent_tree_hash` 在调用前后对该副本做哈希(内容 + 文件模式 + 符号链接目标,故 chmod / 符号链接改向也抓得到);**任何改了
   副本的成员,其产出被丢弃并标记**(成员写文件就是违约,与 C1
   "diff 即交付"正相反)。per-member 副本让这道检查在**并行 fan-out** 下仍可**归因到具体成员**。
   The agy/Gemini member cannot run directly from `/tmp` because `steam-run` gives agy a private `/tmp`;
   routing therefore stages its read-only council copy under `$HOME/.cache/aobus-agy-council`. The helper
   hashes that staged copy too, and if agy mutates it, writes a sentinel into the outer `AGENT_COUNCIL_CWD`
   so the normal council canary still discards the member as a read-only violation.
3. 成员副本不含 `.git`;§10.3 的"足够 agentic 的 CLI 可逃逸出 cwd"对**不完全信任**的 review 输入仍是开放风险(对可信
   厂商 roster 够用,与 C1 agy 同一信任姿态)。

### 11.4 quorum、成本与入口

- **quorum**:一个议事会要有真正的辩论,至少要 `COUNCIL_MIN`(默认 2)份 member draft;不足时 council.sh 仍出
  dossier 但标 `quorum: degraded`,告诉 chair"这接近单跑",由它决定是否照办或重召。零 draft → exit 2。
- **成本**:N 个成员各跑 R1/R2/R3,再由 chair 做 R4;每次都是真 frontier token。与 C1(token 近免)相反,council
  **opt-in**、只用于高风险 plan/review(架构、错误契约、风险 diff)。轮内并行 fan-out 把墙钟压到约 3 个串行慢轮。
- **入口(plan 与 review 都接)**:`run-council` skill 是两个 mode 的统一契约;review 经 `code-review`(内建)/
  `diagnose-issue`(注脚)召集,plan 在 plan 模式由 chair 召集、综合结果即 plan 文件 / `ExitPlanMode` 产物。
- **packet**:复用 v1 schema,新增 `kind: council` / `mode: plan|review`,**无 `validation:`**(无确定性 gate),
  故**不走 `dispatch.sh` 的 allowlist 路径**;council.sh 自成入口。

### 11.5 落地实测(2026-06-05)

`script/agent/council.sh`(C0 编排)+ routing.env `ROUTE_C3_MEMBERS`(claude / codex / gemini / opencode 四个只读成员函数 +
`ROUTE_C3_MEMBER_LABELS`,跨厂商默认、opt-in/可配,fallback 到默认四人组)+ common.sh `agent_tree_hash`(通用目录内容哈希,

排除 `.git`,含 mode/符号链接目标的 typed manifest)+ `.agents/skills/run-council/SKILL.md` + `diagnose-issue` 注脚。
成员 prompt 走 **stdin**(从 `AGENT_COUNCIL_PROMPT_FILE`,绕开单 argv 的 `MAX_ARG_STRLEN` 128KB 上限);`run_one` 硬判
成员退出码(非零即丢弃,不让 timeout/crash 的部分输出蒙混就座)。离线确定性套件 `run_council_test.sh`(63 断言,无 model /
无网络)用 mock 成员(`AOBUS_ROUTING_ENV`,行为由 `COUNCIL_MUTATE` / `COUNCIL_MUTATE_R2` / `COUNCIL_FAIL` /
`COUNCIL_SILENT` / `COUNCIL_ROSTER` 切换)+ 临时树验证整条管线:happy path、canary 抓改树成员(丢弃 + 归因)、R1 盲、
坏 mode / unsafe input / OUT-in-repo 拒、单成员 → quorum degraded、静默/非零退出成员 → 丢弃续跑、R2 阶段违规 → 整体隔离、
canary 对 chmod/符号链接改向敏感、inputs 注入 prompt、R3 排除自评、plan/review 两 mode 选对模板。

**自审固化(dogfood)**:用本机制对自身跑了一次真实 review council(chair=in-loop agent + GPT-5.5 high via codex + Gemini 3 Pro
via gemini,只读)。跨 challenge 的产出抓到一批连单模型思考与离线断言都漏掉的真 bug——`printf '--'` 选项解析、
`run_one` 漏判退出码(部分输出被当成功)、R3 把成员自己的 challenge 当"对它的批评"回灌、R2/R3 阶段违规未整体隔离(R1 draft
仍进 dossier)、`AGENT_COUNCIL_OUT` 落在 repo 内会把 in-flight artifact 拷进成员 cwd 破坏 R1 盲——并指出 prompt 走单 argv 会
撞 `MAX_ARG_STRLEN`。逐条修复并各补回归断言。这本身是 council 价值的最佳实证:多样性抓到了单模型 + 确定性测试都没抓到的东西。

**未尽**:对**不完全信任**输入的硬沙箱(容器/firejail/bwrap,§10.3 开放项——只读 flag + canary 拦不住进程级网络/exec);
K(成员数)与轮数的自适应;成员"自信但错"的滚动统计与熔断;chair 综合质量的人评。
