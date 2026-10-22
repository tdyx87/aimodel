# 审计：标准工具握手（role:"tool" + tool_call_id）

| 字段 | 内容 |
|------|------|
| 角色 | `cpp-implementer` |
| 时间 | 2026-10-22 |
| 任务 | 把工具结果回灌从「尾随 user 消息」改为 OpenAI 规范的**标准工具握手** |

| 结论 | 完成并移交 |
|------|------------|

一句话：让模型看到「这是你刚才那次工具调用的结果」，而不是「用户又说话了」——
根治真实服务上「模型反复请求同一工具、用户被迫反复批准」的问题。

## 1. 输入

- 契约：`product/docs/design/contract.md` §4.3（协议适配）、§5.3（回灌形式）、§2.5.6（工具回合不跨输入保留）
- 交接：`product/docs/HANDOFF.md` §6「高：工具结果标准握手」
- 上游代码：`src/core.c`（`results_append` / `results_append_notice` / `ca_core_run_once`）、
  `src/llm.c`（`ca_llm_build_request_full`）、`include/cagent/llm.h`（`ca_llm_msg`）、
  `src/session.c`（`ca_session_export`）
- 故障记录：真实服务（GLM-4.6/4.7）上任一工具执行后，模型倾向**原样再要一次同一工具**

## 2. 关键决策

### D1. 消息结构里存 tool_calls 的**原文**，而不是解析成结构体

`ca_llm_msg` 新增 `tool_calls_json`（助手）与 `tool_call_id`（rolling 结果）两个字段。

- **选择**：`tool_calls_json` 保存从服务端逐字抄回的 JSON 数组原文，直接原样写回请求体。
- **理由**：服务端会拿回带的 tool_calls 与它当初发来的做一致性校验。
  任何「解析 → 重新序列化」都可能改变字节（键序、转义形式、数字格式），
  从而被拒；而这段 JSON 本来就是我们刚收到的原文，没有任何改写它的好处。
- **否决的备选**：拆成 `{id,name,args}` 结构体再序列化——多一道可能漂移的编解码，
  且要重新实现转义，收益为零。

### D2. tool 结果**每条独立成消息**，不是拼成一坨

一轮里模型可并发请求多个工具（`CA_LLM_MAX_TOOL_CALLS` 上限 8）。
每条结果各成一条 `role:"tool"` 消息、各带自己的 `tool_call_id`。

- **理由**：服务端按 `tool_call_id` 配对调用与结果；拼在一起会错配。
- 结果文本仍统一落在 `c->results` 缓冲（256 KB 上限不变），用
  `{id, off, len}` 记录切分，避免新开一份无界缓冲。

### D3. 回带块**不可被部分丢弃**

历史淘汰（请求体超限时丢最旧完整轮次）只允许发生在回带块**之前**：
`core_one_turn` 里新增 `hist_count`（= `nmsg - replay_n`），淘汰游标不得越界。

- **理由**：把 assistant 的 tool_calls 与 tool 结果拆散，会让服务端看到
  「有 tool 结果却没有对应调用」而 400；界面上只会显示一句「请求失败」。
  要么整块保留、要么整轮报错，绝不发半截握手。
- 被否决：允许丢进回带块——会制造上面那种最难查的「只在长上下文下出现」的 400。

### D4. 缺 `tool_call_id` 的 tool 消息在**构造阶段**判为参数非法（`-1`）

不是拖到发请求才失败，也不是回退成 `-2`（缓冲不足）。

- **理由**：语义上它是「参数不合法」，与容量无关；早一点失败能给出可行动的报错。
- 实现上：把校验放进 `ca_llm_build_request_full` 开头的参数校验循环。

### D5. 单轮（无会话）模式的握手请求仍带**本轮用户输入**

`ca_core` 新增 `turn_user`，`core_collect_msgs` 在 `replay_open && n==0` 时
用它补一条 user 消息。

- **理由**：无会话模式下历史为空，若不补，握手请求会变成
  「assistant 调工具 + tool 结果」——**没有问题可答**。

### D6. 保留「已执行 / 被拒绝」的状态说明措辞

标准握手解决了结构问题，但措辞仍有价值：被拒的工具**没有执行**，
说成「已执行」会让模型以为文件已写好。

- 结果：`<tool_result ... status="denied|done">...</tool_result>` 说明照旧附在
  该条 tool 消息的 content 尾部。

### D7. 顺手修 `llm.h` 里 `ca_llm_build_request_full` 的**重复声明**（含重复注释块）

纯清理，无行为变化；顺带补上 `CA_MSG_TOOL` 的角色字面量映射。

### D8. 回带的 `tool_calls` 必须是**数组**（实现期踩到并修掉）

`build_replay_tool_calls` 最初拼出的是 `{"id":...}`——**对象**而非数组 `[...]`。
服务端会直接 400（`tool_calls` 类型不对），而界面上只显示「请求失败」。

- **为什么单元测试没抓到**：`llm_test` 里 `tool_calls_json` 是**手写**的带方括号，
  测的是「原样回带」这件事，不是「拼出来的形状对不对」。
  真正抓到它的是 `core_test` 里**看真实请求体**的断言
  （`"tool_calls":[{"id":"call_abc"`）。**这就是本项目反复强调
  「看请求体，别看内存结构」的价值。**
- 已加断言：请求体必须能被 JSON 解析器解析（用仓库自带的 `json.h`），
  且 `tool_calls` 的值必须以 `[` 开头。

### D9. 工具结果的回灌上限**由请求体容量倒推**（修掉一个既有 bug）

原先：回灌缓冲固定 256 KB，请求体默认也是 256 KB（还可配到 16 KB 下限）。

- **真实故障**：工具结果一旦填满回灌缓冲，**必然**装不进请求体 →
  整轮失败，用户看到「单轮内容超出上下文上限」；
  把 `CAGENT_CONTEXT_BYTES` 配到 16 KB 下限时，**任何稍大的工具结果都会让代理不可用**。
- **修法**：`ca_core_init` 里按 `body_cap - (工具声明 + 系统提示 + 8 KB 余量)`
  算出 `results_cap`，截断器按它执行。**截断器的作用是「保证请求还发得出去」，
  不是「尽量多塞」**——塞不下就发不出去（契约 C4 的本意）。
- 新增 `ca_core.results_cap` 字段；配额为 0 时（请求体小到放不下任何结果）
  结果被完整标注为「已截断」，而不是让请求失败。

### D10. 回灌缓冲的截断路径修掉两个隐患（既有代码带过来的）

- **不写 NUL 结尾**：序列化器按 C 字符串读结果，没 NUL 就一路读出缓冲区（越界读）。
- **可能把 `results_len` 回缩到某条消息起点之前**：那条消息的长度会下溢成
  天文数字（`size_t`），越界读。
- 现改为：截断时**只增不减** `results_len`，且**始终**写 NUL 结尾；
  标注里带上实际字节上限（「把取舍摆到明面上」）。

## 3. 产出

| 文件 | 改动 |
|------|------|
| `include/cagent/llm.h` | `ca_msg_role` 增 `CA_MSG_TOOL`；`ca_llm_msg` 增 `tool_calls_json` / `tool_call_id`；删重复声明 |
| `src/llm.c` | `role_name` 增 `tool`；序列化器分支：assistant+tool_calls（`content:null`）、`role:"tool"`+`tool_call_id`；参数校验拒绝缺 id 的 tool 消息；单轮构造器 `memset` 初始化新字段 |
| `include/cagent/core.h` | `ca_core` 增 `replay_tool_calls` / `replay_open` / `tool_msgs[]` / `tool_msg_count` / `turn_user` / `results_cap` |
| `src/core.c` | 删 `results_append`/`results_append_notice`，新增 `tool_msg_begin`/`tool_msg_append`/`tool_msg_append_notice`/`build_replay_tool_calls`；`core_collect_msgs` 输出回带块；`core_one_turn` 增 `hist_count`；主循环改为构建 tool 消息；`ca_core_init` 计算 `results_cap` |
| `src/session.c` | `ca_session_export` 显式把新字段置 NULL（见 §4 真 bug） |
| `tests/core_test.c` | 新增 `core_uses_standard_tool_handshake`、`core_handshake_places_each_result_in_its_own_tool_message`、`core_truncates_oversized_tool_result_explicitly`（已登记到用例表）；请求体捕获缓冲 20 KB → 320 KB |
| `tests/llm_test.c` | 新增 `llm_serializes_standard_tool_handshake`、`llm_tool_message_requires_an_id`（已登记）；`llm_builds_request_with_escaping` 增「单轮构造器不得带工具字段」断言 |
| `tests/session_test.c` | `session_exports_llm_messages` 增断言：导出的历史消息工具字段必须为 NULL |
| `docs/design/contract.md` | §4.3 补请求侧角色说明；§5.3 把「已知局限」改为「已采用标准握手」 |

## 4. 术语与实现边界（如实记录）

### 实现期发现的三个**真 bug**（都不是测试数据问题）

**（1）未初始化指针 → 段错误**

`ca_llm_msg` 增字段后，`ca_session_export` 只写了 `role` 与 `text`，
新字段是**未初始化的垃圾指针**。序列化器按「`tool_calls_json != NULL` = 是
tool_calls」去解引用它们 → `core_test` 立刻段错误（`core_tells_user_when_history_is_evicted`）。

- **根因**：结构体增字段而某个**生产者**（这里是 session 导出）没更新。
  这类漏洞编译器不会报，单测若不先跑一遍历史路径也未必撞上。
- **修法**：`ca_session_export` 显式置 NULL，并在 `session_exports_llm_messages`
  里钉住「导出的历史消息工具字段必须为 NULL」这个不变量，防止回归。
- 同一处问题在 `ca_llm_build_request` 里也有一份（`only` 结构体），
  一并 `memset` + 显式置 NULL。

> 教训：给跨层共享的结构体加字段时，**每一个填充它的地方**都要一起改，
> 并且新字段的「默认值」应当是可验证的不变量，而不是「碰巧是 0」。

**（2）`tool_calls` 拼成了对象而不是数组**

`build_replay_tool_calls` 最初漏了方括号，产出 `{"id":...}`。
服务端会 400（`tool_calls` 类型不对）。

- **为什么单元测试没抓到**：`llm_test` 的 `tool_calls_json` 是**手写**的
  带方括号，测的是「原样回带」而非「拼出来的形状对不对」。
- **谁抓到的**：`core_test` 里**看真实请求体**的断言。
  这是本项目反复强调「看请求体，别看内存结构」的一次实证。
- 已加断言：`"tool_calls":[{"id":"call_abc"`，且整个请求体必须能被 JSON 解析器解析。

**（3）回灌上限与请求体容量脱节 → 代理直接不可用**

回灌缓冲固定 256 KB，请求体默认也是 256 KB（还能配到 16 KB 下限）。
工具结果一旦填满回灌缓冲，就**必然**装不进请求体 → 整轮失败。
把 `CAGENT_CONTEXT_BYTES` 配到下限时，任何稍大的工具结果都会让代理不可用。
详见 D9。

### 其它如实记录

- **测试捕获缓冲的教训**：`core_test` 的请求体捕获缓冲原本是 20 KB。
  写「截断标注在末尾」的断言时，标注落在捕获窗口之外 → **假阴性**
  （仓库里早有同类记录：「捕获缓冲太小也是一种测试缺陷」）。
  已把缓冲提到 320 KB。
- **真机验证**：本会话未连真实服务复跑工具链（账户限流 429）。
  验证依据是 `core_test` 对**请求体**的断言、`llm_test` 对序列化产物形状的断言，
  以及一次**用 Python `json` 模块独立解析**请求体的旁证
  （确认产物的结构与 OpenAI 规范逐字一致）。
- **非 OpenAI 协议**：标准握手是 OpenAI 形态的；US-8 的非兼容适配层仍未做，
  届时 `llm` 层需把同一份 `ca_llm_msg` 映射到别的形状（本层已把差异封住）。
- **assistant 文本 + tool_calls 同时出现**：现实现里工具轮次的 assistant
  content 恒为 `null`（工具调用帧通常无文本）。若某服务在工具调用时也带文本，
  当前会丢弃该文本——属于已知未覆盖情形。

## 5. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| （orchestrator / `reviewer`） | | 待复核 | 核对产出与产物一致、D1–D7 理由是否成立、§4 段错误根因与修复是否吻合 |

## 6. 验证记录（本会话实测）

| 项 | 命令 | 结果 |
|----|------|------|
| Windows 构建 + ctest | `scripts/build.ps1 -Config Release -BuildDir build-curl ...` | 8/8 通过，零告警 |
| 单测计数 | 各 `*_test.exe` | core 231 / session 1735 / llm 77 / tools 128 / ui 63 / json 74 / sse 84 / http 25 / mem 37，**全 0 失败** |
| POSIX | `wsl -d Ubuntu-18.04 -- bash scripts/posix_check.sh` | `ALL CLEAN` |
| 内存 | `mem_load.py --turns 500` | 峰值 8.02 MB（基线 7.71 MB；增量来自回带缓冲） |
| Web 冒烟 | `web_smoke.py` / `web_tool_smoke.py` | 均 PASS |
| 独立校验 | Python `json` 解析真实请求体 | 合法 JSON，结构与 OpenAI 规范逐字一致 |
