<#
    build.ps1 - builds the HSMAdvisor Plugin for Fusion 360 add-in.

    Produces the native add-in and assembles the drop-in folder under
    "dist\HSMAdvisor Plugin" (folder/dll/manifest are all named "HSMAdvisor Plugin" so
    Fusion lists the add-in under that name), together with the bundled copy of
    HSMAdvisor's native integration client (third_party\hsmadvisor\x64\HSMAdvisorIntegration.dll).

    Requirements:
      - Visual Studio 2022/2026 with the "Desktop development with C++" workload
        (MSVC + Windows SDK).
      - Fusion 360 installed (its C++ API headers live under
        %APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP).
      HSMAdvisor itself is only needed to run the add-in, not to build it.

    Usage:
      .\build.ps1                 # Release build + assemble dist\
      .\build.ps1 -Configuration Debug
      .\build.ps1 -Package        # also build the Inno Setup installer
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$Package,   # also build the Inno Setup installer
    [switch]$Deploy     # also copy the built add-in into Fusion's AddIns folder (dev)
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Fusion shows the add-in under this name (folder == manifest == dll base name).
$AddinName = 'HSMAdvisor Plugin'

$Version = ([regex]::Match((Get-Content "$root\HSMAdvisorPlugin.manifest" -Raw), '"version"\s*:\s*"([^"]+)"')).Groups[1].Value
if (-not $Version) { throw "Could not read version from HSMAdvisorPlugin.manifest" }

function Find-VS {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio with the 'Desktop development with C++' workload." }
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { $vs = & $vswhere -latest -property installationPath }
    if (-not $vs) { throw "Visual Studio (with C++ tools) not found." }
    return $vs
}

$vs = Find-VS
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$integrationDll = Join-Path $root 'third_party\hsmadvisor\x64\HSMAdvisorIntegration.dll'
if (-not (Test-Path $integrationDll)) { throw "Bundled HSMAdvisorIntegration.dll not found: $integrationDll" }
Write-Host "Visual Studio : $vs"
Write-Host "Configuration : $Configuration"
Write-Host "Version       : $Version`n"

# 1) native add-in (HSMAdvisorPlugin.dll)
& $msbuild "$root\HSMAdvisorPlugin.vcxproj" /t:Build /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed." }
$dll = Join-Path $root "$Configuration\HSMAdvisorPlugin.dll"

# 2) assemble the drop-in folder (folder/dll/manifest all named "$AddinName")
$dist = Join-Path $root "dist\$AddinName"
if (Test-Path (Join-Path $root 'dist')) { Remove-Item (Join-Path $root 'dist') -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'resources\HSMAdvisorPlugin') | Out-Null
Copy-Item $dll (Join-Path $dist "$AddinName.dll")
# fallback copy of the integration client; the one installed with HSMAdvisor is preferred
Copy-Item $integrationDll $dist
Copy-Item "$root\HSMAdvisorPlugin.manifest" (Join-Path $dist "$AddinName.manifest")
Copy-Item "$root\AddInIcon.svg" $dist
Copy-Item "$root\resources\HSMAdvisorPlugin\*.png" (Join-Path $dist 'resources\HSMAdvisorPlugin')
foreach ($f in 'LICENSE', 'README.md') {
    if (Test-Path "$root\$f") { Copy-Item "$root\$f" $dist }
}
Write-Host "`nAssembled drop-in folder: $dist"

# 3) optional: deploy into Fusion's AddIns folder for local testing
if ($Deploy) {
    $addins = Join-Path $env:APPDATA "Autodesk\Autodesk Fusion 360\API\AddIns\$AddinName"
    New-Item -ItemType Directory -Force -Path (Join-Path $addins 'resources\HSMAdvisorPlugin') | Out-Null
    try {
        Copy-Item "$dist\*" $addins -Recurse -Force
        Write-Host "Deployed to: $addins"
    }
    catch {
        Write-Warning "Deploy failed (is the add-in loaded in Fusion? Stop it first): $($_.Exception.Message)"
    }
}

# 4) optional installer
if ($Package) {
    $iscc = Get-ChildItem `
        "${env:ProgramFiles(x86)}\Inno Setup*\ISCC.exe", `
        "$env:ProgramFiles\Inno Setup*\ISCC.exe", `
        "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup (ISCC.exe) not found - install it or run without -Package." }
    # Pass the manifest version into the .iss (single source of truth).
    & $iscc.FullName "/DMyVersion=$Version" "$root\installer\HSMAdvisorPlugin.iss"
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup build failed." }
    Write-Host "Installer built in: $root\installer\Output"

    
    try {
        if (-not (& git -C $root tag --list "v$Version")) {
            Write-Warning "No git tag 'v$Version' yet - remember to tag the release."
        }
    }
    catch { }
}

Write-Host "`nDone."
