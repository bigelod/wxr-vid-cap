# Builds every target: 32-bit helpers first, then the 64-bit solution.
#
#   .\build.ps1              # Release
#   .\build.ps1 -Configuration Debug

param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $msbuild)) {
    throw "MSBuild not found at $msbuild"
}

$solutionDir = "$($root.Replace('\', '/'))/"

function Build-Project([string]$project, [string]$platform) {
    Write-Host "== $project ($platform, $Configuration)"
    & $msbuild "$root\$project" "/p:Configuration=$Configuration" "/p:Platform=$platform" `
        "/p:SolutionDir=$solutionDir" /p:PreferredToolArchitecture=x64 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) {
        throw "$project failed"
    }
}

# 32-bit targets. These must be built directly because the solution is x64-only.
Build-Project "tools\wxr_hook_dll.vcxproj" "Win32"
Build-Project "tools\inject32.vcxproj" "Win32"
Build-Project "tools\d3d9_test.vcxproj" "Win32"
Build-Project "tools\inject_probe_dll.vcxproj" "Win32"
Build-Project "tools\inject_probe.vcxproj" "Win32"

# Everything else, including the tray app and the 64-bit hook DLL.
Write-Host "== wxr-vid-cap.sln (x64, $Configuration)"
& $msbuild "$root\wxr-vid-cap.sln" "/p:Configuration=$Configuration" "/p:Platform=x64" `
    /p:PreferredToolArchitecture=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    throw "solution build failed"
}

Build-Project "tools\wxr_launch.vcxproj" "x64"
Build-Project "tools\wxr_launch.vcxproj" "Win32"

# The tray app injects these from beside its own EXE. The hook projects build into build\tools,
# so copy the current binaries next to the app after every build.
Copy-Item "$root\build\tools\x64\$Configuration\wxr_hook_dll.dll" "$root\build\x64\$Configuration\wxr_hook_dll.dll" -Force
Copy-Item "$root\build\tools\Win32\$Configuration\wxr_hook_dll32.dll" "$root\build\x64\$Configuration\wxr_hook_dll32.dll" -Force
Copy-Item "$root\build\tools\Win32\$Configuration\wxr_inject32.exe" "$root\build\x64\$Configuration\wxr_inject32.exe" -Force
Copy-Item "$root\src\capture-blacklist.txt" "$root\build\x64\$Configuration\capture-blacklist.txt" -Force

# Capture test apps (one per graphics API) and the automated runner.
foreach ($app in @("cube_d3d7", "cube_d3d8", "cube_d3d9", "cube_d3d10", "cube_d3d11", "cube_d3d12", "cube_ddraw", "cube_gl", "cube_vk", "cube_runner")) {
    Build-Project "tools\testapps\$app.vcxproj" "x64"
}

# 32-bit builds for the runner's 32-bit passes. The container's d3d8 is 32-bit, so this is the
# path that actually runs the D3D8 test on-device.
foreach ($app in @("cube_d3d7", "cube_d3d8", "cube_d3d9", "cube_d3d10", "cube_d3d11", "cube_d3d12", "cube_ddraw", "cube_gl")) {
    Build-Project "tools\testapps\$app.vcxproj" "Win32"
    Copy-Item "$root\build\testapps\Win32\$Configuration\${app}32.exe" "$root\build\testapps\$Configuration\${app}32.exe" -Force
}

Write-Host ""
Write-Host "Outputs:"
Get-ChildItem "$root\build\x64\$Configuration" -Filter "*.exe" | ForEach-Object { "  app   $($_.FullName)" }
Get-ChildItem "$root\build\x64\$Configuration" -Filter "*.dll" | ForEach-Object { "  app   $($_.FullName)" }
Get-ChildItem "$root\build\tools\Win32\$Configuration" -Filter "*.exe" | ForEach-Object { "  win32 $($_.FullName)" }
Get-ChildItem "$root\build\tools\Win32\$Configuration" -Filter "*.dll" | ForEach-Object { "  win32 $($_.FullName)" }
