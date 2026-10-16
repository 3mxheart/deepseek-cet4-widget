# 用 MinGW-w64（GCC）把原生 C 版编译成单文件 exe
#
#   powershell -ExecutionPolicy Bypass -File build_c.ps1
#
# 产物：dist\DeepSeek悬浮窗C版.exe（不依赖 Python / .NET，约几百 KB）
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "csrc"
$bin  = Join-Path $here "tools\mingw64\bin"

$gcc     = Join-Path $bin "gcc.exe"
$gpp     = Join-Path $bin "g++.exe"
$windres = Join-Path $bin "windres.exe"
if (Test-Path $gcc) {
    # 用项目自带的便携工具链；同时把工具目录挂到 PATH，windres 预处理 .rc 时会去找 gcc
    $env:PATH = "$bin;$env:PATH"
} else {
    # 没带工具链就退回 PATH 里的 MinGW
    $gcc     = (Get-Command gcc.exe     -ErrorAction SilentlyContinue).Source
    $gpp     = (Get-Command g++.exe     -ErrorAction SilentlyContinue).Source
    $windres = (Get-Command windres.exe -ErrorAction SilentlyContinue).Source
    if (-not $gcc -or -not $gpp) {
        throw "没找到编译器。请把便携版 MinGW-w64 解压到 tools\mingw64（见 README），或把 gcc/g++ 装进 PATH。"
    }
}

$common = @("-O2", "-municode", "-DUNICODE", "-D_UNICODE", "-Wall")

Write-Output "[1/3] 编译 config.c"
& $gcc @common -c -o (Join-Path $src "config.o") (Join-Path $src "config.c")
if ($LASTEXITCODE -ne 0) { throw "config.c 编译失败" }

Write-Output "[2/3] 编译 ui.cpp"
& $gpp @common -c -o (Join-Path $src "ui.o") (Join-Path $src "ui.cpp")
if ($LASTEXITCODE -ne 0) { throw "ui.cpp 编译失败" }

Write-Output "[3/3] 链接（图标 app.rc 交给 gcc 调用 windres 处理）"
$raw = Join-Path $src "DeepSeekWidget.exe"
& $gpp -municode -mwindows -static -s -o $raw `
    (Join-Path $src "config.o") (Join-Path $src "ui.o") (Join-Path $src "app.rc") `
    -lwinhttp -lgdiplus -lcomdlg32 -lshell32 -ladvapi32 -lole32 -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) { throw "链接失败" }

# 产物和 config.json 放同一层：两个版本共用同一份配置
$final = Join-Path $here "DeepSeek悬浮窗C版.exe"

# 旧实例在跑的话文件被占用，先请它退出
$procName = [System.IO.Path]::GetFileNameWithoutExtension($final)
$running = Get-Process -Name $procName -ErrorAction SilentlyContinue
if ($running) {
    Write-Output "检测到旧实例在运行，先关闭：$procName"
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}
if (Test-Path -LiteralPath $final) { Remove-Item -LiteralPath $final -Force }
Move-Item -LiteralPath $raw -Destination $final

$size = [math]::Round((Get-Item -LiteralPath $final).Length / 1KB, 1)
Write-Output ""
Write-Output "编译完成：$final（$size KB）"
Write-Output "提示：config.json / widget.log 会放在 exe 旁边；移动 exe 后重新勾一次“开机自动启动”。"
