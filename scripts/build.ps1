param(
    [string]$BuildDir = "build-vcpkg",
    [string]$Config = "Release",
    [string]$ToolchainFile = "D:/vcpkg/scripts/buildsystems/vcpkg.cmake",
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"

function Normalize-ProcessPath {
    $pathValue = [Environment]::GetEnvironmentVariable("Path", "Process")
    if ([string]::IsNullOrWhiteSpace($pathValue)) {
        $pathValue = [Environment]::GetEnvironmentVariable("PATH", "Process")
    }

    [Environment]::SetEnvironmentVariable("PATH", $null, "Process")
    [Environment]::SetEnvironmentVariable("Path", $pathValue, "Process")
}

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot $BuildDir

Normalize-ProcessPath

if (!(Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found in PATH"
}

if (!(Test-Path $ToolchainFile)) {
    throw "vcpkg toolchain file not found: $ToolchainFile"
}

Push-Location $repoRoot
try {
    if (!$SkipConfigure) {
        Invoke-Checked "cmake" @(
            "-S", ".",
            "-B", $buildPath,
            "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
        )
    }

    Invoke-Checked "cmake" @(
        "--build", $buildPath,
        "--config", $Config
    )
}
finally {
    Pop-Location
}
