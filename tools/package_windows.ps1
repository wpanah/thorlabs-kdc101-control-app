#requires -Version 5.1
param(
    [Parameter(Mandatory)]
    [string]$VcRedist
)

$ErrorActionPreference = 'Stop'
$installer = Get-Item -LiteralPath $VcRedist
if ($installer.VersionInfo.OriginalFilename -ne 'vc_redist.x64.exe') {
    throw 'Supply the Microsoft Visual C++ x64 Redistributable installer.'
}
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-app'
$cache = Join-Path $build 'CMakeCache.txt'

function Get-CacheValue($Name) {
    $entry = Select-String -LiteralPath $cache -Pattern "^${Name}:[^=]+=(.+)$"
    if (-not $entry) { throw "Missing $Name in $cache. Configure the app preset first." }
    $entry.Matches[0].Groups[1].Value
}

# Build the configured lab profile before staging its executable.
$cmake = Get-CacheValue 'CMAKE_COMMAND'
& $cmake --build $build --config Release --target frontend
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$deploy = Join-Path (Get-CacheValue 'Qt6_DIR') '../../../bin/windeployqt.exe'
$temp = Join-Path $build ("usb-" + [guid]::NewGuid())
$stage = Join-Path $temp 'Confo-Quanta'
$archive = Join-Path $build 'Confo-Quanta-win-x64.zip'
$release = Join-Path $build 'Release'
$originalPath = $env:Path
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $exe = Join-Path $stage 'frontend.exe'
    Copy-Item -LiteralPath (Join-Path $release 'frontend.exe') -Destination $exe
    # MSBuild's post-build deployment located the Windows SDK graphics DLLs here.
    $env:Path = "$release;$originalPath"
    & $deploy --release --no-translations --no-compiler-runtime $exe
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'platforms/qwindows.dll'))) {
        throw 'The Qt Windows platform plugin is missing.'
    }
    Copy-Item -LiteralPath $installer.FullName -Destination (Join-Path $stage 'vc_redist.x64.exe')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Start Confo Quanta.bat') -Destination $stage
    @"
Extract the whole Confo-Quanta folder to the lab PC.
Run vc_redist.x64.exe once. If the same or a newer runtime is installed, keep it.
Double-click Start Confo Quanta.bat to launch the application.

Startup requires 64-bit Thorlabs Kinesis and the NI-DAQmx runtime (nicaiu.dll).
For a custom Kinesis installation, add its DLL directory to PATH.
If NI-DAQmx is missing, bring the full NI-DAQmx offline installer for this
Windows version and PCIe-6323 on USB and install it on the lab PC.

Build settings
  Kinesis simulation: $(Get-CacheValue 'KINESIS_SIMULATION')
  Stage travel X/Y/Z (mm): $(Get-CacheValue 'NRT_X_TRAVEL_MM') / $(Get-CacheValue 'NRT_Y_TRAVEL_MM') / $(Get-CacheValue 'NRT_Z_TRAVEL_MM')
  Laser mode: $(Get-CacheValue 'PCIE6323_LASER_MODE') (unconfigured disables laser output)
  Expected PCIe-6323 serial (hex): $(Get-CacheValue 'PCIE6323_SERIAL_HEX')
"@ | Set-Content -LiteralPath (Join-Path $stage 'READ ME.txt') -Encoding ASCII
    $zip = Join-Path $temp 'package.zip'
    Compress-Archive -LiteralPath $stage -DestinationPath $zip
    Move-Item -LiteralPath $zip -Destination $archive -Force
    Write-Host "USB package: $archive"
} finally {
    $env:Path = $originalPath
    $resolved = (Resolve-Path -LiteralPath $temp).Path
    if ($resolved -eq $temp -and (Split-Path -Parent $resolved) -eq $build) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
