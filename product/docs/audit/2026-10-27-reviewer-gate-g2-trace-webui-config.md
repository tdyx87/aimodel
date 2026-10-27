# 代码审查：trace 开关、Web 配置与传输层重试

| 字段 | 内容 |
|------|------|
| 角色 | `reviewer` |
| 时间 | 2026-10-27 |
| 任务 | gate-g2-trace-webui-config |
| 输入 | `product/docs/audit/2026-10-27-cpp-implementer-trace-webui-config.md`；工作区 diff（core/web/main/transport_curl） |
| 结论 | ✅ 通过（2 项 Medium，不阻塞合入，记录为技术债） |

## 1. 输入

- 实现者审计与产物：`include/cagent/core.h`、`include/cagent/web.h`、
  `src/core.c`、`src/web.c`、`src/web_page.c`、`src/main.c`、`src/transport_curl.c`。
- 契约：`docs/design/contract.md`（§3.1 环境变量、§4.3 协议适配、§7 终端命令）。

## 2. 审查清单（C++ 工种）

| 项 | 结论 |
|----|------|
| C++11 合规（无 optional/string_view/filesystem/make_unique/泛型 lambda） | ✅ 未引入 |
| 定长缓冲与边界（`snprintf` 返回值检查） | ✅ `core_rebuild_url`、`auth_header`、`model_buf` 均检查截断 |
| 所有权与悬垂 | ✅ `model_buf` 拷贝解决了 `cfg.model` 悬垂；`curl_slist_free_all` 在重试分支均释放 |
| 错误路径资源释放 | ✅ `curl_easy_cleanup` 在所有 return 前调用 |
| 敏感信息 | ✅ trace 默认关闭；密钥仍只存内存，未打印 |
| 跨平台 | ✅ 无新增平台分支 |
| 注释准确性 | ⚠️ `transport_curl.c` 注释写「简单退避」，实现中**没有退避**（见 M1） |

## 3. 发现的问题

**M1（Medium）重试无退避，且可能重复内容**

`curl_run` 的循环注释为「简单退避」，但循环体直接 `continue`，没有 sleep。
更关键的是：`on_data` 在 `curl_run_once` 内部已被调用过若干次后，
若状态码为 503/429 触发重试，回调会**再次**收到同一段内容，
上层若直接追加到缓冲区就会产生重复文本。

- 影响：仅在「服务端先回 200 头再中途返回 503」或代理改写状态码时出现；
  正常 503 发生在首包前，不触发。
- 建议：重试前要求 `g_ctx` 未收到任何数据（记录已回调字节数），或在重试
  分支清空上层缓冲。当前未强制，属于潜在缺陷。

**M2（Medium）`ca_core_reconfig` 非原子**

先写 URL，再校验模型长度；模型名过长时返回 -1，但 URL 已经生效。
密钥长度校验同理。调用方（Web 面板）把失败吞掉只在控制台留痕，
用户看到「已保存」但实际配置可能只生效了一半。

- 建议：先全部校验，再统一提交；或失败时回滚。
- 说明：长度上限分别为 256（模型）与 `CA_CORE_MAX_KEY_BYTES`，
  正常用户不会触发，因此定为 Medium 而非 High。

**L1（Low）`cfg.api_base` 被改成完整 URL**

`ca_core_reconfig` 中 `c->cfg.api_base = c->url_buf`，而 `url_buf` 含
`/chat/completions` 后缀。若后续代码把 `cfg.api_base` 当基址使用，
会拼出重复后缀。当前只有 `core_rebuild_url` 使用它，且只在 init 时，
暂无实际影响。

## 4. 产出

- 本审计即本次审查的结论载体：`docs/audit/2026-10-27-reviewer-gate-g2-trace-webui-config.md`
- 无代码产物（只读角色）；对实现者的两条 Medium 建议见 §3。

## 5. 结论

✅ 通过。无 Critical/High；M1、M2 记录为技术债，建议在下一个迭代修复，
不阻塞本分支合入。

## 6. 移交

- 交 `test-engineer`（G3）：单测全绿，另建议补一条「重试不重复内容」的用例。
- 移交 `orchestrator`：M1/M2 是否在本分支修复由编排者裁决（本次不修）。

## 6. 复核（待独立复核者填写）

| 复核者 | 时间 | 结论 | 说明 |
|--------|------|------|------|
| | | | |
