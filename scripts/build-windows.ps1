param(
    [string]$BuildDirectory = "build\windows-x64",
    [switch]$LcdSum3Default,
    [switch]$LowLatencyDefault,
    [switch]$BufferedPaintDefault
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Build Tools introuvable. Installez le composant Developpement Desktop en C++."
}

$installation = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installation) {
    throw "Le compilateur C++ de Visual Studio est absent."
}

$cmake = Join-Path $installation "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
}

$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
$ninja = Join-Path $installation "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat est absent de l'installation Visual Studio."
}
if (-not (Test-Path -LiteralPath $ninja)) {
    throw "Ninja est absent de l'installation Visual Studio."
}

$build = Join-Path $root $BuildDirectory
$sum3Default = if ($LcdSum3Default) { "ON" } else { "OFF" }
$latencySetting = if ($LowLatencyDefault) { "ON" } else { "OFF" }
$paintSetting = if ($BufferedPaintDefault) { "ON" } else { "OFF" }
$command = ('call "{0}" && "{1}" -S "{2}" -B "{3}" -G Ninja ' +
    '-DCMAKE_MAKE_PROGRAM="{4}" -DCMAKE_BUILD_TYPE=Release ' +
    '-DSUPERVISION_LCD_SUM3_DEFAULT={5} -DSUPERVISION_LOW_LATENCY_DEFAULT={6} -DSUPERVISION_BUFFERED_PAINT_DEFAULT={7} && ' +
    '"{1}" --build "{3}"') -f $vcvars, $cmake, $root, $build, $ninja, $sum3Default, $latencySetting, $paintSetting

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Compilation Windows echouee." }

$executable = Join-Path $build "host\SupervisionViewer.exe"
Write-Host "Compile : $executable"
