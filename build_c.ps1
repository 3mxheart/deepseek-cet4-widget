# 用 MinGW-w64（GCC）把原生 C 版编译成单文件 exe
#
#   powershell -ExecutionPolicy Bypass -File build_c.ps1
#
# 产物：项目根目录下的 DeepSeek悬浮窗C版.exe（不依赖 Python / .NET，约 230 KB）
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$hereHasSpace = $here -like "* *"

# 路径里有空格时 GCC 的链接器会解析出错（把 "D:/codex work" 当成两个参数），
# 自动改用 8.3 短路径。别人 clone 到带空格的目录也不会踩坑。
if ($here -like "* *") {
    try {
        $short = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($here).ShortPath
        if ($short -and ($short -notlike "* *")) {
            Write-Output "提示：路径含空格，改用短路径 $short"
            $here = $short
        }
    } catch {
    }
}

$src  = Join-Path $here "csrc"
$bin  = Join-Path $here "tools\mingw64\bin"

$gcc     = Join-Path $bin "gcc.exe"
$gpp     = Join-Path $bin "g++.exe"
$windres = Join-Path $bin "windres.exe"
if (Test-Path $gcc) {
    # GCC 自身的安装路径里不能有空格（ld 会把 "D:/codex work" 拆成两个参数）。
    # 项目放在带空格的目录时，把工具链镜像到不带空格的缓存目录再用（只复制一次）。
    $realBin = (Resolve-Path $bin).Path
    if ($hereHasSpace) {
        $cache = Join-Path $env:LOCALAPPDATA "mingw64-widget"
        $cacheBin = Join-Path $cache "bin"
        if (-not (Test-Path (Join-Path $cacheBin "gcc.exe"))) {
            Write-Output "工具链在带空格的路径里，先镜像到 $cache（只需一次，约 900MB）…"
            New-Item -ItemType Directory -Force -Path $cache | Out-Null
            robocopy (Join-Path $here "tools\mingw64") $cache /E /NFL /NDL /NJH /NJS /NP | Out-Null
        }
        $bin = $cacheBin
        $gcc = Join-Path $bin "gcc.exe"
        $gpp = Join-Path $bin "g++.exe"
        $windres = Join-Path $bin "windres.exe"
        Write-Output "改用缓存工具链：$bin"
    }
    # 把工具目录挂到 PATH：windres 预处理 .rc 时会去找 gcc
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

Write-Output "[1/4] 编译 config.c"
& $gcc @common -c -o (Join-Path $src "config.o") (Join-Path $src "config.c")
if ($LASTEXITCODE -ne 0) { throw "config.c 编译失败" }

Write-Output "[2/4] 编译 providers.c"
& $gcc @common -c -o (Join-Path $src "providers.o") (Join-Path $src "providers.c")
if ($LASTEXITCODE -ne 0) { throw "providers.c 编译失败" }

Write-Output "[3/4] 编译 ui.cpp"
& $gpp @common -c -o (Join-Path $src "ui.o") (Join-Path $src "ui.cpp")
if ($LASTEXITCODE -ne 0) { throw "ui.cpp 编译失败" }

Write-Output "[4/4] 链接（图标 app.rc 交给 gcc 调用 windres 处理）"
$raw = Join-Path $src "DeepSeekWidget.exe"
& $gpp -municode -mwindows -static -s -o $raw `
    (Join-Path $src "config.o") (Join-Path $src "providers.o") (Join-Path $src "ui.o") `
    (Join-Path $src "app.rc") `
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
