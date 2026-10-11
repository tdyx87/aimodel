# scripts/build.ps1 — Windows 构建脚本（agents.md 第 3 节命令）
# 用法：./scripts/build.ps1 [-Config Release] [-NoTests]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build"

$tests = if ($NoTests) { "OFF" } else { "ON" }

# MSVC 环境优先；回退到已安装的生成器（如 Ninja/MinGW）
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
