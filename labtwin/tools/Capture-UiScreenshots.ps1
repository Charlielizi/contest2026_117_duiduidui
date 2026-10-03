# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PreviewRoot,
    [string]$OutputDirectory,
    [string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
$releaseRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ui = Join-Path $releaseRoot 'labtwin\overlay\vendor\allwinnertech\apps\luncher_mini'
$preview = (Resolve-Path -LiteralPath $PreviewRoot).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $releaseRoot 'labtwin\docs\images' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path ([IO.Path]::GetTempPath()) ('labtwin-capture-' + [Guid]::NewGuid().ToString('N')) }
$gcc = Join-Path $preview '.tools\w64devkit\bin\gcc.exe'
$sdl = Join-Path $preview '.tools\SDL2-2.32.10\x86_64-w64-mingw32'
$lvgl = Join-Path $preview 'third_party\lvgl'
$library = Join-Path $preview 'build\liblvgl-preview.a'
$font = Join-Path $preview 'assets\MiSans-Normal.ttf'
foreach ($required in @($gcc, $library, $font, (Join-Path $sdl 'bin\SDL2.dll'),
                         (Join-Path $preview 'lv_conf.h'), (Join-Path $lvgl 'lvgl.h'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing prepared preview dependency: $required" }
}
New-Item -ItemType Directory -Path $BuildDirectory,$OutputDirectory -Force | Out-Null
$exe = Join-Path (Resolve-Path -LiteralPath $BuildDirectory).Path 'capture_ui.exe'
Copy-Item -LiteralPath (Join-Path $sdl 'bin\SDL2.dll') -Destination (Join-Path $BuildDirectory 'SDL2.dll')
$ccArgs = @('-DLV_CONF_INCLUDE_SIMPLE', '-DLV_USE_LODEPNG=1',
    ('-I' + (Join-Path $preview 'compat')), ('-I' + $preview),
    ('-I' + (Join-Path $preview 'third_party')), ('-I' + (Join-Path $sdl 'include')), ('-I' + $ui),
    '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
    '-ffunction-sections', '-fdata-sections', '-include', 'string.h',
    (Join-Path $PSScriptRoot 'capture_ui.c'))
foreach ($name in @('lab_ui.c', 'lab_ui_flow.c', 'lab_ui_demo.c', 'ui_tokens.c')) {
    $ccArgs += Join-Path $ui $name
}
$ccArgs += @((Join-Path $lvgl 'src\libs\lodepng\lodepng.c'), $library,
    ('-L' + (Join-Path $sdl 'lib')), '-Wl,--gc-sections', '-lSDL2', '-lm', '-o', $exe)
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $gcc) + ';' + $previousPath
    & $gcc @ccArgs
    if ($LASTEXITCODE -ne 0) { throw 'Capture harness compilation failed' }
    foreach ($scene in @('idle', 'running', 'timers', 'voice', 'alert', 'complete-confirm')) {
        & $exe $scene (Join-Path $OutputDirectory ('sim-ui-' + $scene + '.png')) $font
        if ($LASTEXITCODE -ne 0) { throw "Capture failed: $scene" }
    }
} finally {
    $env:PATH = $previousPath
}
Write-Host 'Six offscreen C/LVGL mock frames captured; no board/network/OS input used.'
Write-Host "Capture build retained for inspection: $BuildDirectory"
