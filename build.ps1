# Build (and optionally flash) sidewalk_nus against the Sidewalk add-on workspace.
#   .\build.ps1                  incremental build
#   .\build.ps1 -Pristine        clean build
#   .\build.ps1 -Flash -Snr <id> build and flash the DK with that J-Link serial number
#   add -Erase on first flash: full chip erase + MCUboot KMU key provisioning (keyfile.json),
#   without it MCUboot cannot verify the app and stays silent
# -Toolchain: NCS v3.4.0 toolchain dir (nrfutil sdk-manager toolchain list), defaults to $env:NCS_TOOLCHAIN
# -Workspace: Sidewalk add-on west workspace, defaults to $env:SIDEWALK_WORKSPACE
param(
    [switch]$Pristine,
    [switch]$Flash,
    [switch]$Erase,
    [string]$Snr,
    [string]$Board = 'nrf54l15dk/nrf54l15/cpuapp',
    [string]$Toolchain = $env:NCS_TOOLCHAIN,
    [string]$Workspace = $env:SIDEWALK_WORKSPACE
)

# Not 'Stop': PowerShell 5.1 would abort on west's progress output to stderr
$ErrorActionPreference = 'Continue'
if (-not $Toolchain -or -not (Test-Path $Toolchain)) { throw 'pass -Toolchain <NCS v3.4.0 toolchain dir> or set NCS_TOOLCHAIN' }
if (-not $Workspace -or -not (Test-Path "$Workspace\sidewalk")) { throw 'pass -Workspace <Sidewalk add-on workspace> or set SIDEWALK_WORKSPACE' }
$tc = $Toolchain
$ws = $Workspace
$app = $PSScriptRoot
$build = Join-Path $app 'build'

$env:PATH = "$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;" + $env:PATH
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:NRFUTIL_HOME = "$tc\nrfutil\home"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_BASE = "$ws\zephyr"

# west must run inside the Sidewalk workspace to resolve its modules
Push-Location $ws
try {
    $p = if ($Pristine) { 'always' } else { 'auto' }
    west build -p $p -b $Board -d $build $app
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

    if ($Flash) {
        if (-not $Snr) { throw 'pass -Snr <J-Link serial> (see: nrfutil device list)' }
        $flashArgs = @('-d', $build, '--dev-id', $Snr)
        if ($Erase) { $flashArgs += '--erase' }
        west flash @flashArgs
        if ($LASTEXITCODE -ne 0) { throw "flash failed ($LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}
