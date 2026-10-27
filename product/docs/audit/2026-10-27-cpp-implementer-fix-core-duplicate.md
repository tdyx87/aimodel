# 编译修复：去除 core_rebuild_url 重复定义

| 字段 | 内容 |
|------|------|
| 角色 | cpp-implementer |
| 时间 | 2026-10-27 |
| 任务 | fix-core-duplicate-def |

## 1. 输入
CI 编译失败：core.c 重复定义 static int core_rebuild_url

## 2. 关键决策
删除后半段重复定义，保留前置定义。

## 3. 产出
- product/src/core.c

## 4. 结论
完成并移交

## 5. 移交
orchestrator
