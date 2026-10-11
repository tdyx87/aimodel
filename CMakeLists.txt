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
    src/cpp/http_server.cpp)
# 平台差异实现（编译期隔离）
if (WIN32)
    target_sources(aimodel PRIVATE src/cpp/system_info_win32.cpp src/cpp/http_server_win32.cpp)
    target_link_libraries(aimodel PRIVATE ws2_32 winspool)
elseif (APPLE)
    target_sources(aimodel PRIVATE src/cpp/system_info_apple.cpp src/cpp/http_server_posix.cpp)
else ()
    target_sources(aimodel PRIVATE src/cpp/system_info_posix.cpp src/cpp/http_server_posix.cpp)
endif ()

# 警告配置（本地构建开墙，CI 可加 -Werror）
if (MSVC)
    target_compile_options(aimodel PRIVATE /W4)
else ()
    target_compile_options(aimodel PRIVATE -Wall -Wextra -Wpedantic)
endif ()

# ---- CLI 工具 ----
if (AIMODEL_BUILD_TOOLS)
    add_executable(system_info_cli tools/system_info_cli.cpp)
    target_link_libraries(system_info_cli PRIVATE aimodel)
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

    include(GoogleTest)
    gtest_discover_tests(system_info_test)
endif ()

# ---- 安装 ----
include(GNUInstallDirs)
install(TARGETS aimodel
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
