# cpp-implementer — 真实服务可用性修复（工具声明 / HTTP 状态码 / 4xx 原因透出）

| 字段 | 内容 |
|------|------|
| 角色 / 时间 / 任务 | cpp-implementer / 2026-10-21 / 用真实第三方服务（GLM-4.7-FLASH）验证 C2 时暴露并修复三个「本地全绿、真实不可用」的缺陷 |
| 结论 | 完成并移交（`reviewer` 审 G2） |

## 1. 输入

- `docs/design/contract.md` §4.3（工具调用用 `tool_calls` 增量拼接）、§5.1（六工具）、§5.3（结果格式）、P1–P5。
- 用户的指令：给凭据，验证 release 条件 **C2**（真实第三方 HTTPS 跑通一轮，含一次工具批准）。
- 真实端点：`https://open.bigmodel.cn/api/paas/v4`，模型 `GLM-4.7-FLASH`（凭据只存在于进程环境变量，未落盘、未进会话文件）。

## 2. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| 工具声明由 **tools 层**提供（`ca_tools_schema_json()`），core 传给请求构造器 | 工具名与参数只有 tools 层是唯一真相；llm 层再写一份必然漂移（参数名写错 → 模型照错名字调用 → 表现为「工具总是失败」）。依赖方向也要求如此：core → tools，llm 不认识 tools | 在 llm 层硬编码工具表——违反依赖方向，且两份定义必然不同步 |
| 请求构造新增 `ca_llm_build_request_full(..., tools_json, ...)`，旧入口保留并传 NULL | 既有调用点与测试不必改；`llm` 层仍不知道 tools 的存在 | 直接改 `ca_llm_build_request_msgs` 签名——会波及所有测试与调用点，收益只有「少一个函数名」 |
| 4xx 错误体只保留**前 255 字节**（`turn_ctx.raw`） | 要把服务端原因说出来，就必须看到响应体；但契约 C1 禁止「按响应大小缓冲」。定长 255 字节既够用（错误 JSON 的 message 通常在开头），又与响应大小无关 | 缓存整段响应——直接违反 C1；只记状态码——用户仍然不知道是额度不足还是 key 过期 |
| 错误文案 `HTTP %ld: %s`，**格式串纯 ASCII** | MSVC 对含中文的格式串误报 C4474，而「零告警」是 CI 硬门禁 | 用中文全角冒号——每次构建都红 |
| 工具结果仍以**尾随 user 消息**回灌，另附「该工具已执行」提示 | 真实服务实测：模型看到裸结果会原样再要一次同一个工具，用户被迫反复批准同一操作 | 直接上 `role:"tool"` + `tool_call_id` 标准握手——这是正解，但需要同时回带 assistant 的 `tool_calls`、扩展消息结构与序列化，约 200 行改动且要动会话导出；本轮先做**能上线**的缓解，标准握手列为遗留项 |
| 新增 `product/scripts/real_api_smoke.py`（凭据只读环境变量） | C2 之前没有任何**可重复**的真实服务验证手段；这个脚本把「真实验证」变成一条命令 | 只在本次会话里手测——下次谁都不知道该怎么复验 |

## 3. 产出

| 文件 | 变更 |
|------|------|
| `src/llm.c` / `include/cagent/llm.h` | 新增 `ca_llm_build_request_full()`：请求体写入 `"tools":<json>`；**顺序**为「先关 messages 数组 → 再挂 tools → 最后关根对象」（顺序错会拼出 `{...,"tools":[...]]}`，服务端 400 且错误体为空） |
| `src/tools.c` / `include/cagent/tools.h` | 新增 `ca_tools_schema_json()`：6 个工具的 OpenAI 兼容 Schema，参数名与各工具实际解析的键一致 |
| `src/tools_shell.c` / `include/cagent/tools.h` | 新增 `ca_tool_search_json()`：从模型给的 JSON 直接执行 search（`ca_tool_search()` 收结构体，core 的 JSON 分发表里此前**没有 search**，「六工具」实际只有五个） |
| `src/core.c` | 请求体传入 `ca_tools_schema_json()`；`run_tool()` 增加 `search` 分派；`turn_ctx` 增有界 `raw[256]`；新增 `core_error_reason()` / `core_http_status()` / `core_note_http_error()`；`results_append_notice()` 附「工具已执行」提示 |
| `src/transport_curl.c` | **修 bug**：`header_cb` 原写 `g_ctx.status_code = size*nmemb`（把响应头**长度**当状态码）→ 改为解析 `HTTP/x.y NNN`；`curl_easy_perform` 后以 `CURLINFO_RESPONSE_CODE` 覆盖 |
| `src/main.c` | 错误输出统一为 `错误：<原因>`（去掉与原因重复的状态分类前缀）；`main` 不再重复打印（core 已打印，重复行会让用户以为失败了两回） |
| `tests/core_test.c` | 新增 `core_offers_tools_to_the_model`、`core_surfaces_http_error_body`、`core_keeps_generic_message_when_no_body`；假传输层新增 `g_fake_status`，≥400 时返回 `CA_HTTP_ERR_PROTOCOL`（与真实 curl 一致） |
| `scripts/real_api_smoke.py` | 新增：真实服务三件套自检（对话 / 多轮记忆 / 工具调用与落盘），429 自动重试，凭据只读环境变量 |

**验证**

- 本地：`ctest` **8/8**；`core_test` **192 检查 0 失败**；`session_test` 1731 检查 0 失败；`tools_test` 115 检查 0 失败；编译零告警。
- **负向验证（证明测试不是装饰）**：把 core 的请求改成不传 tools → `core_offers_tools_to_the_model` 立即 FAIL（exit 1）→ 恢复后转绿。
- **真实服务**（`scripts/real_api_smoke.py`，GLM-4.7-FLASH）：`RESULT: PASS (chat=ok, memory=ok, tool=ok)`
  - 对话：真实 TLS + 流式，模型自述「我是GLM大语言模型…」。
  - 多轮记忆：第一轮「记住 7」→ 第二轮正确回答 7，会话文件 **4 条消息**。
  - 工具：模型发出**原生 `tool_calls`** → 界面显示 `[调用工具 write_file]` + 路径 + 内容 + `--- write_file 请求。允许执行？[y/N]` → 批准后 **文件真的落盘**（内容 `ok`）。
  - 拒绝路径：喂非 `y` 字符 → `status="denied"` 结果回灌、「已拒绝该工具调用，未做任何改动」。
  - 限流：服务端频繁 429（code 1305「该模型当前访问量过大」），脚本重试后通过；**错误提示已能显示** `错误：HTTP 429: 该模型当前访问量过大，请您稍后再试`（修复前只有「请求失败」）。
- 密钥未落盘：会话文件不含 key（已核对）。

## 4. 结论

C2 的技术条件已满足：真实服务、真实流式、真实工具调用、真实批准、真实落盘。
三个缺陷的根因均已消除（工具声明缺失、状态码解析错误、4xx 原因被丢弃），
并各有能证伪的测试。完成并移交 G2/G3。

## 5. 移交

- `reviewer`（G2）：重点看 ① 请求体 JSON 的**拼装顺序**（错一个括号就是 400，且错误体为空）；② `raw[256]` 是否仍满足契约 C1「不缓存整段响应」；③ `results_append_notice()` 的英文提示是否会被误读为「工具已成功」——它在拒绝时也会附加，措辞需要斟酌。
- `test-engineer`（G3）：真实服务脚本的**可重复性**（限流重试是否掩盖真失败）、以及 `search` 新入口的覆盖。

## 6. 复核（由独立复核者填写，非自证）

| 复核人 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| reviewer | 2026-10-21 | 通过 | 见 `2026-10-21-reviewer-gate-g2-real-api.md` |
| test-engineer | 2026-10-21 | 通过 | 见 `2026-10-21-test-engineer-gate-g3-real-api.md` |
