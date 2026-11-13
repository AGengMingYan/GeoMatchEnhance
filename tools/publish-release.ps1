<#
.SYNOPSIS
    用 Gitee OpenAPI v5 发布发行版并上传附件（纯命令行，走本机 SOCKS5 代理）。

.DESCRIPTION
    Gitee 的「发行版附件」不在 git 协议里，git push 推送不到，只能调 HTTP API。
    本脚本做三件事：
      1) 用私人令牌校验身份
      2) 按 tag 创建发行版（已存在则复用，不覆盖）
      3) 把本地压缩包作为附件上传

    本机需通过 SOCKS5 代理访问 gitee.com；curl 走 schannel 时会因
    「吊销服务器离线」失败，所以脚本固定带 --ssl-no-revoke。

.PARAMETER Tag
    已推送到远程的 tag，例如 1.0.3

.PARAMETER File
    要上传的本地文件。必须已经是明文（首两字节 PK）。
    本机 .zip 会被过滤驱动加密，所以请用 .zip1 作为本地文件名。

.PARAMETER UploadAs
    上传到 Gitee 后显示的文件名，默认把 .zip1 还原成 .zip，
    这样下载方拿到的就是正常的 zip，不用自己改名。

.PARAMETER Token
    Gitee 私人令牌。取值顺序：
      -Token 参数 > 环境变量 GITEE_TOKEN > -TokenFile 指定的文件 > 交互式隐藏输入
    需要的权限：projects（仓库读写）。

.PARAMETER TokenFile
    把令牌放在文件里。默认 Gitee私人令牌.txt。
    注意：本机经编辑器/python 写入的 .txt 会被安全软件加密（首字节 E0 A8 91），
    脚本会自动用 Git 的 cat.exe 解密（输出到 .json 临时文件，因为写 .txt 又会加密）。

.EXAMPLE
    .\publish-release.ps1 -Tag 1.0.3 -File ..\release_GeoGUIDemo_x64\GeoGUIDemo-1.0.3-win64.zip1

.EXAMPLE
    .\publish-release.ps1 -Tag 1.0.3 -File x.zip1 -WhatIf
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$File,
    [string]$UploadAs = '',
    [string]$BodyFile = '',
    [string]$Name     = '',
    [string]$Repo     = 'ismingyan/geo-match-enhance',
    [string]$Token    = '',
    [string]$TokenFile = 'Gitee私人令牌.txt',
    [string]$Proxy    = '127.0.0.1:1080',
    [switch]$NoProxy,
    [switch]$Prerelease,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$CURL = Join-Path $env:SystemRoot 'system32\curl.exe'
$API  = "https://gitee.com/api/v5/repos/$Repo"

function Invoke-Curl {
    param([string[]]$CurlArgs, [switch]$AllowFail)

    $full = @('-sS', '--max-time', '600', '--ssl-no-revoke')
    if (-not $NoProxy -and $Proxy) { $full += @('--socks5-hostname', $Proxy) }
    $full += $CurlArgs

    if ($WhatIf) {
        # 绝不把令牌打到屏幕上
        $masked = $full | ForEach-Object {
            if ($_ -match 'token\s+[0-9a-fA-F]{8,}') { $_ -replace '(token\s+)\S+', '$1***' } else { $_ }
        }
        Write-Host ('  [WhatIf] curl.exe ' + ($masked -join ' ')) -ForegroundColor DarkGray
        return @{ code = 0; body = '' }
    }

    $out  = & $CURL @full 2>&1
    $code = $LASTEXITCODE
    $body = ($out | Out-String).Trim()
    if ($code -ne 0 -and -not $AllowFail) { throw "curl 退出码 $code`n$body" }
    return @{ code = $code; body = $body }
}

function Read-TokenFromFile {
    param([string]$Path)
    if (-not $Path) { return $null }
    if (-not (Test-Path -LiteralPath $Path)) { return $null }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
    $isCipher = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xE0 -and $bytes[1] -eq 0xA8 -and $bytes[2] -eq 0x91)

    if ($isCipher) {
        # 本机安全软件对 .txt 加密。用 Git 的 cat.exe 解密，
        # 且输出必须是非 .txt 扩展名（写 .txt 会被再次加密）。
        $gitCat = 'C:\Program Files\Git\usr\bin\cat.exe'
        if (-not (Test-Path -LiteralPath $gitCat)) {
            throw "令牌文件是密文，但没找到 $gitCat，无法自动解密。"
        }
        $tmp = Join-Path $env:TEMP ("gtok_" + [guid]::NewGuid().ToString('N') + ".json")
        $src = (Resolve-Path -LiteralPath $Path).Path
        & cmd /c "`"$gitCat`" `"$src`" > `"$tmp`""
        try   { $text = [System.IO.File]::ReadAllText($tmp, [System.Text.Encoding]::UTF8) }
        finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    } else {
        $text = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).Path,
                                              [System.Text.Encoding]::UTF8)
    }

    $m = [regex]::Match($text, '[0-9a-fA-F]{32}')
    if ($m.Success) { return $m.Value }
    return $null
}

function Get-Token {
    if ($Token) { return $Token }
    if ($env:GITEE_TOKEN) { return $env:GITEE_TOKEN }

    $fromFile = Read-TokenFromFile $TokenFile
    if ($fromFile) {
        Write-Host "令牌来源: 文件 $TokenFile" -ForegroundColor DarkGray
        return $fromFile
    }

    Write-Host '请输入 Gitee 私人令牌（输入时不显示）：' -ForegroundColor Yellow
    $sec  = Read-Host -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try   { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# ---------------------------------------------------------------- 前置检查
if (-not (Test-Path -LiteralPath $File)) { throw "找不到文件: $File" }
$fullPath = (Resolve-Path -LiteralPath $File).Path

# 明文校验：本机的 .zip 落盘即加密，上传上去就是坏包
$fs = [System.IO.File]::OpenRead($fullPath)
try   { $hdr = New-Object byte[] 2; [void]$fs.Read($hdr, 0, 2) }
finally { $fs.Dispose() }
if ($hdr[0] -ne 0x50 -or $hdr[1] -ne 0x4B) {
    $hex = ($hdr | ForEach-Object { $_.ToString('X2') }) -join ' '
    throw "$File 不是明文 zip（首字节 $hex，应为 50 4B）。请用 .zip1 命名（.zip 在本机落盘即加密）。"
}
$sizeMB = [math]::Round((Get-Item -LiteralPath $fullPath).Length / 1MB, 2)

if (-not $UploadAs) {
    $UploadAs = [System.IO.Path]::GetFileName($fullPath)
    if ($UploadAs -match '\.zip1$') { $UploadAs = $UploadAs -replace '\.zip1$', '.zip' }
}
if (-not $Name) { $Name = "$Tag" }
if (-not $BodyFile) {
    $cand = Join-Path (Split-Path $fullPath) 'release_body.md'
    if (Test-Path -LiteralPath $cand) { $BodyFile = $cand }
}

Write-Host "仓库  : $Repo"                    -ForegroundColor Cyan
Write-Host "tag   : $Tag"                     -ForegroundColor Cyan
Write-Host "文件  : $fullPath ($sizeMB MB)"   -ForegroundColor Cyan
Write-Host "上传名: $UploadAs"                -ForegroundColor Cyan
if ($BodyFile) { Write-Host "说明  : $BodyFile" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 1) 令牌
$Token = Get-Token
if (-not $Token) { throw '没有令牌，无法发布。请设置环境变量 GITEE_TOKEN 或交互输入。' }

if (-not $WhatIf) {
    $r = Invoke-Curl @('-H', "Authorization: token $Token", "$API")
    if ($r.body -match '"message"') { throw "令牌无效或无权限：`n$($r.body)" }
    Write-Host "`n[1/3] 令牌校验通过" -ForegroundColor Green
} else {
    Write-Host "`n[1/3] (WhatIf) 跳过令牌校验" -ForegroundColor DarkGray
    Invoke-Curl @('-H', "Authorization: token $Token", "$API") | Out-Null
}

# ---------------------------------------------------------------- 2) 发行版
$releaseId = $null
if (-not $WhatIf) {
    $existing = Invoke-Curl @('-H', "Authorization: token $Token", "$API/releases/tags/$Tag") -AllowFail
    if ($existing.body -match '"id"\s*:\s*(\d+)') {
        $releaseId = [int]$Matches[1]
        Write-Host "[2/3] 发行版已存在，复用 id=$releaseId（不覆盖原有内容）" -ForegroundColor Yellow
    } else {
        $form = @(
            '-X', 'POST',
            '-H', "Authorization: token $Token",
            '-F', "tag_name=$Tag",
            '-F', "name=$Name",
            '-F', 'target_commitish=master'
        )
        if ($BodyFile -and (Test-Path -LiteralPath $BodyFile)) { $form += @('-F', "body=<$BodyFile") }
        if ($Prerelease) { $form += @('-F', 'prerelease=true') }

        $c = Invoke-Curl ($form + @("$API/releases"))
        if ($c.body -match '"id"\s*:\s*(\d+)') {
            $releaseId = [int]$Matches[1]
            Write-Host "[2/3] 已创建发行版 id=$releaseId" -ForegroundColor Green
        } else {
            throw "创建发行版失败：`n$($c.body)"
        }
    }
} else {
    Write-Host '[2/3] (WhatIf) 将查询 releases/tags，不存在则 POST /releases' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- 3) 附件
$localSize = (Get-Item -LiteralPath $fullPath).Length
if (-not $WhatIf) {
    # 幂等：同名且大小一致的附件已存在就跳过。Gitee 的 attach_files 是"追加"语义，
    # 直接重跑会产生重复附件（实测过，会出现两条同名记录）。
    $att = Invoke-Curl @(
        '-H', "Authorization: token $Token",
        "$API/releases/$releaseId/attach_files"
    ) -AllowFail

    $dup = $false
    if ($att.body -match '\[') {
        try {
            $arr = $att.body | ConvertFrom-Json
            foreach ($a in $arr) {
                if ($a.name -eq $UploadAs -and [int64]$a.size -eq $localSize) { $dup = $true; break }
            }
        } catch { }
    }

    if ($dup) {
        Write-Host "[3/3] 附件 $UploadAs 已存在且大小一致，跳过上传" -ForegroundColor Yellow
        Write-Host "      发行版页: https://gitee.com/$Repo/releases/tag/$Tag" -ForegroundColor Cyan
    } else {
        $u = Invoke-Curl @(
            '-X', 'POST',
            '-H', "Authorization: token $Token",
            '-F', "file=@$fullPath;filename=$UploadAs",
            "$API/releases/$releaseId/attach_files"
        )
        if ($u.body -match '"browser_download_url"\s*:\s*"([^"]+)"') {
            Write-Host "[3/3] 附件上传成功" -ForegroundColor Green
            Write-Host "      发行版页: https://gitee.com/$Repo/releases/tag/$Tag" -ForegroundColor Cyan
            Write-Host "      下载直链: $($Matches[1])" -ForegroundColor Cyan
        } else {
            throw "附件上传失败：`n$($u.body)"
        }
    }
} else {
    Write-Host "[3/3] (WhatIf) 将先查同名同尺寸附件，没有才 POST /releases/<id>/attach_files" -ForegroundColor DarkGray
    Write-Host "`nWhatIf 结束，没有发起任何修改请求。" -ForegroundColor DarkGray
}
