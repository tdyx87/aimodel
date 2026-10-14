# 审计：release-engineer · 0.1.0 打包与发布检查单

## 1. 基础信息

| 字段 | 内容 |
|------|------|
| 角色 | `release-engineer` |
| 时间 | 2026-10-13 19:50 |
| 任务 | CPack 三平台打包配置、打包脚本、产物冒烟、发布检查单 |
| 门禁 | G4 |

## 2. 输入 / 依赖

| 输入 | 路径 / 来源 |
|------|-------------|
| 上游审计 | `2026-10-13-doc-engineer-save-inspector.md`（✅ 完成） |
| 三平台 CI 现状 | `2026-10-13-release-engineer-ci-triplatform.md`（Linux/macOS 观察模式） |
| G3 门禁 | `2026-10-13-orchestrator-gate-g3.md` + `-recheck` |
| 角色约束 | `roles.md` §3.11 |

## 3. 证据锚点

| 证据 | 位置 |
|------|------|
| 打包配置 | `product/CMakeLists.txt` 的 CPack 段 |
| 打包脚本 | `product/scripts/package.sh` |
| 发布检查单 | `product/docs/design/release-checklist.md` |
| 产物级 E2E | `product/tests/e2e/test_package_artifact.py` |
| 产物引用 | 本文件 |

## 4. 关键决策

| 决策 | 理由 | 被否决的备选 |
|------|------|--------------|
| macOS 用 `DragNDrop` 而非 `DMG` 作基线 | DMG 需要 `hdiutil` **且**未签名会被 Gatekeeper 直接拦；`.app` 拖放包是无需签名即可分发的最小可用格式 | 直接上 DMG（本环境无 macOS，配置了也无法验证，属「写了但没验」） |
| Linux 同时配 `DEB` 与 `TGZ` | DEB 需 `dpkg-shlibdeps`，TGZ 只要 `make install`；两者覆盖不同发行版习惯 | 只配 DEB（Arch/Fedora 用户无解） |
| 打包脚本内建「先测试再打包」 | `roles.md` §3.11「发布前必须过 G3 门禁」。把它写进脚本而不是靠人记得，才不会漏 | 依赖操作者自觉（一定会有人跳过） |
| 产物 E2E 找不到产物时 **skip 而非 fail** | 日常只跑单测的场景不该因没打包而红。但「打包后忘了验证」仍是可发现的：CI 里产物存在时该用例会真跑 | 缺产物直接失败（会阻断只跑单测的流程） |
| 检查单不判全绿 | `roles.md` §3.11 要求「签名有效、更新通道可用」。这两项因缺证书/缺设计而无法完成 | 勾上「已完成」让检查单好看（等于把未做的事写成做了） |

## 5. 本轮修复的真实缺陷

**打包产物混入测试框架** —— googletest 自带 install 规则，实测会把
`include/gmock/*.h` 等文件全部装进用户安装目录：

```
-- Installing: .../stage_probe/include/gmock/gmock.h
-- Installing: .../stage_probe/include/gmock/internal/custom/gmock-port.h
...
```

同时 `CPACK_COMPONENTS_ALL` 变为 `"Unspecified;gmock;gtest"`，即 gtest/gmock
作为独立打包组件进入了安装包。

**修复**：在 `FetchContent_MakeAvailable(googletest)` **之前**设置
`INSTALL_GTEST OFF`。修复后实测：打包组件仅剩 `Unspecified`，
安装树与 ZIP 均无 gtest/gmock。

> 这正是 `roles.md` §3.11「Python 内部工具链不进安装包」那条约束的同类问题——
> 约束写在纸上，但没人验证打包产物里到底有什么。本轮是第一次真的解开 ZIP 看，
> 才发现测试框架也在里面。

## 6. 实测记录（非纸面声明）

| 步骤 | 结果 |
|------|------|
| CPack 生成 NSIS | `save-inspector-0.1.0-Windows.exe`，355.9 KB ✅ |
| CPack 生成 ZIP | `save-inspector-0.1.0-Windows.zip`，193.6 KB ✅ |
| ZIP 内容检查 | `bin/{save_inspector_ui.exe, save_inspector_cli.exe, web/*}` + `include/` + `lib/`；**无 `.py`、无 gtest/gmock** ✅ |
| 解压后直接启动 | 从解压目录启动成功，**未传 `--web-root`** 仍正确返回 `/`、`/app.js`、`/styles.css`（均 200）✅ |
| 首屏内容 | 响应含「存档查看器」与「只读取，不修改」✅ |
| 产物级端到端 | `test_package_artifact.py` 2/2 通过：界面资源 → 打开真实存档 → 列出 3 槽位 → 详情（第 42 章）→ 导出报告 ✅ |

## 7. 风险与阻塞

| # | 阻塞项 | 原因 | 解阻条件 |
|---|-------|------|---------|
| 1 | Linux / macOS 未编译未测试 | 本环境无该平台；CI 已配置为观察模式，结论未知 | CI 两项转绿 |
| 2 | macOS / Linux 产物未生成未冒烟 | 同上 | 有该平台环境 |
| 3 | 全部代码签名与公证未做 | 缺证书 / 缺 Apple 开发者账号 | 采购证书并在 CI 配置凭据 |
| 4 | 自动更新通道未做 | 需要分发端点与信任链设计，属独立议题 | 独立立项 |

### 7.1 关于第 4 项的澄清

自动更新**不是漏做，是明确未做**：当前仓库中不存在任何自动更新代码，
因此不存在「有更新逻辑但没验证」的风险。这一点与「做了但没验」性质不同，
已在检查单 §3.4 单独标注。

## 8. 结论

| 项 | 内容 |
|------|------|
| 角色级裁决 | **❌ 不满足发布完成标准** |
| 依据 | `roles.md` §3.11：「三平台安装包冒烟通过（安装→首启→出结果）；签名有效、更新通道可用；检查单全绿」——三项均未满足 |
| 实际达成 | Windows 侧从构建到打包到产物级端到端**全链路已验证**；产品可作为内部/试用版本分发 |
| 备注 | 剩下的阻塞全部依赖**证书、账号或异构环境**，不是继续写代码能解决的。这是产品决策点（是否投入签名成本、是否先试用分发），已列入检查单的三个处置选项 |

## 9. 移交

| 项 | 内容 |
|------|------|
| 交给 | `orchestrator`（G4 裁决） |
| G4 裁决所需 | 检查单 §6 的总结与三个处置选项 |
| 提醒 | 按门禁条文，G4 **不应判定为通过**；是否以「试用分发」名义放行属产品决策 |