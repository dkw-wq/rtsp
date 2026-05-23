param(
    [string]$BuildDir = "build-vcpkg",
    [string]$Config = "Release",
    [string]$ToolchainFile = "",
    [string]$Triplet = "x64-windows",
    [string]$Preset = "windows-vcpkg",
    [switch]$EnableCudaFfmpeg,
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

function Resolve-VcpkgRoot {
    param(
        [string]$RequestedToolchainFile
    )

    if (![string]::IsNullOrWhiteSpace($RequestedToolchainFile)) {
        if (!(Test-Path -LiteralPath $RequestedToolchainFile)) {
            throw "vcpkg toolchain file not found: $RequestedToolchainFile"
        }

        $resolvedToolchain = Resolve-Path -LiteralPath $RequestedToolchainFile
        return Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $resolvedToolchain))
    }

    $envRoot = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "Process")
    if (![string]::IsNullOrWhiteSpace($envRoot)) {
        $envToolchain = Join-Path $envRoot "scripts/buildsystems/vcpkg.cmake"
        if (Test-Path -LiteralPath $envToolchain) {
            return (Resolve-Path -LiteralPath $envRoot).Path
        }
    }

    $vcpkgCommand = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkgCommand) {
        return Split-Path -Parent $vcpkgCommand.Source
    }

    throw "vcpkg not found. Set VCPKG_ROOT or pass -ToolchainFile."
}

function Test-LegacyNonManifestBuild {
    param(
        [string]$Path
    )

    $cachePath = Join-Path $Path "CMakeCache.txt"
    if (!(Test-Path -LiteralPath $cachePath)) {
        return $false
    }

    $manifestModeOff = [bool](Select-String `
        -Path $cachePath `
        -Pattern "^VCPKG_MANIFEST_MODE:BOOL=OFF$" `
        -Quiet)
    $manifestInstallOff = [bool](Select-String `
        -Path $cachePath `
        -Pattern "^VCPKG_MANIFEST_INSTALL:.*=OFF$" `
        -Quiet)

    return $manifestModeOff -or $manifestInstallOff
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot $BuildDir

Normalize-ProcessPath

if (!(Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found in PATH"
}

$vcpkgRoot = Resolve-VcpkgRoot $ToolchainFile
$toolchainPath = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
if (!(Test-Path -LiteralPath $toolchainPath)) {
    throw "vcpkg toolchain file not found: $toolchainPath"
}
if (!(Test-Path -LiteralPath $vcpkgExe)) {
    throw "vcpkg executable not found: $vcpkgExe"
}
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $vcpkgRoot, "Process")

$legacyManifestDisabled = Test-LegacyNonManifestBuild $buildPath
if ($legacyManifestDisabled) {
    Write-Warning "Existing build directory was configured before vcpkg manifest mode. Delete '$buildPath' and rerun this script to use vcpkg.json."
}

$manifestFeatures = @()
if ($EnableCudaFfmpeg -and $legacyManifestDisabled) {
    Invoke-Checked $vcpkgExe @(
        "install",
        "ffmpeg[avcodec,avdevice,avfilter,avformat,swresample,swscale,srt,nvcodec]:$Triplet",
        "--recurse"
    )
} elseif ($EnableCudaFfmpeg) {
    $manifestFeatures += "cuda-ffmpeg"
}
$manifestFeatureValue = $manifestFeatures -join ";"

Push-Location $repoRoot
try {
    if (!$SkipConfigure) {
        $usePreset = [string]::IsNullOrWhiteSpace($ToolchainFile) -and
            $BuildDir -eq "build-vcpkg" -and
            (Test-Path -LiteralPath (Join-Path $repoRoot "CMakePresets.json"))

        if ($usePreset) {
            $configureArgs = @(
                "--preset", $Preset,
                "-DVCPKG_TARGET_TRIPLET=$Triplet"
            )
        } else {
            $configureArgs = @(
                "-S", ".",
                "-B", $buildPath,
                "-DCMAKE_TOOLCHAIN_FILE=$toolchainPath",
                "-DVCPKG_TARGET_TRIPLET=$Triplet"
            )
        }

        if (![string]::IsNullOrWhiteSpace($manifestFeatureValue)) {
            $configureArgs += "-DVCPKG_MANIFEST_FEATURES=$manifestFeatureValue"
        }
        if ($legacyManifestDisabled) {
            $configureArgs += "-DVCPKG_MANIFEST_MODE=OFF"
        }

        Invoke-Checked "cmake" $configureArgs
    }

    Invoke-Checked "cmake" @(
        "--build", $buildPath,
        "--config", $Config
    )
}
finally {
    Pop-Location
}
