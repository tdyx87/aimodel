# 界面契约：聊天系统（Chat System）

**产出者：`architect`**  
**对应 PRD：`product/docs/prd/PRD-chat-system.md`**  
**联系方式：`product/docs/audit/2026-10-12-product-manager-prd-chat-system.md`**

---

## 1. 契约目的

本契约定义「聊天系统」要在 `system_info_ui` 上提供的能力：

> 用户通过本地界面表达一个“我这台电脑能做什么、应该用什么模型”问题；系统基于本地已有硬件报告数据，返回一个可执行、可核对的推荐与步骤。

它**不**引入外部 AI 模型、云端 API，也不改变现有硬件报告页面的主视觉结构。

---

## 2. 能力边界（范围）

### 2.1 本期必须实现

- 在现有 UI 中提供一个“聊天/推荐”入口；
- 用户可以输入一段自然语言问题；
- 服务端根据本地已采集的系统信息返回一个“配置摘要 + 推荐分类 + 下一步建议”；
- 推荐必须说明：
  - 当前配置摘要；
  - 推荐分类：`recommend` / `check` / `not_recommended`；
  - 建议步骤；
  - 不确定性与前提假设。

### 2.2 本期明确不做

- 不引入外部 AI 模型；
- 不引入云端 API；
- 不做完整多轮记忆；
- 不做“模型选择与量化推荐”的完整 AI 决策引擎；
- 不改变现有硬件报告页面的主结构（除非新的对话入口明确与之共用）。

---

## 2. 接口契约

### 2.1 核心概念

| 概念 | 含义 |
|---|---|
| `ChatRequest` | 用户向系统提交的自然语言问题 |
| `ChatResponse` | 服务端返回的建议结果 |
| `ConfigSnapshot` | 服务器从已有系统信息 API 采集到的本机配置摘要 |
| `Recommendation` | 推荐项：分类、理由、建议步骤 |
| `ErrorResponse` | 错误请求的返回体 |

### 2.2 数据模型

#### `ChatRequest`

```jsonc
{
  // 用户提交的自然语言问题。
  "query": "我这电脑能不能跑 7B 模型？",

  // 可选。用于调试和审计的服务端标签。
  // 例如：0 = 普通本地建议，1 = 含有“是否能跑模型”的关键词。
  // 生产环境可省略。
  "session_tag": "default"
}
```

#### `ConfigSnapshot`

```jsonc
{
  // 操作系统名称与版本。
  "os_name": "Windows",
  "os_version": "10 Pro",

  // 系统架构，例如 x86_64、arm64。
  "architecture": "x86_64",

  // CPU 列表。
  "cpus": [
    {
      "vendor": "GenuineIntel",
      "brand": "Intel(R) Core(TM) i5-10500 CPU @ 3.10GHz",
      "core_count": 6,
      "physical_core_count": 3,
      "frequency_mhz": 3096
    }
  ],

  // 显卡列表。
  "gpus": [
    {
      "vendor": "AMD",
      "name": "AMD Radeon R7 430",
      "video_memory_bytes": 2147483648
    }
  ],

  // 内存。
  "memory": {
    "total_bytes": 16994660352,
    "available_bytes": 3172605952
  },

  // 磁盘。
  "disks": [
    {
      "mount_point": "C:\\",
      "file_system": "NTFS",
      "total_bytes": 255113695232,
      "free_bytes": 257372160
    }
  ]
}
```

#### `Recommendation`

```jsonc
{
  // 推荐分类。
  // 可选值：
  //   "recommend"   - 适合当前硬件运行；
  //   "check"       - 需要进一步检查或配置；
  //   "not_recommended" - 对当前硬件不推荐。
  "category": "recommend",

  // 推荐原因/限制说明。
  "reason": "当前配置可能支持 7B 级别模型的粗略推荐，但大多数 7B 模型需量化或更强 GPU 显存支持。",

  // 下一步可执行建议。
  "next_steps": [
    {
      "title": "先确认显存",
      "detail": "当前显卡显存为 2 GB。",
      "action": "如果目标模型是 7B，建议优先检查显存是否满足；否则优先选择更轻量的 4-bit 量化方案。"
    },
    {
      "title": "核对内存可用量",
      "detail": "当前可用内存约 3.1 GB。",
      "action": "若运行任务需要较大工作集，建议先关闭后台程序或改用更小模型。"
    }
  ]
}
```

#### `ChatResponse`

```jsonc
{
  "ok": true,
  "request": {
    "query": "我这电脑能不能跑 7B 模型？"
  },
  "config_snapshot": {
    "os_name": "Windows",
    "architecture": "x86_64",
    "cpus": [ ... ],
    "gpus": [ ... ],
    "memory": { ... },
    "disks": [ ... ]
  },
  "recommendations": [ ... ],
  "meta": {
    "source": "local-system-info",
    "model_kind": "local-rule-based-suggestion",
    "privacy_scope": "local-only"
  }
}
```

#### `ErrorResponse`

```jsonc
{
  "ok": false,
  "error": {
    "code": "INVALID_QUERY",
    "message": "当前未检测到可用问题文本。请求必须包含非空的 query。"
  }
}
```

---

## 3. 服务边界

### 3.1 绑定地址

本期产品入口服务仅可本地绑定：

- 默认：`127.0.0.1`
- 不允许外部局域网访问；
- 不允许任意公网入口。

### 3.2 路由约定

本期新增路径建议设计为：

```text
GET  /api/chat/capability
POST /api/chat/suggest
```

其中：

- `GET /api/chat/capability`：返回本服务对“聊天/推荐”的能力说明。
- `POST /api/chat/suggest`：接受 `ChatRequest`，返回 `ChatResponse`。

### 3.3 建议输出限制

`ChatResponse` 中必须包含：

- 配置摘要；
- 推荐分类；
- 下一步建议；
- 明确的不确定性说明；
- 隐私边界说明：**本回答为本地规则建议，不代表外部模型计算结果**。

---

## 4. 错误语义与用户可见文案

前端不可直接暴露底层错误码。服务端在 `ChatResponse` 外返回一段面向用户的中文提示，至少包含：

- 当前状态；
- 是否需要用户操作；
- 是否需要刷新或重试；
- 如有问题，为什么会出错。

例如：

```jsonc
{
  "ok": false,
  "request": null,
  "config_snapshot": null,
  "recommendations": null,
  "error": {
    "code": "UNEXPECTED_ERROR",
    "message": "请求失败，请稍后重试。"
  }
}
```

前端展示时应避免直接显示内部错误码。

---

## 5. 隐私架构

- 服务默认仅监听 `127.0.0.1`；
- 用户问题文本默认只在本地处理；
- 任何“建议”均基于本地采集的系统信息；
- 不引入外部模型、不引入云端传输；
- 若后续增加“保存对话历史”“云同步”等功能，必须重新定义隐私架构，并重新走 G1-G4。

---

## 6. 验收依据

G1 门禁要求：

1. `product-manager` 的 PRD 已存在且含 Given/When/Then 验收标准；
2. `architect` 模块契约与 PRD 一致；
3. `ui-designer` 界面稿与 PRD 一致；
4. 每一项均有审计文档。

本契约的审计文件为：

```text
product/docs/audit/2026-10-12-architect-contract-chat-system.md
```

---

## 7. 兼容性说明

本契约不改变现有：

- `system_info.h` 公共接口；
- 现有 `system_info_ui` 入口；
- 现有 `GET /api/system-info` 数据格式；
- 现有静态页面结构。

它只增加新的请求路径和新的响应模型。
