# 界面架构契约（architect）

> G1 契约：`cpp-implementer`、`frontend-implementer` 依据本文件并行开工，签名级约定不得私自变更。

## 1. 总体架构

```text
┌─ 产品（用户双击启动）────────────────────────────────┐
│  system_info_ui.exe                                 │
│  ├─ LocalServer（仅绑定 127.0.0.1，隐私红线）          │
│  │   ├─ GET /api/system-info → ToJson(GetSystemInfo)│
│  │   └─ GET /…  ← 静态资源 web/dist（React 产物）      │
│  └─ 启动后自动打开默认浏览器 → http://127.0.0.1:<port> │
└──────────────────────────────────────────────────────┘
     浏览器界面（用户看到的产品）：React + TS + Vite
```

- **为什么本地 HTTP 而非原生 GUI**：三平台一份 UI 代码；复用设计系统；离线可用；无 Qt 等外部依赖（CMake `FetchContent` 之外零新增 C++ 依赖）。
- **隐私架构**：监听地址固定 `127.0.0.1`（不可配置为 0.0.0.0）；无出站请求；UI 与数据均在本机闭环。

## 2. C++ 模块契约

```cpp
// include/aimodel/http_server.h
namespace aimodel {

struct ServerConfig {
    std::string bind_host;      // 仅接受 "127.0.0.1"（Start 内强校验）
    int port;                   // 0 = 系统分配；Start 后读 bound_port()
    std::string static_root;    // 静态资源目录；空 = 仅 API
};

class LocalServer {              // 不可拷贝；RAII：析构即 Stop
public:
    LocalServer();
    ~LocalServer();
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    bool Start(const ServerConfig& config, std::string* error);  // 后台线程
    void Stop();                       // 优雅停止（join），可再次 Start
    bool IsRunning() const;
    int  bound_port() const;           // Start 成功后的真实端口
};

}  // namespace aimodel
```

### HTTP 协议（v1）

| 方法/路径 | 响应 |
|-----------|------|
| `GET /api/system-info` | `200 application/json` = `ToJson(GetSystemInfo())` |
| `GET /` → `/index.html` | 静态文件；`static_root` 缺失或无 `web/dist` 时返回 404 提示页（用户语言） |
| `GET /<path>`（白名单扩展名） | 静态文件；含 `..`/反斜杠/`%2e%2e` → `404`（不泄露路径） |
| 其他方法 / 未知路径 | `405` / `404`，均为纯文本或提示页 |

- 仅支持 HTTP/1.0 简化子集：单请求连接、`Connection: close`、无 keep-alive/chunked。
- 静态扩展名白名单：`html css js json svg png ico woff2`。
- 错误处理：接口内部异常 → `500` + `{"error":"..."}`；**UI 永不向用户展示技术细节**（对应 cpp-implementer 硬约束）。

### 平台隔离（agents.md 6.3）

| 文件 | 职责 |
|------|------|
| `src/cpp/http_server.cpp` | 路由/解析/静态服务/线程编排（平台无关） |
| `src/cpp/http_server_impl.h` | 套接字类型 typedef（`_WIN32` 编译期宏，内部头不安装） |
| `src/cpp/http_server_win32.cpp` | winsock2：WSAStartup/listen/accept/recv/send/closesocket |
| `src/cpp/http_server_posix.cpp` | BSD sockets（Linux/macOS 共用） |

- CMake：`WIN32` 时 `target_link_libraries(aimodel PUBLIC ws2_32)`（静态库需传递给消费方）。

## 3. 前端契约（frontend-implementer）

- 栈：React 18 + TypeScript + Vite；`base: './'`；产物 `web/dist/`；**零 CDN、零运行时外部请求**。
- 数据源：`GET /api/system-info`（同源），类型定义与 `system_info.h` 字段一一对应：
  `{ os_name, os_version, architecture, hostname, cpus[{vendor,brand,core_count,physical_core_count,frequency_mhz}], gpus[{vendor,name,video_memory_bytes}], memory{total_bytes,available_bytes}, disks[{mount_point,file_system,total_bytes,free_bytes}], printers[{name,is_default,status}] }`
- 状态决策（对界面稿的裁决）：**单请求整体加载**——失败 → 全局错误态（含重试）；字段为空数组 → 该卡片空态；**不实现块级重试**（本机毫秒级响应，块级重试收益低、复杂度高）。
- 设计约束：严格遵循 `design/design-system.md` 与 `design/screens/system-report.md`。

## 4. 工具目标

| 目标 | 说明 |
|------|------|
| `system_info_cli` | 开发者/脚本用（保留，移入 README 开发者区块） |
| `system_info_ui` | **产品入口**：起服务 + 打开浏览器；参数 `--port N`、`--no-open`、`--web-root <dir>` |
| `web`（CMake 自定义目标） | `npm run build` 产物到 `web/dist` |
