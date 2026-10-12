# aimodel：跨平台系统信息库（C++11）
# 规范来源：agents.md 第 3 节（构建）、第 4 节（跨平台）、第 6 节（C++11 约束）

cmake_minimum_required(VERSION 3.10)

project(aimodel
    VERSION 0.1.0
    DESCRIPTION "Cross-platform system information library (CPU/GPU/Memory/Disk/Printers)"
    LANGUAGES CXX)

# agents.md 第 3 节：强制 C++11、禁止方言扩展
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

option(AIMODEL_BUILD_TESTS "Build unit tests" ON)
option(AIMODEL_BUILD_TOOLS "Build CLI tools" ON)
option(AIMODEL_BUILD_SHARED "Build shared library" OFF)

# ---- 库目标 ----
if (AIMODEL_BUILD_SHARED)
    add_library(aimodel SHARED)
else ()
    add_library(aimodel STATIC)
endif ()

# 对外头文件目录
target_include_directories(aimodel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# agents.md 第 9 节：AIMODEL_BUILD 控制 dllexport；AIMODEL_SHARED 标记共享库形态
# （后者须 PUBLIC 传递给消费方，静态库下两者均不启用导出语义）
target_compile_definitions(aimodel PRIVATE AIMODEL_BUILD)
if (AIMODEL_BUILD_SHARED)
    target_compile_definitions(aimodel PUBLIC AIMODEL_SHARED)
endif ()

# agents.md 第 4 节：平台差异实现按平台选择源文件，编译期隔离
# 共同部分（三平台）
target_sources(aimodel PRIVATE
    src/cpp/system_info.cpp
    src/cpp/http_server.cpp
    src/cpp/file_transfer.cpp
    src/cpp/chat_suggest.cpp)
# 平台差异实现（编译期隔离）
if (WIN32)
    target_sources(aimodel PRIVATE src/cpp/system_info_win32.cpp src/cpp/http_server_win32.cpp)
    target_link_libraries(aimodel PRIVATE ws2_32 winspool)
elseif (APPLE)
    target_sources(aimodel PRIVATE src/cpp/system_info_apple.cpp src/cpp/http_server_posix.cpp)
else ()
    target_sources(aimodel PRIVATE src/cpp/system_info_posix.cpp src/cpp/http_server_posix.cpp)
endif ()

# 警告与编码配置（G3 质量门禁：本项目全部目标零警告）
# 源码为 UTF-8（无 BOM）且含中文注释/字面量。MSVC 默认按系统 ANSI 代码页
# （简中环境为 GBK/936）解码源文件，会把 UTF-8 多字节序列错位配对，
# 进而吞掉后续字节、破坏 #if/#endif 结构（曾导致 C1020「意外的 #endif」而无法编译）。
# /utf-8 同时固定源码与执行字符集，保证中文字面量按 UTF-8 进入二进制
# （HTTP 响应头声明 charset=utf-8，若按 GBK 编码浏览器会乱码）。
if (MSVC)
    set(AIMODEL_WARN_FLAGS /utf-8 /W4)
else ()
    set(AIMODEL_WARN_FLAGS -Wall -Wextra -Wpedantic)
endif ()

target_compile_options(aimodel PRIVATE ${AIMODEL_WARN_FLAGS})

# ---- CLI 工具 ----
if (AIMODEL_BUILD_TOOLS)
    add_executable(system_info_cli tools/system_info_cli.cpp)
    target_link_libraries(system_info_cli PRIVATE aimodel)
    target_compile_options(system_info_cli PRIVATE ${AIMODEL_WARN_FLAGS})

    # docs/design/http-ui.md 第 4 节：产品入口（本地服务 + 打开浏览器）
    add_executable(system_info_ui tools/system_info_ui.cpp)
    target_link_libraries(system_info_ui PRIVATE aimodel)
    target_compile_options(system_info_ui PRIVATE ${AIMODEL_WARN_FLAGS})

    # 界面为免构建静态资源（web/），随可执行文件就位，双击即可打开。
    #
    # 为什么不能只用 POST_BUILD 的 copy_directory：实测（改 web/app.js 后构建）
    # ninja 报 "no work to do"——POST_BUILD 是「目标附带步骤」，不构成独立构建
    # 规则，源文件变化不会触发重跑。结果是改了前端但 build/web 仍是旧副本，
    # 跑起来看不到任何变化（已发生过，用户以为前端没生效）。
    #
    # 改用独立 custom target + ALL：custom target 恒定过期，每次构建都会同步
    # web/ 到可执行文件同级目录，保证副本与源一致。
    #
    # 刻意不用的两个选项（都会破坏 CMake 3.10 下限）：
    #   - CONFIGURE_DEPENDS（需 3.12）
    #   - copy_directory_if_different（需 3.26）
    # web/ 仅 3 个小文本文件，每次 copy_directory 的开销可忽略。
    file(GLOB_RECURSE AIMODEL_WEB_FILES "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
    add_custom_target(aimodel_web_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/web
                $<TARGET_FILE_DIR:system_info_ui>/web
        DEPENDS ${AIMODEL_WEB_FILES}
        COMMENT "同步界面资源 web/ 到可执行文件同级目录"
        VERBATIM)
    add_dependencies(aimodel_web_assets system_info_ui)
endif ()

# ---- 测试（GTest 固定版本，agents.md 第 8.4 条）----
if (AIMODEL_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    # Windows：gtest 与被测库使用同一 CRT
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(system_info_test tests/system_info_test.cpp)
    target_link_libraries(system_info_test PRIVATE aimodel GTest::gtest_main)
    target_compile_options(system_info_test PRIVATE ${AIMODEL_WARN_FLAGS})

    # 文件传输（file_transfer）单测：覆盖路径穿越、保留名、危险扩展名、大小上限
    add_executable(file_transfer_test tests/file_transfer_test.cpp)
    target_link_libraries(file_transfer_test PRIVATE aimodel GTest::gtest_main)
    target_compile_options(file_transfer_test PRIVATE ${AIMODEL_WARN_FLAGS})

    # 聊天/推荐（chat_suggest）单测：覆盖空 query 拒绝、分类不变量、JSON 契约字段
    add_executable(chat_suggest_test tests/chat_suggest_test.cpp)
    target_link_libraries(chat_suggest_test PRIVATE aimodel GTest::gtest_main)
    target_compile_options(chat_suggest_test PRIVATE ${AIMODEL_WARN_FLAGS})

    include(GoogleTest)
    gtest_discover_tests(system_info_test)
    gtest_discover_tests(file_transfer_test)
    gtest_discover_tests(chat_suggest_test)
endif ()

# ---- 安装 ----
include(GNUInstallDirs)
install(TARGETS aimodel
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
