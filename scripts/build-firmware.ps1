param(
    [string]$BuildDirectory = "build\firmware",
    [string[]]$Targets = @()
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$installRoot = $null

if (-not $env:PICO_SDK_PATH) {
    $base = Join-Path $env:ProgramFiles "Raspberry Pi"
    if (Test-Path -LiteralPath $base) {
        $installRoot = Get-ChildItem -LiteralPath $base -Directory -Filter "Pico SDK v*" |
            Sort-Object Name -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($installRoot) {
        $env:PICO_SDK_PATH = Join-Path $installRoot "pico-sdk"
    }
}

if (-not $env:PICO_SDK_PATH) {
    throw "Pico SDK introuvable. Definissez PICO_SDK_PATH ou installez le Pico SDK pour Windows."
}
if (-not (Test-Path -LiteralPath (Join-Path $env:PICO_SDK_PATH "external\pico_sdk_import.cmake"))) {
    throw "PICO_SDK_PATH ne pointe pas vers un Pico SDK valide."
}

if (-not $installRoot) {
    $sdkParent = Split-Path -Parent $env:PICO_SDK_PATH
    if (Test-Path -LiteralPath (Join-Path $sdkParent "gcc-arm-none-eabi")) {
        $installRoot = $sdkParent
    }
}

if ($installRoot) {
    $cmake = Join-Path $installRoot "cmake\bin\cmake.exe"
    $ninja = Join-Path $installRoot "ninja\ninja.exe"
    $toolchain = Join-Path $installRoot "gcc-arm-none-eabi"
    $env:PICO_TOOLCHAIN_PATH = $toolchain
    $env:Path = ((Join-Path $toolchain "bin"),
                 (Join-Path $installRoot "ninja"),
                 (Join-Path $installRoot "python"),
                 (Join-Path $installRoot "git\cmd"),
                 $env:Path) -join ";"
} else {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
}

if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake introuvable." }
if (-not (Test-Path -LiteralPath $ninja)) { throw "Ninja introuvable." }

$build = Join-Path $root $BuildDirectory
& $cmake -S (Join-Path $root "firmware") -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" -DPICO_BOARD=pico
if ($LASTEXITCODE -ne 0) { throw "Configuration du firmware echouee." }

if ($Targets.Count -gt 0) {
    & $cmake --build $build --target @Targets
} else {
    & $cmake --build $build
}
if ($LASTEXITCODE -ne 0) { throw "Compilation du firmware echouee." }

$reportedTargets = if ($Targets.Count -gt 0) { $Targets } else {
    @("supervision_capture", "supervision_capture_6wire", "supervision_capture_proven",
      "supervision_capture_reference_exact", "supervision_capture_polling_fast")
}
foreach ($target in $reportedTargets) {
    $artifact = Join-Path $build "$target.uf2"
    if (-not (Test-Path -LiteralPath $artifact)) {
        throw "Artefact attendu introuvable : $artifact"
    }
    Write-Host "Compile ($target) : $artifact"
}
