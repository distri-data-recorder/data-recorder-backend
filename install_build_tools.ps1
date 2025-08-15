# Visual Studio Build Tools 安装脚本
# 用于安装Rust编译所需的Visual Studio Build Tools

param(
    [switch]$AutoInstall,
    [switch]$CheckOnly
)

Write-Host "=== Visual Studio Build Tools 安装助手 ===" -ForegroundColor Green

function Test-BuildTools {
    Write-Host "`n检查Visual Studio Build Tools..." -ForegroundColor Yellow

    # 检查常见的安装路径
    $possiblePaths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC"
    )

    foreach ($path in $possiblePaths) {
        if (Test-Path $path) {
            $versions = Get-ChildItem $path -Directory | Sort-Object Name -Descending
            if ($versions.Count -gt 0) {
                $latestVersion = $versions[0].Name
                Write-Host "✓ 找到Visual Studio Build Tools: $path\$latestVersion" -ForegroundColor Green

                # 检查link.exe
                $linkPath = "$path\$latestVersion\bin\Hostx64\x64\link.exe"
                if (Test-Path $linkPath) {
                    Write-Host "✓ 链接器可用: $linkPath" -ForegroundColor Green
                    return $true
                }
            }
        }
    }

    # 检查PATH中是否有link.exe
    try {
        $linkLocation = Get-Command link.exe -ErrorAction SilentlyContinue
        if ($linkLocation) {
            Write-Host "✓ 在PATH中找到链接器: $($linkLocation.Source)" -ForegroundColor Green
            return $true
        }
    } catch {
        # 忽略错误
    }

    Write-Host "✗ 未找到Visual Studio Build Tools" -ForegroundColor Red
    return $false
}

function Show-InstallInstructions {
    Write-Host "`n=== 安装说明 ===" -ForegroundColor Cyan
    Write-Host "请按照以下步骤安装Visual Studio Build Tools:" -ForegroundColor White
    Write-Host ""
    Write-Host "1. 访问官方下载页面:" -ForegroundColor Yellow
    Write-Host "   https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022" -ForegroundColor Blue
    Write-Host ""
    Write-Host "2. 下载 'Build Tools for Visual Studio 2022'" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "3. 运行安装程序，确保选择以下组件:" -ForegroundColor Yellow
    Write-Host "   ☑ C++ build tools" -ForegroundColor Green
    Write-Host "   ☑ Windows 11 SDK (10.0.22621.0 或更新版本)" -ForegroundColor Green
    Write-Host "   ☑ MSVC v143 - VS 2022 C++ x64/x86 build tools (latest)" -ForegroundColor Green
    Write-Host "   ☑ CMake tools for Visual Studio" -ForegroundColor Green
    Write-Host ""
    Write-Host "4. 安装完成后重启PowerShell" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "5. 重新运行测试: .\quick_test.ps1" -ForegroundColor Yellow
}

function Download-BuildTools {
    Write-Host "`n下载Visual Studio Build Tools..." -ForegroundColor Yellow

    $downloadUrl = "https://aka.ms/vs/17/release/vs_buildtools.exe"
    $downloadPath = ".\vs_buildtools.exe"

    try {
        Write-Host "正在下载..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $downloadUrl -OutFile $downloadPath -UseBasicParsing

        if (Test-Path $downloadPath) {
            Write-Host "✓ 下载完成: $downloadPath" -ForegroundColor Green
            return $downloadPath
        } else {
            Write-Host "✗ 下载失败" -ForegroundColor Red
            return $null
        }
    } catch {
        Write-Host "✗ 下载出错: $_" -ForegroundColor Red
        return $null
    }
}

function Install-BuildTools {
    param($installerPath)

    Write-Host "`n启动安装程序..." -ForegroundColor Yellow
    Write-Host "请在安装程序中选择以下组件:" -ForegroundColor Cyan
    Write-Host "- C++ build tools" -ForegroundColor White
    Write-Host "- Windows 11 SDK" -ForegroundColor White
    Write-Host "- MSVC v143 compiler toolset" -ForegroundColor White

    try {
        Start-Process -FilePath $installerPath -Wait
        Write-Host "✓ 安装程序已完成" -ForegroundColor Green

        # 清理下载的文件
        Remove-Item $installerPath -ErrorAction SilentlyContinue

        return $true
    } catch {
        Write-Host "✗ 启动安装程序失败: $_" -ForegroundColor Red
        return $false
    }
}

function Test-RustCompilation {
    Write-Host "`n测试Rust编译..." -ForegroundColor Yellow

    Push-Location "data-processor"
    try {
        Write-Host "运行 cargo check..." -ForegroundColor Cyan
        $result = & cargo check 2>&1

        if ($LASTEXITCODE -eq 0) {
            Write-Host "✓ Rust编译测试通过" -ForegroundColor Green
            return $true
        } else {
            Write-Host "✗ Rust编译仍有问题" -ForegroundColor Red
            Write-Host "错误信息: $result" -ForegroundColor Gray
            return $false
        }
    } catch {
        Write-Host "✗ 测试过程出错: $_" -ForegroundColor Red
        return $false
    } finally {
        Pop-Location
    }
}

# 主执行流程
try {
    if ($CheckOnly) {
        $buildToolsOk = Test-BuildTools
        if ($buildToolsOk) {
            Write-Host "`n✅ Visual Studio Build Tools 已正确安装" -ForegroundColor Green
            Test-RustCompilation
        } else {
            Write-Host "`n❌ Visual Studio Build Tools 未安装" -ForegroundColor Red
            Show-InstallInstructions
        }
        exit
    }

    # 检查当前状态
    $buildToolsOk = Test-BuildTools

    if ($buildToolsOk) {
        Write-Host "`n✅ Visual Studio Build Tools 已安装" -ForegroundColor Green

        # 测试Rust编译
        $rustOk = Test-RustCompilation
        if ($rustOk) {
            Write-Host "`n🎉 一切就绪！可以运行 .\quick_test.ps1 进行完整测试" -ForegroundColor Green
        } else {
            Write-Host "`n⚠ Rust编译仍有问题，可能需要重启PowerShell或检查环境变量" -ForegroundColor Yellow
        }
    } else {
        Write-Host "`n❌ 需要安装Visual Studio Build Tools" -ForegroundColor Red

        if ($AutoInstall) {
            $installerPath = Download-BuildTools
            if ($installerPath) {
                $installOk = Install-BuildTools $installerPath
                if ($installOk) {
                    Write-Host "`n请重启PowerShell并重新运行此脚本进行验证" -ForegroundColor Yellow
                }
            }
        } else {
            Show-InstallInstructions
            Write-Host "`n提示: 使用 -AutoInstall 参数可自动下载安装程序" -ForegroundColor Cyan
        }
    }

} catch {
    Write-Host "`n❌ 脚本执行出错: $_" -ForegroundColor Red
    exit 1
}