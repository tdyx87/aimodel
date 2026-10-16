cmake_minimum_required(VERSION 3.15)
project(cagent C)

# 契约：docs/design/contract.md
# 产品定位：极低内存的终端 AI 代理。语言锁 C，不因为"现代语言更好写"而放宽。

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(CAGENT_BUILD_TESTS "构建单元测试" ON)

# 源码编码：源码为 UTF-8，HTTP 响应声明 charset=utf-8。
#
# /utf-8 必须放在 CMAKE_C_FLAGS（命令行靠前位置），不能只写在
# add_compile_options 里（那会落到命令末尾的 AdditionalOptions）。
#
# 关于 /wd4819：本工具链是 VS2017 / MSVC 14.16，它对中文**窄字符串字面量**
# 仍发 C4819（注释里的中文不报，容易误判成"只影响注释"）。已逐项验证：
#   1. 所有源文件均为合法 UTF-8（逐文件校验解码通过）
#   2. /utf-8 确实传到了编译器（见 compile_commands.json）
#   3. 加 BOM、显式 /source-charset+/execution-charset 均无效
#   4. **实测输出字节是合法 UTF-8 且中文正确** —— 说明 /utf-8 实际生效，
#      该告警是此工具链的误报
# 因此只抑制这一条编号，并在注释里留下判断依据。
# 换成较新工具链后可去掉；届时应重新验证而不是默认继承本条。
if (MSVC)
    add_compile_options(/W4 /wd4819)
    string(APPEND CMAKE_C_FLAGS " /utf-8")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- mem：最底层，其余层依赖它 ----
add_library(cagent_mem STATIC src/mem.c)
target_include_directories(cagent_mem PUBLIC include)
# GetProcessMemoryInfo 需要 psapi；它只是导入表项，运行时不增加负担
if (WIN32)
    target_link_libraries(cagent_mem PRIVATE psapi)
endif()

# ---- buf / sse：纯逻辑，不依赖网络 ----
# 分帧与传输刻意分开：混在一起时出了问题分不清是网络还是解析，
# 而且「绝不整块缓冲」这条只能在纯逻辑层被可靠测到。
add_library(cagent_buf STATIC src/buf.c)
target_link_libraries(cagent_buf PUBLIC cagent_mem)

add_library(cagent_sse STATIC src/sse.c)
target_link_libraries(cagent_sse PUBLIC cagent_buf)

# ---- 测试 ----
if (CAGENT_BUILD_TESTS)
    include(CTest)

    # 轻量测试框架：契约 §C7 要求依赖固定，本测试不引入 gtest
    add_library(ca_test STATIC tests/ca_test.c)
    target_include_directories(ca_test PUBLIC tests)
    target_link_libraries(ca_test PUBLIC cagent_mem)

    add_executable(mem_test tests/mem_test.c)
    target_link_libraries(mem_test PRIVATE ca_test)

    add_executable(sse_test tests/sse_test.c)
    target_link_libraries(sse_test PRIVATE ca_test cagent_sse)

    add_test(NAME mem_test COMMAND mem_test)
    add_test(NAME sse_test COMMAND sse_test)
endif()
