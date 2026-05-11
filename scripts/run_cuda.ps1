param(
    [string]$ExePath = "build-vcpkg/bin/Release/rtsp_player.exe",
    [string]$CudnnBin = "C:/Program Files/NVIDIA/CUDNN/v9.22/bin/13.2/x64",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = "Stop"

$resolvedExe = Resolve-Path -LiteralPath $ExePath
$resolvedCudnn = Resolve-Path -LiteralPath $CudnnBin
$cudnnDll = Join-Path $resolvedCudnn "cudnn64_9.dll"

if (-not (Test-Path -LiteralPath $cudnnDll)) {
    throw "cuDNN runtime DLL not found: $cudnnDll"
}

$env:Path = "$resolvedCudnn;$env:Path"
& $resolvedExe @Args
exit $LASTEXITCODE
