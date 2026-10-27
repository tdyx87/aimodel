# 编译修复：core_rebuild_url 前置声明/定义

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-27 |
| 任务 | fix-core-rebuild-url-decl |

## 1. 输入

CI 编译失败（Linux/GCC）：core.c 调用 core_rebuild_url 时未声明，随后静态定义导致冲突。

## 2. 关键决策

将 core_rebuild_url 定义移到 ca_core_init 使用前，删除重复定义。

## 3. 产出

- product/src/core.c（编译修复）

## 4. 结论

完成并移交

## 5. 移交

orchestrator
