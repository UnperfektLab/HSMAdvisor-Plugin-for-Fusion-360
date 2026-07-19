<#
    build.ps1 - builds the HSMAdvisor Plugin for Fusion 360 add-in.

    Produces the native add-in and the dialog host (HSMAdvisorPluginHost.exe), and
    assembles the drop-in folder under "dist\HSMAdvisor Plugin" (folder/dll/manifest
    are all named "HSMAdvisor Plugin" so Fusion lists the add-in under that name).

    Requirements:
      - Visual Studio 2022/2026 with the "Desktop development with C++" workload
        (MSVC + Windows SDK) and the Roslyn C# compiler (bundled).
      - HSMAdvisor installed (https://hsmadvisor.com/download) - its DLLs are
        referenced at build time and loaded from the installation at runtime.
      - Fusion 360 installed (its C++ API headers live under
        %APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP).

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

function Find-VS {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio with the 'Desktop development with C++' workload." }
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { $vs = & $vswhere -latest -property installationPath }
    if (-not $vs) { throw "Visual Studio (with C++ tools) not found." }
    return $vs
}

function Find-HSMAdvisor {
    $keys = @(
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($r in $keys) {
        if (-not (Test-Path $r)) { continue }
        foreach ($k in Get-ChildItem $r -ErrorAction SilentlyContinue) {
            $p = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
            if ($p.DisplayName -match 'HSMAdvisor' -and $p.InstallLocation `
                    -and (Test-Path (Join-Path $p.InstallLocation 'HSMAdvisorCore.dll'))) {
                return $p.InstallLocation.TrimEnd('\')
            }
        }
    }
    foreach ($c in @("$env:LOCALAPPDATA\Programs\HSMAdvisor", "$env:ProgramFiles\HSMAdvisor")) {
        if (Test-Path (Join-Path $c 'HSMAdvisorCore.dll')) { return $c }
    }
    throw "HSMAdvisor installation not found - install it from https://hsmadvisor.com/download first."
}

$vs = Find-VS
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$hsm = Find-HSMAdvisor
Write-Host "Visual Studio : $vs"
Write-Host "HSMAdvisor    : $hsm"
Write-Host "Configuration : $Configuration`n"

# 1) native add-in (HSMAdvisorPlugin.dll)
& $msbuild "$root\HSMAdvisorPlugin.vcxproj" /t:Build /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed." }
$dll = Join-Path $root "$Configuration\HSMAdvisorPlugin.dll"

# 2) dialog host (HSMAdvisorPluginHost.exe), referencing the installed HSMAdvisor DLLs
$hostExe = Join-Path $root 'HSMAdvisorPluginHost.exe'
& $csc /nologo /target:winexe /platform:x64 /langversion:latest `
    /reference:"$hsm\HSMAdvisorCore.dll" /reference:"$hsm\HSMAdvisorDatabase.dll" `
    /reference:'System.Windows.Forms.dll' /reference:'System.Drawing.dll' `
    /reference:'System.dll' /reference:'System.Core.dll' /reference:'System.Xml.dll' `
    /out:"$hostExe" "$root\HSMAdvisorPluginHost\Program.cs"
if ($LASTEXITCODE -ne 0) { throw "Host (C#) build failed." }

# 3) assemble the drop-in folder (folder/dll/manifest all named "$AddinName")
$dist = Join-Path $root "dist\$AddinName"
if (Test-Path (Join-Path $root 'dist')) { Remove-Item (Join-Path $root 'dist') -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'resources\HSMAdvisorPlugin') | Out-Null
Copy-Item $dll (Join-Path $dist "$AddinName.dll")
Copy-Item $hostExe $dist
Copy-Item "$root\HSMAdvisorPlugin.manifest" (Join-Path $dist "$AddinName.manifest")
Copy-Item "$root\AddInIcon.svg" $dist
Copy-Item "$root\resources\HSMAdvisorPlugin\*.png" (Join-Path $dist 'resources\HSMAdvisorPlugin')
foreach ($f in 'LICENSE', 'README.md') {
    if (Test-Path "$root\$f") { Copy-Item "$root\$f" $dist }
}
Write-Host "`nAssembled drop-in folder: $dist"

# 4) optional: deploy into Fusion's AddIns folder for local testing
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

# 5) optional installer
if ($Package) {
    $iscc = Get-ChildItem `
        "${env:ProgramFiles(x86)}\Inno Setup*\ISCC.exe", `
        "$env:ProgramFiles\Inno Setup*\ISCC.exe", `
        "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup (ISCC.exe) not found - install it or run without -Package." }
    & $iscc.FullName "$root\installer\HSMAdvisorPlugin.iss"
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup build failed." }
    Write-Host "Installer built in: $root\installer\Output"
}

Write-Host "`nDone."
