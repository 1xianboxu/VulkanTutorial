<#
.SYNOPSIS
    将 ImageViewer 注册为 Windows 默认图片查看器
.DESCRIPTION
    1. 注册 ProgID（VulkanImageViewer.Image）
    2. 关联所有支持的图片扩展名（png/jpg/bmp/tga/gif/hdr/psd...）
    3. 添加文件夹右键菜单："用 Vulkan 图片浏览器打开"
.NOTES
    全部写入 HKCU（无需管理员权限），卸载：register_file_association.ps1 -Uninstall
#>

param(
    [string]$ExePath = "",
    [switch]$Uninstall
)

# ---- Thumbnail Handler 常量 ----
$ThumbDll   = "ImageViewerThumbnail.dll"
$ThumbCLSID = "{587EF8B0-7D02-4492-A069-942A37B54297}"
$ThumbShellexGUID = "{E357FCCD-A995-4578-B878-6D1F3C60A6A8}"  # Shell 缩略图查找接口的 GUID

# ---- 定位 exe ----
if (-not $ExePath) {
    $candidates = @(
        "$PSScriptRoot\..\ImageViewer\build\Release\ImageViewer.exe"
        "$PSScriptRoot\..\ImageViewer\build\Debug\ImageViewer.exe"
        "G:\ProjectC++\VulkanTutorial_\ImageViewer\build\Release\ImageViewer.exe"
        "G:\ProjectC++\VulkanTutorial_\ImageViewer\build\Debug\ImageViewer.exe"
    )
    foreach ($c in $candidates) {
        $resolved = Resolve-Path $c -ErrorAction SilentlyContinue
        if ($resolved) { $ExePath = $resolved.Path; break }
    }
    if (-not $ExePath) {
        Write-Host "未找到 ImageViewer.exe，请用 -ExePath 指定路径" -ForegroundColor Red
        exit 1
    }
}

$ExePath = (Resolve-Path $ExePath).Path
$ExeDir  = Split-Path $ExePath -Parent
$DllPath = Join-Path $ExeDir $ThumbDll
$ProgID  = "VulkanImageViewer.Image"
$AppName = "Vulkan 图片浏览器"
$iconPath = "$ExePath,0"
$cmdLine = "`"$ExePath`" `"%1`""
$cmdLineDir = "`"$ExePath`" `"%V`""

$extensions = @(".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".hdr", ".pic", ".ppm", ".pgm", ".psd")

if ($Uninstall) {
    Write-Host "正在卸载文件关联..." -ForegroundColor Yellow

    Remove-Item "HKCU:\Software\Classes\$ProgID" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "HKCU:\Software\Classes\CLSID\$ThumbCLSID" -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($ext in $extensions) {
        Remove-Item "HKCU:\Software\Classes\$ext\shell\VulkanImageViewer" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item "HKCU:\Software\Classes\$ext\OpenWithProgids" -Force -ErrorAction SilentlyContinue
        Remove-Item "HKCU:\Software\Classes\$ext\shellex\$ThumbShellexGUID" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\OpenWithProgids" -Force -ErrorAction SilentlyContinue
        Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoice" -Force -ErrorAction SilentlyContinue
    }
    Remove-Item "HKCU:\Software\Classes\Directory\shell\VulkanImageViewer" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "HKCU:\Software\Classes\Directory\Background\shell\VulkanImageViewer" -Recurse -Force -ErrorAction SilentlyContinue

    reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer" /s /f "VulkanImageViewer" 2>$null | ForEach-Object {
        if ($_ -match "^HKEY_") {
            $keyPath = $_
            Remove-Item -Path "Registry::$keyPath" -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # 通知资源管理器刷新（包括缩略图缓存）
    rundll32.exe user32.dll,UpdatePerUserSystemParameters
    Write-Host "已卸载。" -ForegroundColor Green
    exit 0
}

# ---- 安装 ----
Write-Host "Exe 路径: $ExePath" -ForegroundColor Cyan
Write-Host "ProgID:   $ProgID" -ForegroundColor Cyan
Write-Host ""

# 1. 注册 ProgID（用 PowerShell 原生命令，避免 reg add 的引号吞掉和覆盖交互）
$keyProgID = "HKCU:\Software\Classes\$ProgID"
$null = New-Item -Path $keyProgID -Force | Out-Null
Set-ItemProperty -Path $keyProgID -Name "(Default)" -Value $AppName -Type String -Force | Out-Null
Set-ItemProperty -Path $keyProgID -Name "FriendlyTypeName" -Value $AppName -Type String -Force | Out-Null

$null = New-Item -Path "$keyProgID\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "$keyProgID\DefaultIcon" -Name "(Default)" -Value $iconPath -Type String -Force | Out-Null

$null = New-Item -Path "$keyProgID\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "$keyProgID\shell\open\command" -Name "(Default)" -Value $cmdLine -Type String -Force | Out-Null
Write-Host "[1/3] ProgID 已注册" -ForegroundColor Green

# 2. 关联图片扩展名（用 PowerShell 原生命令避免 reg add 的覆盖交互卡住）
foreach ($ext in $extensions) {
    # Classes\$ext 主键
    $keyClasses = "HKCU:\Software\Classes\$ext"
    $null = New-Item -Path $keyClasses -Force | Out-Null

    # OpenWithProgids —— REG_NONE 类型
    $null = New-Item -Path "$keyClasses\OpenWithProgids" -Force | Out-Null
    Remove-ItemProperty -Path "$keyClasses\OpenWithProgids" -Name $ProgID -ErrorAction SilentlyContinue
    New-ItemProperty -Path "$keyClasses\OpenWithProgids" -Name $ProgID -PropertyType None -Value ([byte[]]@()) -Force | Out-Null

    # PerceivedType（标记为图片）
    New-ItemProperty -Path $keyClasses -Name "PerceivedType" -Value "image" -PropertyType String -Force -ErrorAction SilentlyContinue | Out-Null

    # ProgID 作为默认打开程序（设置 (Default)）
    Set-ItemProperty -Path $keyClasses -Name "(Default)" -Value $ProgID -Type String -Force | Out-Null

    # Explorer FileExts 下的 OpenWithProgids（"打开方式"菜单来源）
    $keyFileExts = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext"
    $null = New-Item -Path "$keyFileExts\OpenWithProgids" -Force | Out-Null
    Remove-ItemProperty -Path "$keyFileExts\OpenWithProgids" -Name $ProgID -ErrorAction SilentlyContinue
    New-ItemProperty -Path "$keyFileExts\OpenWithProgids" -Name $ProgID -PropertyType None -Value ([byte[]]@()) -Force | Out-Null

    # OpenWithList 也加一份（备用）
    $null = New-Item -Path "$keyFileExts\OpenWithList" -Force -ErrorAction SilentlyContinue | Out-Null
    $null = New-Item -Path "$keyClasses\OpenWithList\$ProgID" -Force -ErrorAction SilentlyContinue | Out-Null
    Set-ItemProperty -Path "$keyClasses\OpenWithList\$ProgID" -Name "(Default)" -Value "" -Type String -Force -ErrorAction SilentlyContinue | Out-Null
}
Write-Host "[2/3] $($extensions.Count) 个图片扩展名已关联" -ForegroundColor Green

# 3. 文件夹右键菜单（两种：普通 + 背景）
$dirs = @(
    "HKCU:\Software\Classes\Directory\shell\VulkanImageViewer",
    "HKCU:\Software\Classes\Directory\Background\shell\VulkanImageViewer"
)
foreach ($dir in $dirs) {
    $null = New-Item -Path $dir -Force | Out-Null
    Set-ItemProperty -Path $dir -Name "(Default)" -Value "用 Vulkan 图片浏览器打开" -Type String -Force | Out-Null
    Set-ItemProperty -Path $dir -Name "Icon" -Value $iconPath -Type String -Force | Out-Null
    $null = New-Item -Path "$dir\command" -Force | Out-Null
    Set-ItemProperty -Path "$dir\command" -Name "(Default)" -Value $cmdLineDir -Type String -Force | Out-Null
}
Write-Host "[3/4] 文件夹右键菜单已添加" -ForegroundColor Green

# 4. 注册 Thumbnail Handler（DLL + shellex 关联）
$hasDll = Test-Path $DllPath
if ($hasDll) {
    # 注册 CLSID：{CLSID} + InprocServer32(DLL 路径) + ThreadingModel=Apartment
    $keyCLSID = "HKCU:\Software\Classes\CLSID\$ThumbCLSID"
    $null = New-Item -Path $keyCLSID -Force | Out-Null
    Set-ItemProperty -Path $keyCLSID -Name "(Default)" -Value "Vulkan ImageViewer Thumbnail Provider" -Type String -Force | Out-Null

    $null = New-Item -Path "$keyCLSID\InprocServer32" -Force | Out-Null
    Set-ItemProperty -Path "$keyCLSID\InprocServer32" -Name "(Default)" -Value $DllPath -Type String -Force | Out-Null
    Set-ItemProperty -Path "$keyCLSID\InprocServer32" -Name "ThreadingModel" -Value "Apartment" -Type String -Force | Out-Null

    # 给每个扩展名关联 shellex → 我们的 CLSID
    foreach ($ext in $extensions) {
        $keyShellex = "HKCU:\Software\Classes\$ext\shellex\$ThumbShellexGUID"
        $null = New-Item -Path $keyShellex -Force | Out-Null
        Set-ItemProperty -Path $keyShellex -Name "(Default)" -Value $ThumbCLSID -Type String -Force | Out-Null
    }
    Write-Host "[4/4] Thumbnail Handler 已注册" -ForegroundColor Green
} else {
    Write-Host "[4/4] 跳过 Thumbnail Handler（未找到 $DllPath）" -ForegroundColor DarkGray
}

# 5. 尝试写 UserChoice 设为默认（Windows 10/11 hash 保护可能阻止，但试一下）
foreach ($ext in $extensions) {
    $keyUC = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoice"
    $null = New-Item -Path $keyUC -Force -ErrorAction SilentlyContinue | Out-Null
    New-ItemProperty -Path $keyUC -Name "ProgId" -Value $ProgID -Type String -Force -ErrorAction SilentlyContinue | Out-Null
}

# 5. 强制刷新文件关联缓存
if (-not ('Win32' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("shell32.dll", SetLastError=true)]
    public static extern void SHChangeNotify(int wEventId, int uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
"@
}
[Win32]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "注册完成！" -ForegroundColor Green
Write-Host ""
Write-Host "已刷新文件关联缓存（SHChangeNotify）。" -ForegroundColor White
Write-Host "现在请测试：" -ForegroundColor Yellow
Write-Host "  - 右键 .png 文件 → 「打开方式」→ 应该能看到 Vulkan 图片浏览器" -ForegroundColor White
Write-Host "  - 右键文件夹 → 应该有「用 Vulkan 图片浏览器打开」" -ForegroundColor White
Write-Host ""
Write-Host "如果双击 PNG 仍打开「照片」：" -ForegroundColor Yellow
Write-Host "  设置 → 应用 → 默认应用 → 按文件类型选择默认应用" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
