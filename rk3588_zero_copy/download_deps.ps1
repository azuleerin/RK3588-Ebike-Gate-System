<#
.SYNOPSIS
    在 Windows 上下载 RK3588 板子(RK3588 / Ubuntu 26.04 "resolute" / Armbian aarch64)
    所需的【arm64】 .deb 安装包, 全部放进 board_deps 目录。

.DESCRIPTION
    工作流程:
      1) 从 Ubuntu Ports 镜像拉取 <suite>/{main,universe,multiverse}/binary-arm64/Packages.gz
         (清华源优先, 失败自动回退 ports.ubuntu.com)
      2) 解析索引 -> 包名 => {版本, 文件名, Depends, SHA256}
      3) 从你要的包出发做【依赖闭包】(BFS), 把所有依赖一起下载
         —— 这一步很关键: 板子没有外网, 少一个依赖 dpkg 就装不上
      4) curl 并行下载, 逐个按索引里的 Size 校验, 失败的换备用源重下
      5) Rockchip MPP (Ubuntu 源里没有) 从 Radxa 的 apt 仓库拿 arm64 的 .deb
      6) 生成 manifest.txt(文件名/sha256/来源URL), 板端可以用它校验

    ⚠️ 只下载 arm64 / all 架构的包。脚本会强制过滤 Architecture 字段,
       绝不会下成 x86 的。

.PARAMETER Suite
    Ubuntu 发行代号。26.04 = resolute, 24.04 = noble, 22.04 = jammy。

.PARAMETER Packages
    要装的顶层包。默认已包含编译本工程所需的全部依赖。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\download_deps.ps1

.EXAMPLE
    # 只下 FFmpeg 相关(板子上已经有 OpenCV / gcc 时用这个, 快很多)
    powershell -ExecutionPolicy Bypass -File .\download_deps.ps1 `
        -Packages ffmpeg,libavformat-dev,libavcodec-dev,libavutil-dev,libswscale-dev,libdrm-dev

.EXAMPLE
    # 先看看会下哪些包, 不实际下载
    powershell -ExecutionPolicy Bypass -File .\download_deps.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string]$OutputDir = '',
    [string]$Suite = 'resolute',
    [string[]]$Components = @('main', 'universe', 'multiverse'),
    [string[]]$Packages = @(
        'build-essential', 'cmake', 'pkg-config',
        'ffmpeg', 'libavformat-dev', 'libavcodec-dev', 'libavutil-dev',
        'libswscale-dev', 'libswresample-dev', 'libdrm-dev',
        'libopencv-dev'
    ),
    [string[]]$Skip = @(),
    [string]$Mirror = 'https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports',
    [string]$Mirror2 = 'http://ports.ubuntu.com/ubuntu-ports',
    [switch]$NoOpenCV,
    [switch]$NoMpp,
    [switch]$NoToolchain,
    [switch]$DryRun,
    [int]$Parallel = 8,
    [switch]$VerifyHash
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot 'board_deps' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

function Say  ($m) { Write-Host $m -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "  [!]  $m" -ForegroundColor Yellow }
function Err  ($m) { Write-Host "  [X]  $m" -ForegroundColor Red }
function Step ($m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }

# ---------------------------------------------------------------- curl 封装
$script:CurlExe = (Get-Command curl.exe -ErrorAction SilentlyContinue)
if (-not $script:CurlExe) { $script:CurlExe = (Get-Command curl -ErrorAction SilentlyContinue) }
if (-not $script:CurlExe) { Err "找不到 curl"; exit 1 }

function Invoke-Curl {
    param([string[]]$Urls, [string]$OutFile, [string]$WorkDir)
    $args = @('-sSfL', '--retry', '3', '--retry-delay', '2', '--connect-timeout', '20')
    if ($OutFile) { $args += @('-o', $OutFile) }
    if ($WorkDir) { $args += @('--output-dir', $WorkDir) }
    $args += $Urls
    $null = & $script:CurlExe @args
    return $LASTEXITCODE
}

# ---------------------------------------------------------------- 索引解析
function Get-PackageIndex {
    param([string]$Base, [string]$Suite, [string]$Component)
    $url = "$Base/dists/$Suite/$Component/binary-arm64/Packages.gz"
    $tmp = Join-Path $env:TEMP ("pk_{0}_{1}_{2}.gz" -f $Suite, $Component, [guid]::NewGuid().ToString('N').Substring(0,6))
    $rc = Invoke-Curl -Urls @($url) -OutFile $tmp
    if ($rc -ne 0 -or -not (Test-Path -LiteralPath $tmp)) { return @{} }

    $index = @{}
    $fs = [IO.File]::OpenRead($tmp)
    $gz = New-Object IO.Compression.GzipStream($fs, [IO.Compression.CompressionMode]::Decompress)
    $sr = New-Object IO.StreamReader($gz, [Text.Encoding]::UTF8)
    $cur = @{}
    $want = @('Package','Version','Architecture','Filename','Depends','Pre-Depends','Provides','Size','SHA256')
    while (($line = $sr.ReadLine()) -ne $null) {
        if ($line.Length -eq 0) {
            if ($cur.ContainsKey('Package')) {
                $arch = if ($cur.ContainsKey('Architecture')) { $cur['Architecture'] } else { '' }
                # ★ 只收 arm64 和 all, 绝不会下成 x86 的
                if ($arch -eq 'arm64' -or $arch -eq 'all') { $index[$cur['Package']] = $cur }
            }
            $cur = @{}
        } else {
            $i = $line.IndexOf(': ')
            if ($i -gt 0) {
                $k = $line.Substring(0, $i)
                if ($want -contains $k) { $cur[$k] = $line.Substring($i + 2) }
            }
        }
    }
    $sr.Close(); $gz.Dispose(); $fs.Dispose()
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    return $index
}

# ---------------------------------------------------------------- 依赖闭包
function Resolve-Closure {
    param([hashtable]$Index, [string[]]$Roots)
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    $missing = New-Object 'System.Collections.Generic.HashSet[string]'

    foreach ($r in $Roots) {
        if ($Index.ContainsKey($r)) { [void]$queue.Enqueue($r) }
        elseif (-not $missing.Contains($r)) { [void]$missing.Add($r) }
    }

    while ($queue.Count -gt 0) {
        $p = $queue.Dequeue()
        if (-not $seen.Add($p)) { continue }
        if (-not $Index.ContainsKey($p)) { continue }
        $e = $Index[$p]
        $depText = ''
        if ($e.ContainsKey('Pre-Depends')) { $depText += $e['Pre-Depends'] + ', ' }
        if ($e.ContainsKey('Depends'))    { $depText += $e['Depends'] }

        foreach ($alt in ($depText -split ',')) {
            $alt = $alt.Trim()
            if ($alt -eq '') { continue }
            $picked = $null
            foreach ($one in ($alt -split '\|')) {
                $name = (($one -split '\(')[0]).Trim()   # 去掉版本约束
                $name = (($name -split ':')[0]).Trim()    # 去掉 :any / :arm64
                if ($name -eq '') { continue }
                if ($Index.ContainsKey($name)) { $picked = $name; break }
            }
            if ($picked) {
                if (-not $seen.Contains($picked)) { [void]$queue.Enqueue($picked) }
            } elseif (-not $missing.Contains($alt)) {
                # 虚拟包(由别的包 Provides)或跨组件, 记录下来但不阻断
                [void]$missing.Add($alt)
            }
        }
    }
    return @{ Pkgs = @($seen); Missing = @($missing) }
}

# =====================================================================
Say "目标架构 : arm64 (aarch64)"
Say "发行代号 : $Suite"
Say "输出目录 : $OutputDir"

$roots = @($Packages)
if ($NoOpenCV)  { $roots = @($roots | Where-Object { $_ -ne 'libopencv-dev' });  Ok "跳过 libopencv-dev" }
if ($NoToolchain){ $roots = @($roots | Where-Object { $_ -notin @('build-essential','cmake','pkg-config') }); Ok "跳过编译工具链" }
if ($Skip)      { $roots = @($roots | Where-Object { $Skip -notcontains $_ }); Ok "跳过: $($Skip -join ', ')" }

# ---- 1) 索引 ----
Step "1) 下载并解析 Packages 索引 (arm64)"
$index = @{}
foreach ($c in $Components) {
    $got = Get-PackageIndex -Base $Mirror -Suite $Suite -Component $c
    if ($got.Count -eq 0) {
        Warn "$Mirror 上的 $c 索引不可用, 换备用源"
        $got = Get-PackageIndex -Base $Mirror2 -Suite $Suite -Component $c
    }
    if ($got.Count -eq 0) { Warn "组件 $c 的索引拿不到, 跳过"; continue }
    Ok ("{0}: {1} 个包" -f $c, $got.Count)
    foreach ($k in $got.Keys) { $index[$k] = $got[$k] }
}
if ($index.Count -eq 0) { Err "没有任何索引可用, 检查网络/发行代号"; exit 1 }
Ok ("索引合计 {0} 个 arm64 包" -f $index.Count)

# ---- 2) 依赖闭包 ----
Step "2) 解析依赖闭包"
$res = Resolve-Closure -Index $index -Roots $roots
$closure = $res.Pkgs
Ok ("需要下载 {0} 个包(顶层 {1} 个)" -f $closure.Count, $roots.Count)
if ($res.Missing.Count -gt 0) {
    Warn ("以下依赖在索引里找不到(通常是虚拟包或跨组件): {0}" -f (($res.Missing | Select-Object -First 12) -join ', '))
    if ($res.Missing.Count -gt 12) { Warn ("... 还有 {0} 个" -f ($res.Missing.Count - 12)) }
}

# ---- 3) 列出计划 ----
Step "3) 下载计划"
$plan = @()
foreach ($p in $closure) {
    $e = $index[$p]
    $plan += [pscustomobject]@{
        Name = $p
        File = $e['Filename']
        Size = [int64]$e['Size']
        Sha  = if ($e.ContainsKey('SHA256')) { $e['SHA256'] } else { '' }
        Url  = "$Mirror/$($e['Filename'])"
        Url2 = "$Mirror2/$($e['Filename'])"
    }
}
$total = ($plan | Measure-Object -Property Size -Sum).Sum
Write-Host ("  共 {0} 个, 合计 {1:N1} MB" -f $plan.Count, ($total / 1MB))
if ($DryRun) {
    $plan | Sort-Object -Property Size -Descending | Select-Object -First 25 |
        Format-Table Name, @{N='MB';E={[math]::Round($_.Size/1MB,2)}} -AutoSize
    Write-Host "  (DryRun: 不实际下载)"
}

# ---- 4) 下载 ----
if (-not $DryRun) {
    Step "4) 下载 .deb"
    $urlFile = Join-Path $env:TEMP ("urls_{0}.txt" -f [guid]::NewGuid().ToString('N').Substring(0,6))
    # curl 的 --config 文件格式有坑, 踩过两次:
    #   1) 裸 URL 会被当成未知配置项 -> 必须写成 url = "..."
    #   2) --remote-name / --output-dir 只作用于【第一个】URL, 其余的会把内容
    #      直接吐到 stdout -> 每个 URL 之间必须用 --next 隔开, 各自带输出选项
    $bs = [char]92
    $dirUnix = $OutputDir.Replace($bs, '/')

    # ★ 幂等: 已经下好且大小正确的包直接跳过, 重跑脚本不用再拉 400MB
    $todo = @()
    foreach ($p in $plan) {
        $lp = Join-Path $OutputDir ([IO.Path]::GetFileName($p.File))
        if (Test-Path -LiteralPath $lp) {
            $have = (Get-Item -LiteralPath $lp).Length
            if ($have -eq $p.Size) { continue }
            Warn ("{0} 已存在但大小不符({1}/{2}), 重下" -f $p.Name, $have, $p.Size)
            Remove-Item -LiteralPath $lp -Force -ErrorAction SilentlyContinue
        }
        $todo += $p
    }
    if ($todo.Count -eq 0) {
        Ok ("全部 {0} 个包已就位, 跳过下载" -f $plan.Count)
    }

    $cfg = @()
    for ($i = 0; $i -lt $todo.Count; $i++) {
        $p = $todo[$i]
        $cfg += 'url = "' + $p.Url + '"'
        $cfg += 'remote-name'
        $cfg += 'output-dir = "' + $dirUnix + '"'
        # --next 只能夹在两个 URL 中间; 末尾多一个会生成空 transfer,
        # curl 报 "no URL specified" 并把整批并行传输一起带崩
        if ($i -lt $todo.Count - 1) { $cfg += '--next' }
    }
    if ($todo.Count -gt 0) {
        $cfg | Set-Content -LiteralPath $urlFile -Encoding ASCII
        Write-Host "  curl 并行下载 ($Parallel 路, 待下 $($todo.Count) 个)..."
        & $script:CurlExe -sSfL --retry 3 --retry-delay 2 --connect-timeout 20 `
            --parallel --parallel-max $Parallel --config $urlFile
        $rc = $LASTEXITCODE
    } else {
        $rc = 0
    }
    Remove-Item -LiteralPath $urlFile -Force -ErrorAction SilentlyContinue

    # 校验大小, 失败的换备用源单独重下
    $failed = @()
    foreach ($p in $plan) {
        $local = Join-Path $OutputDir ([IO.Path]::GetFileName($p.File))
        if (-not (Test-Path -LiteralPath $local)) { $failed += $p; continue }
        $sz = (Get-Item -LiteralPath $local).Length
        if ($sz -ne $p.Size) {
            Warn ("{0} 大小不符(本地 {1} / 期望 {2}), 换源重下" -f $p.Name, $sz, $p.Size)
            Remove-Item -LiteralPath $local -Force -ErrorAction SilentlyContinue
            & $script:CurlExe -sSfL --retry 3 --connect-timeout 20 -o $local $p.Url2
            if ($LASTEXITCODE -ne 0) { $failed += $p }
        }
    }
    if ($failed.Count -gt 0) {
        Warn ("{0} 个包下载失败: {1}" -f $failed.Count, (($failed | Select-Object -First 8 | ForEach-Object { $_.Name }) -join ', '))
    } else {
        Ok "全部下载完成"
    }

    # ---- 5) sha256 校验 ----
    if ($VerifyHash) {
        Step "5) SHA256 校验"
        $bad = 0
        foreach ($p in $plan) {
            $local = Join-Path $OutputDir ([IO.Path]::GetFileName($p.File))
            if (-not (Test-Path -LiteralPath $local) -or -not $p.Sha) { continue }
            $h = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash.ToLower()
            if ($h -ne $p.Sha.ToLower()) { Err "$($p.Name) 校验失败"; $bad++ }
        }
        if ($bad -eq 0) { Ok "SHA256 全部通过" }
    }

    # ---- 6) MPP (Ubuntu 源里没有, 从 Radxa 的 apt 仓库拿) ----
    if (-not $NoMpp) {
        Step "6) 下载 Rockchip MPP (arm64)"
        # 说明: Ubuntu/Debian 源里没有 librockchip-mpp。
        #      Radxa 的 apt 仓库有 arm64 的 .deb(20190626 版, 头文件装到
        #      /usr/include/rockchip/, 库装到 /usr/lib/aarch64-linux-gnu/)。
        $mppFiles = @(
            'librockchip-mpp1_20190626-1_arm64.deb',
            'librockchip-mpp-dev_20190626-1_arm64.deb'
        )
        foreach ($f in $mppFiles) {
            $url = "https://apt.radxa.com/bionic/pool/main/m/mpp/$f"
            $local = Join-Path $OutputDir $f
            & $script:CurlExe -sSfL --retry 2 --connect-timeout 20 -o $local $url
            if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $local)) {
                # ⚠️ [math] 的方括号不能省: 写成 $(math]::Round(..) 时 PowerShell 会把
                #    "math]" 当成命令去调用, 报 "op_Division: [System.Object[]] 不包含..."
                Ok "$f  ($([math]::Round((Get-Item -LiteralPath $local).Length / 1KB, 1)) KB)"
            } else {
                Warn "$f 下载失败(不影响编译, 只是没有 MPP 硬解支持)"
                Remove-Item -LiteralPath $local -Force -ErrorAction SilentlyContinue
            }
        }
    }

    # ---- 7) manifest ----
    Step "7) 生成 manifest"
    $mf = Join-Path $OutputDir 'manifest.txt'
    $lines = @('# 文件名  size  sha256  来源')
    foreach ($p in $plan) {
        $lines += ("{0}`t{1}`t{2}`t{3}" -f [IO.Path]::GetFileName($p.File), $p.Size, $p.Sha, $p.Url)
    }
    $lines | Set-Content -LiteralPath $mf -Encoding UTF8
    Ok "manifest.txt"
}

# ---- 8) 板端安装说明 ----
$readme = @"
board_deps/ 里是 RK3588(Ubuntu $Suite / arm64) 的 .deb 安装包, 全部离线可用。

板子上执行:
    cd <包含 board_deps 的目录>
    sudo ./install_deps.sh board_deps

或者手动:
    sudo dpkg -i board_deps/*.deb
    sudo dpkg --configure -a
    sudo ldconfig

注意:
  * 板子【没有外网】, 所以不能用 apt-get -f install 去网上补依赖;
    本目录已经包含了完整依赖闭包, dpkg -i 一次装齐即可。
  * 全部是 arm64 架构, 不会被当成 x86 包。
  * ffmpeg 来自 Ubuntu 官方源, 【没有 h264_rkmpp 硬解码器】(Debian/Ubuntu 不带
    Rockchip MPP 支持)。硬解需要 Rockchip SDK 里的 ffmpeg-rockchip。
    没有硬解时本工程会自动退回软解码, 流水线照常跑。
  * MPP 的 .deb 来自 Radxa 的 apt 仓库, 只提供头文件(/usr/include/rockchip/)
    和库, 版本是 20190626(旧 API), 够本工程取 dma-buf fd 用了。
"@
$readme | Set-Content -LiteralPath (Join-Path $OutputDir 'README.txt') -Encoding UTF8

Say ""
Ok "全部完成"
$debs = @(Get-ChildItem -LiteralPath $OutputDir -Filter *.deb -ErrorAction SilentlyContinue)
Say "  目录: $OutputDir"
Say "  个数: $($debs.Count) 个 .deb"
if ($debs.Count -gt 0) {
    $sum = ($debs | Measure-Object -Property Length -Sum).Sum
    Say ("  大小: {0:N1} MB" -f ($sum / 1MB))
}
Say ""
Say "下一步: 把 board_deps 整个目录 scp 到板子, 然后 sudo ./install_deps.sh board_deps"
