# 存档格式与接口契约

| 字段 | 内容 |
|------|------|
| 契约范围 | 核心库对外接口 + 文件格式定义 + 前端通信协议 + 隐私架构 |
| 角色 | `architect` |
| 上游 | `product/docs/prd/PRD-save-inspector.md` |
| 状态 | 待 G1 门禁评审 |

---

## 1. 契约范围与不做什么

**覆盖**：文件格式定义、C++ 对外 API、前端 ↔ 核心通信协议、隐私架构。

**不覆盖**（PRD §4 范围外，实现方不得自行扩张）：写回存档、解密、多游戏格式、云同步。

---

## 2. 文件格式定义（PRD §5 待确认问题的结论）

PRD 把「格式由谁提供」列为待确认。**本契约的结论是：由 `architect` 固化一种自有格式**，理由是：

- 若依赖外部格式，本工具的可用性完全受制于第三方，且无法在 PRD 阶段确定验收基线；
- 自有格式可完整覆盖 PRD 需要的全部字段与损坏场景，且可被 `python-implementer` 精确生成样本。

> 若后续要支持真实游戏存档，只需新增一个格式适配器，不改对外 API（见 §3.2）。

### 2.1 布局

所有多字节整数为**小端**。偏移均为相对文件起点。

```text
[文件头 32B]
  magic        8B   固定 "SAVEFILE"
  format_ver   4B   uint32，1 / 2 / 3
  flags        4B   uint32，bit0=已压缩，bit1=已加密
  slot_count   4B   uint32，槽位数量
  reserved     4B   固定 0
  created_utc  8B   uint64，秒级 Unix 时间

[槽位表 slot_count × 24B]
  offset       8B   uint64，该槽位数据区起点
  length       8B   uint64，该槽位数据长度
  checksum     8B   uint64，该槽位数据的 CRC64（见 §2.1.1）

[槽位数据区]
  按槽位表顺序排列，各槽位数据按 format_ver 解码

[文件尾 8B]
  固定 "ENDSLOT\0"
```

### 2.1.1 CRC64 参数（实现必须逐位对齐，不得自行选择变体）

`CRC64` 有十几种互不兼容的变体。若只写「CRC64」，三平台各自实现会得到**不同的校验值**，
同一存档在不同机器上诊断结果相反——这是必须在契约层面钉死的一处。

| 参数 | 取值 |
|------|------|
| 变体 | **CRC-64/ECMA-182** |
| 生成多项式（正常式） | `0x42F0E1EBA9EA3693` |
| 初值 init | `0x0000000000000000` |
| 输入/输出反射 refin / refout | **否 / 否**（按 MSB 先行处理字节与各位） |
| 输出异或 xorout | `0x0000000000000000` |
| 校验值（对 ASCII `"123456789"`） | **`0x6C40DF5F0B497347`** |

> 最后一行是**强制测试向量**：`cpp-implementer` 必须在单测中断言该值，
> `python-implementer` 的样本生成器用同一算法。这是跨平台一致性的唯一可验证手段。

### 2.2 槽位字段的版本演进

| 字段 | v1 | v2 | v3 |
|------|----|----|----|
| `level` uint32 | ✅ | ✅ | ✅ |
| `coins` uint64 | ✅ | ✅ | ✅ |
| `items` 变长数组（id uint32 + count uint32） | — | ✅ | ✅ |
| `playtime_sec` uint64 | — | ✅ | ✅ |
| `achievements` 变长数组（id uint32） | — | — | ✅ |
| `last_played_utc` uint64 | — | — | ✅ |
| `signature` uint64 | — | — | ✅ |

**迁移规则**：v1 → v2 → v3 逐级补默认值（空数组、0）。工具**只读**原文件，迁移结果仅存在于内存与导出内容中（PRD NFR-5）。

### 2.3 损坏场景与判定

| 场景 | 判定依据 | 面向用户的结论 |
|------|---------|--------------|
| 非存档文件 | `magic != "SAVEFILE"` | 这不是存档文件，请确认是否选错 |
| 版本过新 | `format_ver > 3` | 存档来自更新版本的游戏，请升级游戏后再看 |
| 文件被截断 | 实际长度 < 声明长度，或缺尾部标记 | 存档不完整，可能在保存时被中断 |
| 槽位偏移越界 | `offset+length > 文件实际长度` | 第 N 号槽位的数据位置异常 |
| 校验和不匹配 | 重算 CRC64 ≠ 存储值 | 数据已被改动过，建议用游戏本体打开并重新存档 |
| 需密钥 | `flags` bit1 置位 | 该存档已加密，需要游戏本体才能读取 |

> 每种场景都必须产出**可执行的下一步建议**，而非仅错误码——这是 PRD US-5 的硬要求。

---

## 3. C++ 对外接口

### 3.1 核心类型（`include/saveinspector/`）

```cpp
enum class FormatVersion { kV1 = 1, kV2 = 2, kV3 = 3 };

enum class Severity { kInfo, kWarning, kError };   // 诊断条目严重级别

struct Diagnostic {
    Severity severity;
    std::string title;        // 用户可读：哪里坏了
    std::string detail;       // 用户可读：为什么坏
    std::string advice;       // 用户可读：能做什么
};

struct Item {
    uint32_t id;          // 稳定 ID（存档内编码）
    uint32_t count;       // 数量
    std::string name;     // 展示名，由核心库给出（见下方说明）
};

struct SlotSummary {
    uint32_t index;               // 槽位号（用户可见，从 1 开始）
    uint32_t level;
    uint64_t coins;
    uint64_t playtime_sec;
    uint64_t last_played_utc;     // 0 = 未知
    bool healthy;
};

struct SlotDetail {
    SlotSummary summary;
    std::vector<Item> items;            // name 已填好，前端不维护映射表
    std::vector<uint32_t> achievements;
    uint64_t signature;
    bool upgraded;                      // 是否由旧版本迁移而来
    FormatVersion source_version;
};

struct SaveFile {
    FormatVersion version;
    uint64_t created_utc;
    std::vector<SlotDetail> slots;
    std::vector<Diagnostic> diagnostics;   // 含成功项（如「已从 v1 升级」）
};
```

### 3.2 解析器接口

```cpp
class SaveInspector {
public:
    // 只读打开。绝不写入或修改 path 指向的文件（PRD NFR-5）。
    bool OpenFile(const std::string& path, SaveFile* out, std::string* error);

    // 从内存打开（供测试与拖放/粘贴场景复用同一套解析逻辑）。
    bool OpenBytes(const std::string& bytes, SaveFile* out, std::string* error);

    // 只读槽位详情；index 为 0 基。
    bool ReadSlot(uint32_t index, SlotDetail* out) const;

    // 导出：详情 → JSON；摘要 → 纯文本报告（PRD US-4 / US-7）。
    std::string SlotToJson(const SlotDetail& slot) const;
    std::string SlotToTextReport(const SlotDetail& slot) const;

    std::vector<SlotSummary> ListSlots() const;
};
```

**道具名映射归属**：`Item::name` 由核心库填充，ID→名称的映射表是核心库的内部实现。
**理由**：前端与核心各维护一份映射必然漂移，且存档格式演进时容易只改一处。
未知 ID 一律给出可读回退名（如「道具 #1234」），不得返回空串。

**格式适配点**：新增 `OpenFormat_v4()` 之类的重载即可支持新版本，**对外 API 不变**——这是 §2 结论的兑现方式。

### 3.3 硬约束（继承 `roles.md` §3.3 并针对本用例细化）

- 仅用 C++11，禁用 C++14+ 设施（`agents.md §7.1` 清单）；需要 `std::optional` 语义时用 `bool + T` 组合
- 平台差异（文件读取、CRC64 实现）隔离为 `*_win32.cpp` / `*_posix.cpp`，由 CMake `target_sources` 按平台选择
- 对外字符串一律 UTF-8；文件路径在 Windows 上须经宽字符 API
- **不得崩溃**：解析任意字节序列都必须返回 `false` 或降级结果，不得抛出未捕获异常
- 所有面向用户的 `title/detail/advice` 必须是自然语言，**禁止**把错误码或偏移量直接透给用户

---

## 4. 前端 ↔ 核心通信协议

前端为免构建原生静态页（`roles.md` §3.5），故采用**本地 HTTP + 同源端点**形态，沿用既有工程惯例。

| 端点 | 方法 | 请求 | 响应 |
|------|------|------|------|
| `/api/save/open` | POST | `?path=<urlencoded>` 或 body 为文件字节 | `{"ok":true,"save":{...}}` 或 `{"ok":false,"diagnostics":[...]}` |
| `/api/save/slots` | GET | — | `{"ok":true,"slots":[...]}` |
| `/api/save/slot` | GET | `?index=<n>` | `{"ok":true,"slot":{...}}` |
| `/api/save/export` | GET | `?index=<n>&format=json\|text` | 二进制流 + `Content-Disposition` |

**错误约定**（继承 aimodel 的教训，本次固化）：

- 失败响应用**对应 HTTP 状态码**，不一律 200；
- 业务失败（存档损坏）返回 `200 + {"ok":false,"diagnostics":[...]}` —— 这是**可预期的业务结果**，不是传输错误；
- 前端按 `Content-Type` 区分「成功数据流」与「错误 JSON」，**禁止**用 `window.location.href` 触发下载。

---

## 5. 隐私架构（PRD NFR-3 / NFR-5）

| 数据 | 流向 | 约束 |
|------|------|------|
| 存档内容 | 仅本机内存 → 本机界面 | 不落盘、不外发 |
| 用户本机路径 | **不得**出现在任何响应体与导出内容中 | 诊断只说「第 N 号槽位」，不说绝对路径 |
| 崩溃/遥测 | **无** | 禁止引入任何上报 SDK |

**只读保证**：`OpenFile` 全程以只读方式打开；实现方不得提供任何写入 API。这是可被 `security-reviewer` 以「产物中不存在写入路径」为判据核验的硬约束。

---

## 6. 各工种接口

| 工种 | 契约 | 产物目录 |
|------|------|---------|
| `cpp-implementer` | §2 格式、§3 API、§3.3 硬约束 | `include/saveinspector/`、`src/cpp/`、`tests/` |
| `python-implementer` | 生成各版本 + 各损坏场景的存档样本，供测试与手工验证使用（内部工具，不随产品分发） | `python/saveinspector_tools/` |
| `frontend-implementer` | §4 通信协议 + `design/` 界面稿 | `web/` |
| `ui-designer` | PRD US-1~US-7 的四态 | `design/` |

---

## 7. 待实现方注意的坑

以下均为 aimodel 实战中付出代价才暴露的问题，本契约显式列出以避免重蹈：

| 坑 | 后果 | 本契约的对策 |
|----|------|-------------|
| 把「读超时/中断」与「读全了」混为一谈 | 上传/解析静默产出截断数据 | §3.2 解析接口返回 `bool`，且 `diagnostics` 必须记录截断 |
| 错误一律回 200 | 前端无法判别，把错误当数据 | §4 状态码约定 |
| 损坏输入导致越界读/死循环 | 崩溃、卡死（NFR-2 不成立） | §3.3「不得崩溃」+ §2.3 逐场景判定 |
| 诊断文案泄漏路径/偏移 | 用户看不懂，且泄露本机信息 | §3.3 + §5 |

---

## 8. 移交

| 项 | 内容 |
|------|------|
| 交给 | `cpp-implementer`、`python-implementer`、`frontend-implementer` |
| 开工前提 | 本契约 §2 格式定义与 §4 协议为唯一事实源；实现方不得自行增删端点或字段 |
| 待办 | `ui-designer` 依 PRD US-1~US-7 产出界面稿（四态齐备），与本契约 §4 端点对齐 |
