# aimodel

**aimodel 是一款 AI 产品：它自动感知你的电脑硬件（CPU、显卡、内存、硬盘等），并据此自动为你选好合适的运行方式与模型大小——不用手动调设置，打开就能用。**

## 当前可用功能：系统信息报告

aimodel 的「硬件自适应」能力已经可用：它会检测你电脑的以下信息，生成一份本机硬件报告——

- **CPU（处理器）**：型号、核心数、主频
- **GPU（显卡）**：型号与显存大小
- **内存**：总量与当前可用量
- **硬盘**：每个分区的容量与剩余空间
- **打印机**：已安装的打印机、默认打印机与状态

这份报告就是自动适配的依据：aimodel 会根据你的实际配置，**自动选择用显卡还是 CPU 来运行 AI、匹配合适大小的模型**——显卡强就用显卡加速，显存小就自动换用更轻量的模型，全程无需你操心。

## 快速开始：跑出你的第一份报告

### Windows

在仓库目录打开 PowerShell，执行构建命令：

```powershell
./scripts/build.ps1 -Config Release
```

构建完成后，运行报告工具：

```powershell
./build/system_info_cli.exe
```

### macOS / Linux

在仓库目录打开终端，执行构建命令：

```bash
./scripts/build.sh Release
```

构建完成后，运行报告工具：

```bash
./build/system_info_cli
```

> 构建脚本会自动完成编译与自检，最后看到 `Build OK` 即表示成功。若提示找不到命令，说明本机尚缺基础构建环境，可参见文末「开发者须知」。

### 运行后会看到什么

终端会直接打印出一份 JSON 格式的硬件报告（下面是一份节选示例，每个列表仅保留 1 条，实际会列出你的全部显卡、分区与打印机）：

```json
{
  "os_name": "Windows 10 Pro",
  "os_version": "22H2 (19045)",
  "architecture": "x86_64",
  "hostname": "MY-PC",
  "cpus": [
    {
      "vendor": "GenuineIntel",
      "brand": "Intel(R) Core(TM) i5-10500 CPU @ 3.10GHz",
      "core_count": 12,
      "physical_core_count": 12,
      "frequency_mhz": 3096
    }
  ],
  "gpus": [
    { "vendor": "AMD", "name": "AMD Radeon R7 430", "video_memory_bytes": 2147483648 }
  ],
  "memory": { "total_bytes": 16994660352, "available_bytes": 3172605952 },
  "disks": [
    { "mount_point": "C:\\", "file_system": "NTFS", "total_bytes": 255113695232, "free_bytes": 257372160 }
  ],
  "printers": [
    { "name": "GDI PDF Printer", "is_default": true, "status": "idle" }
  ]
}
```

两个小提示：

- 想要单行输出（方便保存或发送），加 `--compact`：`./build/system_info_cli.exe --compact`
- 想看全部可用选项，加 `--help`

## 输出说明：每个字段是什么意思

报告中所有以 `_bytes` 结尾的数字，单位都是**字节**（1 GB ≈ 1073741824 字节）；数值为 `0` 或字符串为空，表示该项**未能读到**，并不代表硬件不存在。

| 字段 | 含义 | 对你意味着什么 |
|------|------|----------------|
| `os_name` / `os_version` | 操作系统名称与版本 | 确认软件是否与你的系统兼容 |
| `architecture` | 系统架构（如 `x86_64`、`arm64`） | 决定可用的软件与模型版本 |
| `hostname` | 电脑名称 | 本机的标识 |
| `cpus[].brand` | CPU 型号 | 反映处理器档次 |
| `cpus[].core_count` / `physical_core_count` | 逻辑核心数 / 物理核心数 | 核心越多，同时处理多件事的能力越强 |
| `cpus[].frequency_mhz` | CPU 主频（MHz） | 主频越高，单个任务响应越快 |
| `gpus[].vendor` / `gpus[].name` | 显卡厂商与型号 | 判断你的电脑能否用显卡加速 AI 运行 |
| `gpus[].video_memory_bytes` | **显存** | **决定能运行多大的模型**——显存越大，可运行的模型越大、越流畅 |
| `memory.total_bytes` | 内存总量 | 运行时的临时空间，影响多任务与大模型的加载 |
| `memory.available_bytes` | 当前可用内存 | 数值越小，说明内存此刻越"满" |
| `disks[].mount_point` | 分区位置（如 `C:\`、`/`） | 模型文件会存放的位置 |
| `disks[].file_system` | 分区的文件系统类型（如 NTFS、ext4） | 一般无需关心，用于识别分区格式 |
| `disks[].total_bytes` / `free_bytes` | 分区总容量 / 剩余空间 | **决定能下载并保存多大的模型** |
| `printers[].name` | 打印机名称 | 你的电脑上装了哪些打印机 |
| `printers[].is_default` | 是否为默认打印机 | 哪台打印机是默认的 |
| `printers[].status` | 打印机状态（如 `idle` 空闲） | 当前是否空闲可用 |

## 隐私说明

- 所有硬件检测**全部在你的电脑上完成**；
- 报告只显示在你自己的屏幕上，**不会上传到任何服务器，也不会离开你的本地设备**；
- 生成报告的过程不收集、不发送任何个人信息。

---

## 开发者须知

以下内容面向参与开发的读者，**普通用户无需阅读**：

- [`agents.md`](agents.md) —— 项目总纲：架构、工程规范、构建与测试约定
- [`docs/roles.md`](docs/roles.md) —— 角色体系设计与协作流程
