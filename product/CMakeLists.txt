cmake_minimum_required(VERSION 3.15)
project(cagent C)

# 契约：docs/design/contract.md
# 产品定位：极低内存的终端 AI 代理。语言锁 C，不因为"现代语言更好写"而放宽。

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# **关掉编译器扩展**。默认是 ON，即实际编译的是 -std=gnu99 ——
# glibc 会通过 _DEFAULT_SOURCE 额外暴露 realpath / kill 等 POSIX 函数，
# 少一个头文件也不报错（只会退化成隐式声明，返回 int，64 位指针被截断）。
# 关掉之后 `-std=c99` 是真的：用 POSIX 就得自己声明依赖。
set(CMAKE_C_EXTENSIONS OFF)
# POSIX 侧显式声明所需标准版本。realpath / fork / pipe / kill 都在其中。
# 这比依赖编译器「顺手给的」更可预测，也说明了这些函数为什么可用。
if(NOT WIN32)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()
# macOS 的「POSIX 模式」和 glibc 不一样：Darwin 把 ru_maxrss 等 BSD 扩展
# 挡在 _DARWIN_C_SOURCE 后面。只给 _POSIX_C_SOURCE 时，
# <sys/resource.h> 里的 struct rusage **没有** ru_maxrss 这个成员
# （Linux 上则有）。于是同一份代码在 Ubuntu 编译通过、在 macOS 编译失败。
# 「严格 POSIX」不是跨 libc 一致的，这一点必须显式处理。
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

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

# ---- json：零分配游标扫描器（契约 C6，不引第三方） ----
add_library(cagent_json STATIC src/json.c)
target_link_libraries(cagent_json PUBLIC cagent_buf)

# ---- session：会话历史（契约 §6） ----
# 历史是本产品里唯一随对话增长的东西，所以它的上限必须长在结构里。
add_library(cagent_session STATIC src/session.c src/session_store.c)
target_link_libraries(cagent_session PUBLIC cagent_json cagent_mem)

# ---- tools：工具层（契约 §5，含路径安全与权限模型） ----
# tools_shell.c 单独成文件：进程创建 / 管道 / 超时 / 目录遍历集中在一处，
# 任何一处出错都可能是安全或挂死问题，混在一个文件里看不出边界。
add_library(cagent_core STATIC src/core.c)
target_include_directories(cagent_core PUBLIC include)
target_link_libraries(cagent_core PUBLIC cagent_ui cagent_llm cagent_tools cagent_json cagent_sse cagent_session)

add_library(cagent_ui STATIC src/ui.c src/ui_term.c)
target_include_directories(cagent_ui PUBLIC include)

add_library(cagent_tools STATIC src/tools.c src/tools_shell.c src/tools_edit.c)
target_link_libraries(cagent_tools PUBLIC cagent_json cagent_buf)

# ---- llm：服务适配（契约 §4.3） ----
add_library(cagent_llm STATIC src/llm.c)
target_link_libraries(cagent_llm PUBLIC cagent_json cagent_http cagent_tools)

# ---- http：传输层 ----
# 传输做成接口（ca_transport_ops）而不是直接调 libcurl：
#   1. 契约 C1 要求「只有流式回调」，做成接口等于结构上强制；
#   2. 传输逻辑可在无网络下被完整测试，不必依赖网络抖动。
# 优先 config 模式：自建 / vcpkg / Conan 提供的是 CURLConfig.cmake，
# 它带着静态链接所需的**传递依赖**（Windows 上用 Schannel 时需要 crypt32、
# secur32 等）。FindCURL 模块只给出库文件本身，静态链接时不知道这些，
# 表现是链接期一堆 LNK2019。
# 找不到 config 再退回模块模式（Linux 发行版常见这种布局）。
find_package(CURL QUIET CONFIG)
if (NOT CURL_FOUND)
    find_package(CURL QUIET MODULE)
endif()
add_library(cagent_http STATIC
    src/transport_mem.c
    src/transport_curl.c)
target_link_libraries(cagent_http PUBLIC cagent_sse)

if (CURL_FOUND)
    target_link_libraries(cagent_http PRIVATE CURL::libcurl)
    target_compile_definitions(cagent_http PRIVATE CAGENT_HAVE_CURL=1)
    # config 模式不一定会设 CURL_VERSION_STRING（模块模式才会）。
    # 直接写 "$()" 会在构建日志里留一个空括号，看起来像是版本探测坏了。
    if (CURL_VERSION_STRING)
        message(STATUS "cagent: 使用 libcurl 传输后端 (${CURL_VERSION_STRING})")
    else()
        message(STATUS "cagent: 使用 libcurl 传输后端")
    endif()
    set(CA_TRANSPORT_BACKEND "libcurl")
else()
    # 刻意**降级而非报错**：缺 libcurl 时其余层仍可构建与测试，
    # 只是运行时会拒绝联网请求并在界面上说明原因。
    # 早前把 NSIS 设为打包硬前置、结果包源一次 504 拖红整条 CI 腿，
    # 那是依赖层级搞错的反面教材——此处不能让可选依赖变成阻塞项。
    message(STATUS "cagent: 未找到 libcurl，仅构建内存传输后端（无法联网）")
    set(CA_TRANSPORT_BACKEND "mem")
endif()

# 机器可读的一行，**必须是纯 ASCII**。
# 实测：Windows 上 CMake 把非 ASCII 输出成 '?'（`cagent: ?????????
# libcurl ?????????`），于是任何按中文去 grep 配置日志的检查在 Windows
# 上**永远匹配不上**——与是否真的找到 curl 毫无关系。
# 这个教训来自 CI 上「libcurl 后端未编入」的误报：那次不是缺依赖，
# 是检查语句本身在非英文区域根本没生效过。
message(STATUS "cagent: CAGENT_TRANSPORT_BACKEND=${CA_TRANSPORT_BACKEND}")

# ---- 工具 ----
# 空闲内存探针：NFR-1 承诺的是常驻占用，而单元测试跑完就退出，
# 测不出那个数字。探针初始化各层后静默待命，供独立脚本采样。
add_executable(mem_probe tools/mem_probe.c)
target_link_libraries(mem_probe PRIVATE cagent_llm)

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

    add_executable(http_test tests/http_test.c)
    target_link_libraries(http_test PRIVATE ca_test cagent_http)

    add_executable(json_test tests/json_test.c)
    target_link_libraries(json_test PRIVATE ca_test cagent_json)

    add_executable(session_test tests/session_test.c)
    target_link_libraries(session_test PRIVATE ca_test cagent_session)

    add_executable(llm_test tests/llm_test.c)
    target_link_libraries(llm_test PRIVATE ca_test cagent_llm)

    add_executable(ui_test tests/ui_test.c)
    target_link_libraries(ui_test PRIVATE ca_test cagent_ui)

    add_executable(tools_test tests/tools_test.c)
    target_link_libraries(tools_test PRIVATE ca_test cagent_tools)

    add_test(NAME mem_test COMMAND mem_test)
    add_test(NAME sse_test COMMAND sse_test)
    add_test(NAME http_test COMMAND http_test)
    add_test(NAME json_test COMMAND json_test)
    add_test(NAME session_test COMMAND session_test)
    add_test(NAME llm_test COMMAND llm_test)
    add_test(NAME tools_test COMMAND tools_test)

    add_executable(core_test tests/core_test.c)
    target_link_libraries(core_test PRIVATE ca_test cagent_core)

    add_test(NAME core_test COMMAND core_test)
endif()

# ---- 可执行入口 ----
# main 刻意薄：配置解析 + 读一行 + 交给 core。
# 它不属于测试构建，所以放在 CAGENT_BUILD_TESTS 之外。
add_executable(cagent src/main.c)
target_link_libraries(cagent PRIVATE cagent_core)
# cagent_core 是静态库，其依赖（ui/llm/tools/json/sse）以 PUBLIC 标注，
# 因此这里不必重复列出。若漏标 PUBLIC，问题只会在链接期暴露，
# 而且报错指向的是一个很远的符号名，不容易联想到缺了哪一行。
