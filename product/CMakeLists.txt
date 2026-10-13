# 存档查看器：构建定义
# 规范来源：.role-system/agents.md §3（构建）、§7（C++11 约束与构建细则）

# 最低版本按**实际用到的特性**定：
#   - FetchContent_MakeAvailable → 3.14
#   - ctest --test-dir（scripts/build.*）→ 3.20
# 写低版本会让旧版 CMake 的用户在 configure 或 test 阶段直接失败，
# 且报错指向 CMake 内部而非版本声明本身。
cmake_minimum_required(VERSION 3.20)

project(saveinspector
    VERSION 0.1.0
    DESCRIPTION "Local save-file inspector for players and QA"
    LANGUAGES CXX)

# agents.md §7：强制 C++11、禁止方言扩展
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

option(SAVEINSPECTOR_BUILD_TESTS "Build unit tests" ON)
option(SAVEINSPECTOR_BUILD_TOOLS "Build CLI/UI tools" ON)
option(SAVEINSPECTOR_BUILD_SHARED "Build shared library" OFF)

# ---- 库目标 ----
if (SAVEINSPECTOR_BUILD_SHARED)
    add_library(saveinspector SHARED)
else ()
    add_library(saveinspector STATIC)
endif ()

target_include_directories(saveinspector
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
# 内部头（save_file.h）只对本库可见，不进 include/ 也不安装。
target_include_directories(saveinspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp)

target_compile_definitions(saveinspector PRIVATE SAVEINSPECTOR_BUILD)
if (SAVEINSPECTOR_BUILD_SHARED)
    target_compile_definitions(saveinspector PUBLIC SAVEINSPECTOR_SHARED)
endif ()

# 平台无关
target_sources(saveinspector PRIVATE
    src/cpp/save_inspector.cpp
    src/cpp/local_http.cpp)
# 平台差异（编译期隔离，agents.md §7.3）
if (WIN32)
    target_sources(saveinspector PRIVATE
        src/cpp/save_file_win32.cpp
        src/cpp/local_http_win32.cpp)
    target_link_libraries(saveinspector PRIVATE ws2_32)
else ()
    target_sources(saveinspector PRIVATE
        src/cpp/save_file_posix.cpp
        src/cpp/local_http_posix.cpp)
endif ()

# 警告与编码：源码为 UTF-8（无 BOM）且含中文注释/字面量。
# MSVC 默认按系统 ANSI 代码页（简中 GBK/936）解码源文件，会把 UTF-8 多字节序列
# 错位配对进而吞掉后续字节、破坏 #if/#endif 结构（曾导致 C1020 无法编译）。
# /utf-8 同时固定源码与执行字符集：HTTP 响应声明 charset=utf-8，按 GBK 编码浏览器会乱码。
if (MSVC)
    set(SAVEINSPECTOR_WARN_FLAGS /utf-8 /W4)
else ()
    set(SAVEINSPECTOR_WARN_FLAGS -Wall -Wextra -Wpedantic)
endif ()

target_compile_options(saveinspector PRIVATE ${SAVEINSPECTOR_WARN_FLAGS})

# ---- MinGW/GCC 可执行文件的自包含性（必做，非优化）----
# GCC 默认**动态**链接 libstdc++/libgcc，运行时由加载器按 PATH（及同目录）顺序
# 解析 libstdc++-6.dll。只要 PATH 上先出现另一个 GCC 发行版的同名 DLL——
# 典型如 Git for Windows 的 mingw64/bin——进程就会加载 ABI 不匹配的运行库。
#
# 实测后果（本机窗口，退出码 0xC0000005）：首个**静态资源**请求即在 libstdc++
# 内 SIGSEGV，服务进程直接死亡，界面对用户完全不可用；而 API 路径恰好不崩，
# 于是「接口测得通」掩盖了「页面打不开」。同一份二进制换台机器/换个 PATH
# 顺序就换一个结果，是最难查的一类缺陷。
#
# 静态链接 libgcc/libstdc++ 后产物自包含，不再依赖 PATH 上恰好是构建时的
# 那一份 DLL。libwinpthread-1.dll 仍为动态（该工具链只提供 DLL，无静态库），
# 但它是稳定的 C API，不受 C++ ABI 差异影响。
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# ---- 工具 ----
if (SAVEINSPECTOR_BUILD_TOOLS)
    add_executable(save_inspector_cli tools/save_inspector_cli.cpp)
    target_link_libraries(save_inspector_cli PRIVATE saveinspector)
    target_compile_options(save_inspector_cli PRIVATE ${SAVEINSPECTOR_WARN_FLAGS})

    # 产品入口：本地服务 + 打开浏览器（PRD NFR-1 首屏 ≤ 2 秒）
    add_executable(save_inspector_ui tools/save_inspector_ui.cpp)
    target_link_libraries(save_inspector_ui PRIVATE saveinspector)
    target_compile_options(save_inspector_ui PRIVATE ${SAVEINSPECTOR_WARN_FLAGS})

    # 界面为免构建静态资源（web/），随可执行文件就位。
    # 刻意不用 POST_BUILD copy_directory：它是「目标附带步骤」，不构成独立构建
    # 规则，源文件变化不触发重跑（改前端但 build/web 仍是旧副本，已发生过）。
    # 改用独立 custom target + ALL：恒定过期，每次构建都同步。
    file(GLOB_RECURSE SAVEINSPECTOR_WEB_FILES "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
    add_custom_target(saveinspector_web_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/web
                $<TARGET_FILE_DIR:save_inspector_ui>/web
        DEPENDS ${SAVEINSPECTOR_WEB_FILES}
        COMMENT "同步界面资源 web/ 到可执行文件同级目录"
        VERBATIM)
    add_dependencies(saveinspector_web_assets save_inspector_ui)
endif ()

# ---- 测试（GTest 固定版本，agents.md §7.4）----
if (SAVEINSPECTOR_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    # Windows：gtest 与被测库使用同一 CRT
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(save_inspector_test tests/save_inspector_test.cpp)
    target_link_libraries(save_inspector_test PRIVATE saveinspector GTest::gtest_main)
    target_compile_options(save_inspector_test PRIVATE ${SAVEINSPECTOR_WARN_FLAGS})

    # HTTP 层单测：起真实本地服务 + 裸 socket 客户端。
    # 闸门（Host/Origin/Content-Type）、请求体截断、状态码无法在库层断言。
    add_executable(local_http_test tests/local_http_test.cpp)
    # 静态资源分支需要一个**真实存在**的 web 目录才能被覆盖：此前夹具一律
    # 清空 static_root，于是「配了 static_root 的静态资源」长期零覆盖。
    # 直接指向源码树里的 web/，不拷贝、不造临时目录。
    target_compile_definitions(local_http_test PRIVATE
        SAVEINSPECTOR_TEST_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
    if (WIN32)
        target_link_libraries(local_http_test PRIVATE saveinspector GTest::gtest_main ws2_32)
    else ()
        target_link_libraries(local_http_test PRIVATE saveinspector GTest::gtest_main)
    endif ()
    target_compile_options(local_http_test PRIVATE ${SAVEINSPECTOR_WARN_FLAGS})

    include(GoogleTest)
    gtest_discover_tests(save_inspector_test)
    gtest_discover_tests(local_http_test)
endif ()

# ---- 安装 ----
include(GNUInstallDirs)
install(TARGETS saveinspector
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# 工具与界面资源：web/ 必须与可执行文件同级（入口按相对路径 "web" 查找），
# 否则安装后启动只会得到「界面资源缺失」。
if (SAVEINSPECTOR_BUILD_TOOLS)
    install(TARGETS save_inspector_cli save_inspector_ui
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_BINDIR}/web)
endif ()
