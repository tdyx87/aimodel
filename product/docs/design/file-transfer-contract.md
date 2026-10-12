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
- 成功响应：

```json
{
  "ok": true,
  "saved_path": "uploads/12345.bin",
  "file_name": "12345.bin",
  "size": 1234
}
```

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

## 5. 错误码

| 错误码 | 含义 |
|--------|------|
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
- 文件列表；
- 下载日志。
