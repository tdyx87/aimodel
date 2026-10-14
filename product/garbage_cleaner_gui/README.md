# 垃圾清理工具 GUI（NIM Duilib）

一个 Windows 桌面小工具：选择一个目录 → 扫描出可清理的垃圾文件（构建产物 / 临时缓存 / 日志）
→ 勾选结果列表 → 确认后删除，并把释放的空间报给用户。

界面基于 **NIM Duilib**（网易云信的 duilib 分支）。框架源码**随仓库携带、直接参与编译**：
不需要 vcpkg，不需要 conan，也不依赖任何预编译库。

---

## 1. 构建

前置条件：Windows + Visual Studio 2022（含「使用 C++ 的桌面开发」）+ CMake ≥ 3.20。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build/Release/garbage_cleaner_gui.exe`，同级目录已经带好 `resources\`（构建后自动拷贝）。

直接运行（可选地指定初始扫描目录，便于脚本化）：

```powershell
.\build\Release\garbage_cleaner_gui.exe
.\build\Release\garbage_cleaner_gui.exe "D:\temp\build-cache"
```

> 只有 `NIM_Duilib_Framework-1.0.0.zip` 而没有解压时：
> 解压到 `thirdparty/NIM_Duilib_Framework-1.0.0/`，
> 或用 `-DNIM_DUILIB_DIR=<框架源码根目录>` 指到别处（该目录下应能看到 `duilib/UIlib.h`）。

## 2. 目录结构

```
garbage_cleaner_gui/
├─ CMakeLists.txt                  # 一处定义三个目标：nbase、duilib、App
├─ thirdparty/
│   └─ NIM_Duilib_Framework-1.0.0/ # 框架源码原样存放（未改动上游文件）
├─ src/
│   ├─ main.cpp                    # 入口：消息循环 + 资源目录 + 命令行参数
│   ├─ gc_log.{h,cpp}              # 极简日志（%TEMP%\garbage_cleaner_gui.log）+ 崩溃记录
│   ├─ gc_scanner.{h,cpp}          # 扫描/删除逻辑（纯 Win32，不依赖界面，可单测）
│   └─ gc_main_form.{h,cpp}        # 主窗口：控件绑定、扫描工作线程、结果列表
├─ resources/themes/default/garbage_cleaner/
│   ├─ main_layout.xml             # 主窗口皮肤（标题栏/工具行/表头/列表/底部）
│   └─ list_item.xml               # 结果列表的行模板
└─ tools/
    └─ smoke_run.ps1               # 端到端冒烟测试（真实点击，见第 4 节）
```

框架的两个静态库（`nbase`、`duilib`）由 CMake 直接用 `file(GLOB_RECURSE)` 收集源码编译，
保持上游的字符集 / 静态 CRT / 包含目录设置（集中在 `nim_duilib_options` 这个 INTERFACE 目标里）。
产物一律带调试符号（`/Zi` + `/DEBUG:FULL`），崩溃时能直接定位到行。

## 3. 已知的框架坑（踩过的都记在这里）

1. **`WindowBuilder` 不能跨行复用**（否则第二行必崩 `0xC0000005`）。
   `Create()` 内部每次都会 `CMarkup::LoadFromFile()`，而它先调 `Release()`；
   上游 `CMarkup::Release()`（`duilib/Core/Markup.cpp`）只把 `m_pstrXML` / `m_pElements` 置空，
   **漏了把 `m_nElements` 归零**（那行写成了一个空语句 `m_nElements;`）。
   于是第二次解析时 `_ReserveElement()` 认为"还有额度"、不再 `realloc`，
   返回 `NULL + nElements` 的野指针，写进去就崩。→ 每行新建一个 `WindowBuilder`。
2. **列表要用 `VListBox` / `HListBox`，不能用 `ListBox`**。
   框架里 `ListBox` 绑的是基类 `Layout`，它只给 float 子项定位；
   普通行永远拿不到位置和尺寸（现象是"数据加进去了但界面上什么都没有"）。
   `VListBox` 绑的是 `VLayout`，才会逐行纵向排列并撑满宽度。
3. 按名字找行内子控件必须**递归**：名字挂在行内 `HBox` 的子控件上，
   只遍历一层会全部返回 `nullptr`（现象是行渲染出来了但全是空白）。
   也可以等控件绑定窗口后用 `Box::FindSubControl()`（内部走 `Window::FindSubControlByName`）。
4. XML 根节点不是 `Window` 时，`WindowBuilder` 会退化成"取根节点的第一个子节点 + 用户提供的容器"，
   这正是行模板需要的语义（见 `list_item.xml`）。

## 4. 验证

```powershell
# 端到端冒烟测试：真实鼠标点击，不看断言看结果
powershell -ExecutionPolicy Bypass -File tools/smoke_run.ps1 -Config Release
```

它做的事：造一个临时"垃圾目录"（3 个待清理文件 + 1 个需保留文件）→ 启动 exe →
按**贴图填充色**定位并点击「开始扫描」→ 断言列表区域像素发生变化（结果真的画出来了）→
点击「清理」→ 等原生的确认对话框出现（按进程号找 `#32770`）→ 发 `IDOK` →
断言 3 个文件已从磁盘删除、`keep.txt` 仍在。

> 脚本里的 PowerShell 文件必须保存为 **UTF-8 with BOM**：Windows PowerShell 5.1 在无 BOM 时
> 按系统 ANSI（简中 GBK）解码，中文会直接语法报错。

运行期日志在 `%TEMP%\garbage_cleaner_gui.log`（UTF-8）：启动参数、扫描汇总、清理结果，
以及未处理异常的异常码与地址——用户报"点了没反应"时，把这个文件发过来即可定位。

## 5. 安全边界

* 只删**文件**，不删目录；`DeleteFileSafely()` 逐个再校验一次。
* 受保护路径前缀（`\Windows\`、`\Program Files\`、`\System Volume Information\` 等）直接跳过。
* 删除前做"是否仍在扫描根目录之下"的前缀校验，防止界面传入意外路径。
* 不跟随符号链接 / 目录联接点（`FILE_ATTRIBUTE_REPARSE_POINT`），避免死循环与跨盘越界。
* 清理前弹原生确认框，明确"删除后不可撤销"。
