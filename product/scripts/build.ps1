# 配置 + 构建 + 测试（Windows）
#
# 这是仓库文档（AGENTS.md）里写的那个入口：
#     ./scripts/build.ps1 -Config Release
# 文档写了不存在的脚本，等于文档在骗人——所以它必须存在、且必须真的能跑。
#
# 两处踩过的坑，写在这里免得下次再踩：
#
#  1. **不要设 `$ErrorActionPreference = 'Stop'`**。PowerShell 会把**原生
#     命令写到 stderr 的每一行**包装成 ErrorRecord，而 CMake 的告警正是走
#     stderr 的。于是 `Stop` 会让脚本在一个**成功**的 configure 之后中止，
#     报错文字是 "CMake Warning:" —— 看起来像 CMake 挂了，其实是我们自己
#     把无害输出升级成了致命错误。正确做法：不看 stderr，只看 `$LASTEXITCODE`。
#
#  2. **CMAKE_BUILD_TYPE 对多配置生成器无意义**。默认的 Visual Studio
#     生成器是多配置的，传了它 CMake 会警告
#     「Manually-specified variables were not used by the project」。
#     构建配置由 `--config` 指定。用 Ninja/Makefile 这类单配置生成器时
#     才需要 CMAKE_BUILD_TYPE —— 那时加 `-SingleConfig`。
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build',
    # 单配置生成器（Ninja/Makefiles）才需要把 Config 写进 CMAKE_BUILD_TYPE
    [switch]$SingleConfig,
    # 透传给 cmake 的额外参数，例如：
    #   -CmakeArgs @('-A','x64','-DCMAKE_PREFIX_PATH=C:\path\to\curl')
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Continue'

# 局部变量**不能**叫 $cmakeArgs/$CmakeArgs：PowerShell 的变量名
# **不区分大小写**，`$cmakeArgs` 和参数 `$CmakeArgs` 是同一个变量。
# 踩过一次，症状很隐蔽：赋值把参数值冲掉，随后的 `+=` 变成
# 「基础参数拼自己两遍」，用户传的 -D 参数**静默消失**——
# 脚本不报错，只是构建出来的二进制换了后端（libcurl 变成 mem）。
# 所以这里用 $allArgs，并且参数名改成 $ExtraArgs 以免再撞。
$allArgs = @('-S', '.', '-B', $BuildDir)
if ($SingleConfig) { $allArgs += "-DCMAKE_BUILD_TYPE=$Config" }
$allArgs += $ExtraArgs

# -Verbose 时把**实际执行**的命令打出来：
# 参数出问题（比如 -D 被吞掉）时，看自己拼出来的命令行比猜快得多。
Write-Verbose ("cmake " + ($allArgs -join ' '))

Write-Host "== 配置（$BuildDir，$Config）==" -ForegroundColor Cyan
& cmake @allArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== 构建 ==" -ForegroundColor Cyan
& cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== 测试 ==" -ForegroundColor Cyan
& ctest --test-dir $BuildDir -C $Config --output-on-failure
exit $LASTEXITCODE
