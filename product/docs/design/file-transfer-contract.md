# design：文件传输（File Transfer）

> 产出者：architect
> 审计：product/docs/audit/2026-10-12-architect-file-transfer.md

---

## 1. 功能摘要

在 `system_info_ui` 页面增加文件传输能力：

1. 用户可上传文件到服务端授权目录；
2. 用户可从服务端授权目录下载文件；
3. 服务端严格校验路径和权限，防止目录穿越；
4. 不暴露内部绝对路径；
5. 不破坏现有 `system_info_ui`。

---

## 2. 服务端职责

- 接收文件上传；
- 校验文件名和目标目录；
- 限制文件大小；
- 将文件写入授权目录；
- 提供下载入口；
- 校验下载目标是否在授权目录内；
- 统一错误返回，不泄露内部路径。

---

## 3. 前端职责

- 提供文件选择；
- 提供拖放上传；
- 展示上传状态；
- 展示保存位置；
- 展示成功 / 失败提示；
- 下载文件；
- 不做权限判断；
- 不做服务端安全策略。

---

## 4. 接口定义

### 4.1 上传

- 路径：`POST /api/file/upload`
- 输入：文件内容、安全文件名
- 请求头：`Content-Type: application/octet-stream`（见 §4.6）
- 成功响应：

```json
{
  "ok": true,
  "saved_path": "uploads/12345.bin",
  "file_name": "12345.bin",
  "size": 1234
}
```

> `saved_path` 只给**相对**形式：授权根为相对路径时原样给出（如
> `data/uploads/12345.bin`），为绝对路径时退化为末段目录名
> （如 `uploads/12345.bin`）。**绝不**返回服务端绝对路径。

- 失败响应：

```json
{
  "ok": false,
  "code": "PATH_NOT_ALLOWED",
  "message": "路径不在允许目录内。"
}
```

---

### 4.2 下载

- 路径：`GET /api/file/download`
- 参数：

```text
file_name: string
```

- 成功响应：

```json
{
  "ok": true,
  "file_name": "12345.bin",
  "size": 1234
}
```

- 实际返回二进制流：

```text
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="12345.bin"
```

- 失败响应：

```json
{
  "ok": false,
  "code": "NOT_FOUND",
  "message": "文件不存在。"
}
```

---

### 4.3 状态码与错误码的对应

失败响应的 HTTP 状态码由错误码决定，**不再一律回 200**：

| 错误码 | HTTP 状态码 | 含义 |
|--------|-------------|------|
| INVALID_FILENAME | 400 | 文件名非法（空、超长、危险扩展名、点文件） |
| PATH_NOT_ALLOWED | 400 | 路径不在授权目录 |
| NOT_FOUND | 404 | 文件不存在 |
| TOO_LARGE / REQUEST_TOO_LARGE | 413 | 文件或请求体过大 |
| UNSUPPORTED_MEDIA_TYPE | 415 | 写接口的 Content-Type 不符（见 §4.6） |
| WRITE_FAILED | 500 | 写入 / 删除失败 |
| INTERNAL_ERROR | 500 | 内部错误 |

> 为何改：失败回 200 时，客户端无法按状态码判别。界面若用
> `window.location.href` 触发下载，浏览器会把错误 JSON 当正文渲染，
> 直接把用户踢出整个界面。状态码是前端区分「二进制流」与「错误 JSON」的依据。

成功响应仍为 `200`。

---

### 4.4 列出文件

- 路径：`GET /api/file/list`
- 成功响应：

```json
{
  "ok": true,
  "files": ["报告.txt", "data.bin"]
}
```

- 目录不存在时 `files` 为空数组（不报错）。

> 说明：`security-reviewer-file-transfer.md` 曾建议「HTTP 层不要暴露
> `ListUploads`」，顾虑是文件名泄露。该顾虑的前提是缺少跨站防护；
> 闸门（§4.6）补齐后，跨站页面既读不到响应（同源策略，且不返回 CORS 头），
> 也写不进请求，故本接口改为对外暴露——否则界面无法展示重启前的文件，
> 而删除接口（§4.5）也不可用。

---

### 4.5 删除文件

- 路径：`POST /api/file/remove?file_name=<name>`
- 成功响应：

```json
{
  "ok": true,
  "file_name": "报告.txt"
}
```

- 文件不存在时返回 `404` + `NOT_FOUND`。

---

### 4.6 请求闸门

服务虽只监听 `127.0.0.1`，但任何网页都能向该端口发请求，且 DNS rebinding
能让对方的同源校验指向本机。故服务端对**每个**请求按三层校验，任一不通过
一律 `403 Forbidden`（不说明具体原因，避免给探测方反馈）：

| 层 | 规则 | 挡住什么 |
|----|------|----------|
| 1 | `Host` 必须是 `127.0.0.1[:port]` / `localhost[:port]` / `[::1][:port]`，端口须等于实际监听端口 | DNS rebinding |
| 2 | `Origin` 若存在，必须是 `http://127.0.0.1:<port>` 或 `http://localhost:<port>`（`null` 一律拒绝） | 跨站 fetch / form 的写操作 |
| 3 | 写接口的 `Content-Type` 必须匹配 | HTML `<form>` 的跨站简单请求 |

第 3 层的具体要求：

| 接口 | 要求的 `Content-Type` |
|------|----------------------|
| `POST /api/file/upload` | `application/octet-stream` |
| `POST /api/chat/suggest` | `application/json` |

理由：跨站「简单请求」只允许 `text/plain` / `application/x-www-form-urlencoded`
/ `multipart/form-data` 这三种 CORS 安全类型；要发出上表的类型就必须先发
CORS 预检 `OPTIONS`，而本服务不响应预检（也不返回任何 `Access-Control-*` 头），
请求因此根本到不了服务端。

不匹配时返回 `415` + `UNSUPPORTED_MEDIA_TYPE`。

---

### 4.7 请求体完整性

`Content-Length: N` 的请求，服务端只收满 N 字节才派发。超时或对端断开导致
未收全时一律返回 `400 Bad Request`，**不得**把已收到的片段当完整请求处理。

> 为何强调：早期实现把「读超时」与「请求已完整」混为一谈，于是
> 声明 1000 字节只发 100 字节的上传被当作完整请求落盘，并回 `200 OK`——
> 用户拿到一个静默截断的文件。

---

## 5. 错误码

| 错误码 | 含义 |
|--------|------|
| INVALID_FILENAME | 文件名非法（空、超长、危险扩展名、点文件、首尾空白） |
| PATH_NOT_ALLOWED | 路径不在授权目录 |
| NOT_FOUND | 文件不存在 |
| TOO_LARGE | 文件过大 |
| WRITE_FAILED | 写入失败 |
| INTERNAL_ERROR | 内部错误 |

---

## 6. 安全边界

### 6.1 目录白名单

服务端默认授权目录：

```text
<app_root>/data/uploads
```

运行时由服务端配置。
前端只展示文件名，不展示完整路径。

---

### 6.2 路径校验

服务端必须：

1. 接收输入路径或文件名；
2. 去除 `../`；
3. 归一化路径；
4. 判断归一化结果是否仍在白名单目录内；
5. 拒绝绝对路径；
6. 禁止目录穿越；
7. 文件名不直接作为服务端绝对路径使用。

---

### 6.3 文件名约束

服务端必须：

1. 去除非法字符；
2. 限制文件名长度；
3. 禁止空文件名；
4. 禁止危险扩展名；
5. 必要时生成安全文件名；
6. 返回安全名称，不返回用户原始注入名。

---

### 6.4 文件大小

默认上限：

```text
10 MB
```

如果超过，返回：

```json
{
  "ok": false,
  "code": "TOO_LARGE",
  "message": "文件过大，请选择小于 10 MB 的文件。"
}
```

---

## 7. 目录边界

服务端默认允许目录：

```text
<app_root>/data/uploads
```

详情见：

- `product/include/aimodel/file_transfer.h`
- `src/cpp/file_transfer.cpp`
- `CMakeLists.txt`

---

## 8. 后续扩展

- 大文件分片；
- 断点续传；
- 多文件批量；
- 进度跟踪；
- 下载日志。
