# ============================================================================
# 垃圾清理工具 GUI —— 基于 NIM Duilib（本地源码直接编译，不使用 vcpkg / conan）
#
# 依赖获取方式：
#   Duilib 框架源码随本仓库携带，位于 thirdparty/NIM_Duilib_Framework-1.0.0
#   （来源：NIM_Duilib_Framework-1.0.0.zip 解压）
#   本文件把 base / duilib 两个静态库当作普通 CMake 目标从源码编译，
#   不需要 conan、vcpkg、find_package 或任何预编译产物。
#
# 用法（Windows + MSVC）：
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
# 详细说明与验证方式见 README.md
# ============================================================================

cmake_minimum_required(VERSION 3.20)

project(GarbageCleanerGUI VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "NIM Duilib 是 Windows 专用界面库，本项目只支持 Windows 构建。")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 所有目标（含框架的两个静态库）都带调试符号：崩溃时 cdb/WinDbg 才能给出
# 函数名与行号，否则只能看到「某个地址 + offset」。优化级别不受影响（等同于
# CMake 的 RelWithDebInfo 做法），.pdb 只留在构建目录，不进 install 产物。
if(MSVC)
    add_compile_options(/Zi)
    add_link_options(/DEBUG:FULL)
endif()

# Duilib 上游工程使用静态 CRT（/MT、/MTd），这里保持一致，避免 LNK2038 运行库不匹配。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 允许覆盖框架路径（例如把源码放在别处时：-DNIM_DUILIB_DIR=D:/libs/NIM_Duilib_Framework-1.0.0）
set(NIM_DUILIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/NIM_Duilib_Framework-1.0.0"
    CACHE PATH "NIM Duilib 框架源码根目录")

if(NOT EXISTS "${NIM_DUILIB_DIR}/duilib/UIlib.h")
    message(FATAL_ERROR
        "在 ${NIM_DUILIB_DIR} 未找到 NIM Duilib 源码（缺少 duilib/UIlib.h）。\n"
        "请确认 thirdparty/NIM_Duilib_Framework-1.0.0 已解压，"
        "或用 -DNIM_DUILIB_DIR=<框架源码根目录> 指定。")
endif()

# ---------------------------------------------------------------------------
# 框架公共编译设置
#
# 上游 .vcxproj 的事实：Unicode 字符集、静态运行库、包含目录为「框架根 + duilib」。
# 这里用 INTERFACE 目标集中表达，避免两个库和 App 三处重复。
# ---------------------------------------------------------------------------
add_library(nim_duilib_options INTERFACE)

target_include_directories(nim_duilib_options INTERFACE
    "${NIM_DUILIB_DIR}"          # 让 "base/base.h"、"duilib/UIlib.h"、"build/build_config.h" 可解析
    "${NIM_DUILIB_DIR}/duilib"   # 让 duilib 内部 "Utils/Macros.h"、"Core/Window.h" 等可解析
)

target_compile_definitions(nim_duilib_options INTERFACE
    UNICODE
    _UNICODE
    WIN32
    _WINDOWS
    # 源码是 2013 年前后的代码，大量使用已被标记为不安全的 CRT 函数与宽字符 API
    _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE
    # 上游默认把 WINVER 压到 WinXP；现代 SDK 已不保证该目标，这里抬到 Win7，
    # 与 GetTickCount64、DPI API 等实现相符（winsdk_config.h 只在未定义时兜底）。
    WINVER=0x0601
    _WIN32_WINNT=0x0601
)

# duilib/StdAfx.h 会在包含 GdiPlus.h 之前定义 GDIPVER=0x0110；
# 这里显式定义，保证不经 StdAfx.h 的翻译单元也拿到一致的 GDI+ 头。
target_compile_definitions(nim_duilib_options INTERFACE GDIPVER=0x0110)

# ---------------------------------------------------------------------------
# nbase —— Google base 精简版（上游 base.vcxproj）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE NBASE_SOURCES CONFIGURE_DEPENDS
    "${NIM_DUILIB_DIR}/base/*.cpp"
    "${NIM_DUILIB_DIR}/base/*.cc"
    "${NIM_DUILIB_DIR}/base/*.c"
)
# 上游 base.vcxproj 未编译该文件：它是依赖 gtest / base/util/bind.h 的单元测试，不是库的一部分。
list(FILTER NBASE_SOURCES EXCLUDE REGEX "callback_unittest\\.cpp$")

add_library(nbase STATIC ${NBASE_SOURCES})
target_link_libraries(nbase PUBLIC nim_duilib_options)

# 上游 base.vcxproj 定义了 NOMINMAX：base 里存在 traits::min()/traits::max() 这类
# 成员函数调用，被 windows.h 的 min/max 宏吃掉后会直接编译失败。
# 这里必须是 PRIVATE —— duilib 反而依赖 windows.h 的 min/max 宏（见下）。
target_compile_definitions(nbase PRIVATE NOMINMAX)

# base 有相当多的 POSIX 分支（file_util_posix.cpp 等），在 Windows 上靠内部
# #if defined(OS_POSIX) 编译成空翻译单元，不需要额外排除。
set_target_properties(nbase PROPERTIES
    CXX_STANDARD 14 # Markup.cpp 等仍在使用 C++17 已移除的 register 关键字
    CXX_EXTENSIONS OFF
)

# ---------------------------------------------------------------------------
# duilib —— 界面库本体（上游 duilib.vcxproj 的 45 个源文件）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DUILIB_SOURCES CONFIGURE_DEPENDS
    "${NIM_DUILIB_DIR}/duilib/*.cpp"
)
# UIlib.cpp 只有 DllMain，属于「以 DLL 形式使用」时才需要的实现。
# 本工程静态链接进 exe，留着会在可执行文件里留一个无意义的 DllMain。
list(FILTER DUILIB_SOURCES EXCLUDE REGEX "/UIlib\\.cpp$")

add_library(duilib STATIC ${DUILIB_SOURCES})
target_link_libraries(duilib PUBLIC nbase nim_duilib_options)

set_target_properties(duilib PROPERTIES
    CXX_STANDARD 14
    CXX_EXTENSIONS OFF
)

# duilib 刻意不定义 NOMINMAX：duilib/StdAfx.h 用 "#define MAX max / #define MIN min"
# 支撑源码里的 MAX(...)/MIN(...) 写法，这依赖 windows.h 提供的 min/max 宏。

# 上游每个 .cpp 都会自己 #include "StdAfx.h"，因此这里不需要预编译头；
# 保持默认（不使用 PCH）可少一层与 CMake 生成头文件名相关的耦合。

# duilib 需要的系统库（上游 duilib.vcxproj 的 AdditionalDependencies）
target_link_libraries(duilib PUBLIC
    comctl32    # InitCommonControls / 通用控件
    gdiplus     # 渲染后端
    msimg32     # AlphaBlend / TransparentBlt
    winmm       # timeBeginPeriod（nbase 计时）
    imm32       # 输入法（RichEdit 等控件）
    shlwapi     # 路径工具
    comdlg32    # GetOpenFileName
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    shell32
    ws2_32      # base/network
)

# ---------------------------------------------------------------------------
# 应用
#
# 应用侧不定义 NOMINMAX（原因见下面应用目标处的说明），
# 只额外补一个 WIN32_LEAN_AND_MEAN 避免 winsock 头冲突。
# ---------------------------------------------------------------------------
add_executable(garbage_cleaner_gui
    src/main.cpp
    src/gc_log.h
    src/gc_log.cpp
    src/gc_scanner.h
    src/gc_scanner.cpp
    src/gc_main_form.h
    src/gc_main_form.cpp
)

set_target_properties(garbage_cleaner_gui PROPERTIES
    CXX_STANDARD 17      # 应用代码可以用新标准，框架库仍留在 C++14
    CXX_EXTENSIONS OFF
)

target_link_libraries(garbage_cleaner_gui PRIVATE duilib)
# 应用侧不能定义 NOMINMAX：GDI+ 的 SDK 头（GdiplusTypes.h）直接用 min()/max()，
# 关掉 windows.h 的 min/max 宏反而会让它编译失败。
#
# 但要定义 WIN32_LEAN_AND_MEAN：应用源码通常先包 <windows.h>，
# 若不瘦身，windows.h 会拉进旧版 winsock.h，与 base/network 的 winsock2.h 撞车
# （sockaddr / fd_set 等大量重定义）。框架自己的 build/winsdk_config.h 也是这么做的。
target_compile_definitions(garbage_cleaner_gui PRIVATE WIN32_LEAN_AND_MEAN)
# 源码里含中文注释与中文字符串，显式按 UTF-8 解释源文件
target_compile_options(garbage_cleaner_gui PRIVATE /utf-8)

# 框架头文件里的中文注释是 GBK（base/encrypt/des.h 等 102 个文件都是），
# 用 /utf-8 去编译它们必然报 C4828。框架库自己不带 /utf-8 所以不受影响，
# 这里只为 App 关掉这条「/utf-8 vs 上游注释编码」的噪音告警。
target_compile_options(garbage_cleaner_gui PRIVATE /wd4828)

# 窗口用 wWinMain（配合 UNICODE 的 WinMain 入口），与 upstream examples 一致
target_link_options(garbage_cleaner_gui PRIVATE /SUBSYSTEM:WINDOWS)



# ---------------------------------------------------------------------------
# 运行时资源
#
# NIM Duilib 通过 GlobalManager::Startup(resourcePath, ...) 定位皮肤：
#   <resourcePath>\themes\<theme>\global.xml 与 <resourcePath>\lang\zh_CN\gdstrings.ini
# 因此把框架自带的 bin/resources 拷到 exe 同级的 resources\，
# 再把本项目的主题目录叠加进去。
# ---------------------------------------------------------------------------
set(APP_RESOURCE_DIR "$<TARGET_FILE_DIR:garbage_cleaner_gui>/resources")

add_custom_command(TARGET garbage_cleaner_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${NIM_DUILIB_DIR}/bin/resources"
        "${APP_RESOURCE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "${APP_RESOURCE_DIR}"
    COMMENT "复制 Duilib 资源与本项目主题到输出目录"
)

# 让产物目录也可以被 Visual Studio / IDE 直接 F5
set_target_properties(garbage_cleaner_gui PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:garbage_cleaner_gui>"
)

install(TARGETS garbage_cleaner_gui RUNTIME DESTINATION bin)
install(DIRECTORY "${NIM_DUILIB_DIR}/bin/resources/" DESTINATION bin/resources)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/" DESTINATION bin/resources)
