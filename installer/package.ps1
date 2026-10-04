# Builds the fix DLLs and the installer, then assembles the shareable packages in ..\dist:
#   HollowFix_v<ver>_Installer.zip  - HollowFixInstaller.exe + README.txt
#   HollowFix_v<ver>_Manual.zip     - ready-to-copy folders (both / controller only / HUD only),
#                                     README.txt and the source code
# Needs Visual Studio 2022 Build Tools (C++).
$ErrorActionPreference = 'Stop'
$ver   = '1.0'
$here  = $PSScriptRoot
$src   = Join-Path $here '..\src' | Resolve-Path
$dist  = Join-Path $here '..\dist'
$vc    = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'

function Invoke-VC($cmd) {
    cmd /c "`"$vc`" >nul 2>nul && $cmd 2>&1"
    if ($LASTEXITCODE) { throw "Build step failed: $cmd" }
}

# Compress-Archive in Windows PowerShell writes backslashes into entry names, which some unzip
# tools don't treat as folders; write standard forward-slash entries instead.
function New-Zip($dir, $zipPath) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        $root = (Resolve-Path $dir).Path.TrimEnd('\') + '\'
        Get-ChildItem $dir -Recurse -File | ForEach-Object {
            $name = $_.FullName.Substring($root.Length).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $name, 'Optimal') | Out-Null
        }
    } finally { $zip.Dispose() }
}

# 1. Fix DLLs
cmd /c "`"$src\build.bat`" 2>nul"
if ($LASTEXITCODE) { throw 'DLL build failed' }

# 2. Installer (DLLs + default ini are embedded as resources)
Push-Location $here
try {
    New-Item -ItemType Directory -Force obj | Out-Null
    Invoke-VC 'rc /nologo /fo obj\HollowFixInstaller.res HollowFixInstaller.rc'
    Invoke-VC 'cl /nologo /utf-8 /O2 /W3 /MT /EHsc /DUNICODE /D_UNICODE HollowFixInstaller.cpp obj\HollowFixInstaller.res /Fo:obj\ /Fe:obj\HollowFixInstaller.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO'
} finally { Pop-Location }

# 3. Package layout
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$stage = Join-Path $dist 'stage'
$inst  = Join-Path $stage 'installer'
$man   = Join-Path $stage 'manual'
New-Item -ItemType Directory -Force $inst, $man | Out-Null

Copy-Item "$here\obj\HollowFixInstaller.exe", "$here\README.txt" $inst

$iniText = Get-Content "$here\HollowInputFix.ini" -Raw
$variants = @(
    @{ Dir = '1 - Both Fixes';          Ctrl = 1; Hud = 1 },
    @{ Dir = '2 - Controller Fix Only'; Ctrl = 1; Hud = 0 },
    @{ Dir = '3 - HUD Fix Only';        Ctrl = 0; Hud = 1 }
)
foreach ($v in $variants) {
    $d = New-Item -ItemType Directory -Force (Join-Path $man $v.Dir)
    Copy-Item "$src\out\xinput1_3.dll" $d
    if ($v.Ctrl) { Copy-Item "$src\out\dinput8.dll" $d }
    $ini = $iniText -replace '(?m)^ControllerFix=\d', "ControllerFix=$($v.Ctrl)" -replace '(?m)^HUDFix=\d', "HUDFix=$($v.Hud)"
    [IO.File]::WriteAllText((Join-Path $d 'HollowInputFix.ini'), $ini)
}
Copy-Item "$here\README.txt" $man
$s = New-Item -ItemType Directory -Force (Join-Path $man 'Source')
Copy-Item "$src\HollowInputFix.c", "$src\xinput1_3.def", "$src\dinput8.def", "$src\build.bat" $s
$si = New-Item -ItemType Directory -Force (Join-Path $s 'installer')
Copy-Item "$here\HollowFixInstaller.cpp", "$here\HollowFixInstaller.rc", "$here\app.manifest", "$here\resource.h", "$here\app.ico", "$here\HollowInputFix.ini", "$here\package.ps1" $si

New-Zip $inst (Join-Path $dist "HollowFix_v${ver}_Installer.zip")
New-Zip $man  (Join-Path $dist "HollowFix_v${ver}_Manual.zip")
Copy-Item "$here\obj\HollowFixInstaller.exe" $dist
Remove-Item $stage -Recurse -Force
Get-ChildItem $dist | ForEach-Object { "{0,-34} {1,10:N0}" -f $_.Name, $_.Length }
