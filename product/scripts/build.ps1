# scripts/build.ps1 — Windows 构建脚本（agents.md 第 3 节命令）
# 用法：./scripts/build.ps1 [-Config Release] [-NoTests]
# 本文件为 UTF-8 with BOM：无 BOM 时 PowerShell 5.1 按系统 ANSI 代码页（简中为
# GBK/936）解码，中文注释与字符串会乱码，遇到不成对的多字节序列还会直接语法报错。
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build"

# cmake 可能未加入 PATH（常见于仅随 VS 安装的场景）：先探测常见安装位置，
# 找不到再交由 CommandNotFoundException 报错，给出明确失败信号。
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $candidates = @(
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
        "$env:LOCALAPPDATA\Programs\CMake\bin\cmake.exe"
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) {
        # 同一目录里也有 ctest，补进 PATH 后下面的 ctest 调用才可用。
        $env:Path = (Split-Path -Parent $found) + ";" + $env:Path
    } else {
        Write-Error "未找到 cmake。请安装 CMake 并将其加入 PATH，或安装到默认目录。"
        exit 1
    }
}

$tests = if ($NoTests) { "OFF" } else { "ON" }

# 生成器交给 CMake 自动探测（装了什么就用什么），不硬编码 -G。
# 用 $() 显式取值：避免某些 PowerShell 宿主把 "-DX=$Var" 原样传给 cmake
# （曾导致 CMAKE_BUILD_TYPE 落盘为字面量 "$Config"，ninja 规则名非法而无法构建）
cmake -S $Root -B $BuildDir `
    "-DCMAKE_BUILD_TYPE=$($Config)" `
    "-DAIMODEL_BUILD_TESTS=$($tests)"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoTests) {
    ctest --test-dir $BuildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Build OK: $BuildDir ($Config)"
