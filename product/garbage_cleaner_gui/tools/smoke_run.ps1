# smoke_run.ps1 — 无人值守冒烟测试（真实点击，不是「能编译就算过」）
#
# 验证的是交付物本身：构建出的 exe 在真实桌面上被真实鼠标点击后能否完成
# 「扫描 → 列出垃圾 → 确认清理 → 文件真的从磁盘上消失」这条用户路径。
#
# 步骤：
#   1. 准备一个临时「垃圾目录」：3 个会被识别的文件 + 1 个不该被动的文件；
#   2. 启动 exe，把该目录作为命令行参数传入（见 main.cpp 的 argv 约定）；
#   3. 截图 + 按填充色定位蓝色「开始扫描」按钮 → 点击；
#   4. 断言列表区域像素发生变化（= 扫描结果真的画出来了）；
#   5. 定位红色「清理」按钮 → 点击 → 向确认对话框发送 IDOK；
#   6. 断言 3 个垃圾文件已从磁盘删除、且 keep.txt 仍在。
#
# 用法：powershell -ExecutionPolicy Bypass -File tools/smoke_run.ps1 [-Config Release]
#
# 注意：本文件必须保存为 UTF-8 with BOM。Windows PowerShell 5.1 在没有 BOM 时
# 按系统 ANSI 代码页（简中为 GBK/936）解码，中文字符串会直接语法报错。
# 用法：powershell -ExecutionPolicy Bypass -File tools/smoke_run.ps1 [-Config Release] [-BuildDir build]
#   -BuildDir 可指向任意构建目录（如构建到 build_verify 时验证那个产物）
param(
    [string]$Config = "Release",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class GcWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    // 找属于指定进程的对话框（MessageBox 的窗口类是 #32770）。
    // 不能用 FindWindowW("#32770", NULL)：它只返回第一个匹配的窗口，
    // 桌面上别的程序（安装器、IE 之类）也有 #32770，会被它一直返回。
    public static IntPtr FindDialog(uint targetPid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid || !IsWindowVisible(h)) return true;
            var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
            if (cls.ToString() == "#32770") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flag, uint dx, uint dy, uint data, IntPtr extra);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@

# --- 通用工具 ---------------------------------------------------------------

function Save-Bytes([System.Drawing.Image]$img) {
    $ms = New-Object System.IO.MemoryStream
    $img.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

function Hash([System.Drawing.Image]$img) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash((Save-Bytes $img)))).Replace("-", "") }
    finally { $sha.Dispose() }
}

# 按填充色找出按钮矩形中心。Duilib 的按钮是贴图（btn_global_*_80x30），
# 颜色取自 themes/default/public/button/*_normal.png，比按坐标硬算稳得多。
function Find-ColorBox([System.Drawing.Bitmap]$bmp, $r, $g, $b, $yFrom, $yTo, [int]$tol = 12) {
    $minX = [int]::MaxValue; $minY = [int]::MaxValue; $maxX = -1; $maxY = -1
    for ($y = $yFrom; $y -le $yTo; $y++) {
        for ($x = [int]($bmp.Width / 2); $x -lt $bmp.Width; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ([Math]::Abs($c.R - $r) -le $tol -and [Math]::Abs($c.G - $g) -le $tol -and [Math]::Abs($c.B - $b) -le $tol) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) { return $null }
    return New-Object System.Drawing.Point ([int](($minX + $maxX) / 2)), ([int](($minY + $maxY) / 2))
}

# --- 准备「垃圾目录」 -------------------------------------------------------

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root (Join-Path $BuildDir "$Config\garbage_cleaner_gui.exe")
if (-not (Test-Path $exe)) { throw "找不到可执行文件：$exe" }

$scanDir = Join-Path $env:TEMP "gc_smoke_scan"
if (Test-Path $scanDir) { Remove-Item -LiteralPath $scanDir -Recurse -Force }
New-Item -ItemType Directory -Path $scanDir | Out-Null

$junk = @("cache_a.tmp", "session_b.log", "build_c.obj")
foreach ($n in $junk) { [System.IO.File]::WriteAllBytes((Join-Path $scanDir $n), (New-Object byte[] 1048576)) }
[System.IO.File]::WriteAllText((Join-Path $scanDir "keep.txt"), "这个文件不该被删掉。")
Write-Output "扫描目录：$scanDir（3 个待清理文件 + 1 个需保留文件）"

$proc = Start-Process -FilePath $exe -ArgumentList "`"$scanDir`"" -WorkingDirectory (Split-Path $exe) -PassThru
try {
    Start-Sleep -Seconds 3
    $proc.Refresh()
    if ($proc.HasExited) { throw "进程已退出，退出码 = $($proc.ExitCode)（多半是资源加载失败）" }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw "进程存活但没有主窗口" }

    $rect = New-Object 'GcWin+RECT'
    [void][GcWin]::GetWindowRect($proc.MainWindowHandle, [ref]$rect)
    $client = New-Object 'GcWin+RECT'
    [void][GcWin]::GetClientRect($proc.MainWindowHandle, [ref]$client)
    $origin = New-Object 'GcWin+POINT'
    [void][GcWin]::ClientToScreen($proc.MainWindowHandle, [ref]$origin)
    $cw = $client.Right
    $ch = $client.Bottom
    Write-Output "窗口：$($rect.Right - $rect.Left) x $($rect.Bottom - $rect.Top)，客户区 $cw x $ch，标题 '$($proc.MainWindowTitle)'"

    [void][GcWin]::SetForegroundWindow($proc.MainWindowHandle)
    Start-Sleep -Milliseconds 600

    # 截图（客户区坐标，含 duilib 自己留的阴影边距）
    $shot = New-Object System.Drawing.Bitmap $cw, $ch
    $gfx = [System.Drawing.Graphics]::FromImage($shot)

    # 列表区域：标题栏(35) + 目录行(48) + 表头(30) 之后
    $listRect = New-Object System.Drawing.Rectangle 12, 115, ($cw - 24), 150

    function Grab([string]$tag) {
        $gfx.CopyFromScreen($origin.X, $origin.Y, 0, 0, $shot.Size)
        [System.IO.File]::WriteAllBytes((Join-Path $env:TEMP "gc_$tag`_full.png"), (Save-Bytes $shot))
        $region = $shot.Clone($listRect, $shot.PixelFormat)
        $h = Hash $region
        $region.Dispose()
        return $h
    }

    function Click-At([System.Drawing.Point]$p) {
        [void][GcWin]::SetCursorPos($p.X, $p.Y)
        Start-Sleep -Milliseconds 300
        [GcWin]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)  # LEFT DOWN
        Start-Sleep -Milliseconds 60
        [GcWin]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)  # LEFT UP
    }

    # --- 步骤 1：扫描 -------------------------------------------------------
    # 先在「干净」状态截图定位按钮；按钮中心换算成屏幕坐标（客户区原点 + 图像内坐标）
    $gfx.CopyFromScreen($origin.X, $origin.Y, 0, 0, $shot.Size)
    $blue = Find-ColorBox $shot 35 142 250 40 120
    if ($null -eq $blue) { throw "没在窗口里找到蓝色「开始扫描」按钮，界面可能没渲染出来" }
    Write-Output "「开始扫描」按钮中心（客户区坐标）：$($blue.X),$($blue.Y)"

    $blueScreen = New-Object System.Drawing.Point ($origin.X + $blue.X), ($origin.Y + $blue.Y)
    # 先落鼠标再截「点击前」：悬停高亮已经生效，之后的像素差异只可能来自点击本身
    [void][GcWin]::SetCursorPos($blueScreen.X, $blueScreen.Y)
    Start-Sleep -Milliseconds 400
    $listBefore = Grab "before"
    Write-Output "列表区域（扫描前）：$($listBefore.Substring(0,16))"

    Click-At $blueScreen
    Start-Sleep -Seconds 3
    $proc.Refresh()
    if ($proc.HasExited) { throw "点击扫描后进程崩溃，退出码 = $($proc.ExitCode)" }

    $listAfter = Grab "after"
    Write-Output "列表区域（扫描后）：$($listAfter.Substring(0,16))"
    if ($listBefore -eq $listAfter) {
        throw "点击「开始扫描」后列表区域没有任何变化——扫描结果没有画出来"
    }
    Write-Output "OK：扫描完成且结果列表已渲染"

    # --- 步骤 2：清理 -------------------------------------------------------
    $red = Find-ColorBox $shot 240 91 96 ($ch - 90) ($ch - 6)
    if ($null -eq $red) { throw "没在窗口里找到红色「清理」按钮" }
    Write-Output "「清理」按钮中心（客户区坐标）：$($red.X),$($red.Y)"

    $redScreen = New-Object System.Drawing.Point ($origin.X + $red.X), ($origin.Y + $red.Y)
    Click-At $redScreen

    # 确认对话框是原生 MessageBox（类名 #32770），必须属于本进程才算我们的
    $dlg = [IntPtr]::Zero
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        $dlg = [GcWin]::FindDialog([uint32]$proc.Id)
        if ($dlg -ne [IntPtr]::Zero) { break }
    }
    if ($dlg -eq [IntPtr]::Zero) { throw "点击「清理」后没有出现确认对话框" }
    Write-Output "确认对话框已出现（hwnd=$dlg），发送 IDOK"
    [void][GcWin]::PostMessage($dlg, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # WM_COMMAND, IDOK

    Start-Sleep -Seconds 2
    $proc.Refresh()
    if ($proc.HasExited) { throw "确认清理后进程崩溃，退出码 = $($proc.ExitCode)" }

    # --- 步骤 3：断言磁盘状态 ----------------------------------------------
    $left = @()
    foreach ($n in $junk) { if (Test-Path (Join-Path $scanDir $n)) { $left += $n } }
    if ($left.Count -gt 0) { throw "清理后这些文件仍然存在：$($left -join ', ')" }
    if (-not (Test-Path (Join-Path $scanDir "keep.txt"))) { throw "不该被删的 keep.txt 被删掉了" }

    Write-Output "PASS：扫描 → 列出 → 确认 → 3 个文件已从磁盘删除，keep.txt 未被误删"
    Write-Output "截图：$env:TEMP\gc_before_full.png / gc_after_full.png"
}
finally {
    $proc.Refresh()
    if (-not $proc.HasExited) {
        [void]$proc.CloseMainWindow()
        Start-Sleep -Seconds 1
        $proc.Refresh()
        if (-not $proc.HasExited) { $proc.Kill() }
    }
}
