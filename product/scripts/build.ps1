# scripts/build.ps1 — Windows 构建脚本
# 用法：./scripts/build.ps1 [-Config Release|Debug] [-NoTests] [-NoPythonTests] [-Generator <名>] [-Arch <架构>]
# 本文件为 UTF-8 with BOM：无 BOM 时 PowerShell 5.1 按系统 ANSI 代码页
# （简中为 GBK/936）解码，中文注释与字符串会乱码，甚至直接语法报错。
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$NoTests,
    [switch]$NoPythonTests,
    # 生成器与架构：留空 = 交 CMake 自动探测。但**探测结果随机器而变**
    # （本机默认 Ninja + MinGW；GitHub 的 Windows 运行器默认 Visual Studio + MSVC），
    # 要让「构建工具链」成为受控变量就得能显式钉死：
    #   -Generator "Visual Studio 17 2022" -Arch x64
    [string]$Generator = "",
    [string]$Arch = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build"

# cmake 可能未加入 PATH（常见于仅随 VS 安装的场景）：先探测常见安装位置，
# 找不到再报错，给出明确失败信号。
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

# 生成器默认交 CMake 自动探测（装了什么就用什么），只在显式指定时才传 -G/-A。
# 为什么要有这个开关：探测结果**因机器而异**（本机是 Ninja + MinGW，而 CI 的
# Windows 运行器会落在 Visual Studio + MSVC）。工具链是构建的输入之一——
# 要复现就得能钉死它，否则「同一份源码两样结果」又会重演。
# 用 $() 显式取值：避免某些 PowerShell 宿主把 "-DX=$Var" 原样传给 cmake
# （曾导致 CMAKE_BUILD_TYPE 落盘为字面量 "$Config"）。
$cmakeArgs = @(
    "-S", $Root,
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$($Config)",
    "-DSAVEINSPECTOR_BUILD_TESTS=$($tests)")
if ($Generator) { $cmakeArgs += @("-G", $Generator) }
if ($Arch) { $cmakeArgs += @("-A", $Arch) }
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoTests) {
    ctest --test-dir $BuildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# ---- Python 层：内部工具链单测 + 用户场景 E2E（一次 pytest 跑两层）----
# testpaths 已在 python/pyproject.toml 声明为 ["tests", "../tests/e2e"]：E2E 从
# **真实可执行文件**启动，与链接库的单测覆盖面不同，因此必须从 python/ 目录
# 跑 pytest；拆成两条命令会把 E2E 跑两遍。
#
# 解释器优先级：$env:PYTHON 显式指定 > 项目内 .venv > PATH 上的 python/python3。
# 项目内 venv 优先很重要：系统 python 往往没装 pytest。
#
# 缺 python/pytest 时**不静默跳过**：静默跳过正是「产物层其实零覆盖、
# 却被当成全绿」的来源。要跳过请显式传 -NoPythonTests。
$py = $env:PYTHON
if (-not $py) {
    $venvPy = Join-Path $Root ".venv\Scripts\python.exe"
    if (Test-Path $venvPy) { $py = $venvPy }
}
if (-not $py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if ($cmd) { $py = $cmd.Source }
}

if (-not $NoPythonTests) {
    if (-not $py) {
        Write-Host "错误：未找到 python，Python 层用例（内部工具链单测 + 真实产物 E2E）未运行。"
        Write-Host "      安装 python 后重试，或显式传 -NoPythonTests 跳过。"
        exit 1
    }
    # 探测 pytest 是否可用。
    # **不能**写成 `& $py -c "import pytest" 2>&1`：$ErrorActionPreference = "Stop"
    # 下，被重定向进成功流的原生 stderr 会被抛成 NativeCommandError（终止错误），
    # 脚本当场中断，下面这段友好提示根本执行不到。
    # find_spec 只查可导入性、不触发导入，也就**不产生 stderr**，从根上避开该陷阱。
    & $py -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('pytest') else 1)"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "错误：$py 未安装 pytest，Python 层用例（含 E2E）未运行。"
        Write-Host ('      修复：& "' + $py + '" -m pip install -e "' + $Root + '\python[dev]"')
        Write-Host "      或显式传 -NoPythonTests 跳过（会让产物层零覆盖）。"
        exit 1
    }
    Push-Location (Join-Path $Root "python")
    try {
        # 不再额外传 -q：pyproject 的 addopts 已有 -q，再传一次成了 -qq，
        # 连「N passed」的结论行都被抑制。构建脚本的用途就是留下门禁证据，
        # 结论行必须可见。
        & $py -m pytest
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}

Write-Host "Build OK: $BuildDir ($Config)"
