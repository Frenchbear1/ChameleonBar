param(
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$Compiler = 'C:\Program Files\Windhawk\Compiler\bin\clang++.exe'
$Python = Join-Path $env:USERPROFILE `
    '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    $Python = (Get-Command python.exe -ErrorAction Stop).Source
}
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = if ($OutputDirectory) {
    if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
        $OutputDirectory
    } else {
        Join-Path $Root $OutputDirectory
    }
} else {
    Join-Path $Root 'build'
}

New-Item -ItemType Directory -Force -Path $Build | Out-Null

$IconResource = Join-Path $Build 'ChameleonBar-icon.res'
& $Python (Join-Path $Root 'tools\make_icon_resource.py') `
    (Join-Path $Root 'assets\chameleonbar.ico') $IconResource
if ($LASTEXITCODE -ne 0) { throw 'Application icon resource build failed.' }

& $Compiler --target=x86_64-w64-mingw32 -std=c++23 -O2 -shared -Wall -Wextra `
    -Wno-dll-attribute-on-redeclaration `
    -I 'C:\Program Files\Windhawk\Compiler\include' `
    (Join-Path $Root 'bridge\xaml_bridge.cpp') `
    -static -static-libgcc -static-libstdc++ `
    -lole32 -loleaut32 -lruntimeobject -lgdi32 -luser32 -luuid `
    -o (Join-Path $Build 'ChameleonBar.XamlBridge.dll')
if ($LASTEXITCODE -ne 0) { throw 'Local XAML bridge build failed.' }

& $Compiler --target=x86_64-w64-mingw32 -std=c++20 -O2 -Wall -Wextra `
    -I (Join-Path $Root 'src') `
    (Join-Path $Root 'tests\sampler_tests.cpp') `
    (Join-Path $Root 'src\color_sampler.cpp') `
    (Join-Path $Root 'src\surface_publisher.cpp') `
    (Join-Path $Root 'src\window_activity.cpp') `
    -static -static-libgcc -static-libstdc++ `
    -luser32 -ldwmapi `
    -o (Join-Path $Build 'sampler_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Sampler test build failed.' }

& (Join-Path $Build 'sampler_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Sampler tests failed.' }

& $Compiler --target=x86_64-w64-mingw32 -std=c++20 -O2 -Wall -Wextra `
    -municode -mwindows `
    -I (Join-Path $Root 'src') `
    (Join-Path $Root 'src\main.cpp') `
    (Join-Path $Root 'src\color_sampler.cpp') `
    (Join-Path $Root 'src\settings.cpp') `
    (Join-Path $Root 'src\surface_publisher.cpp') `
    (Join-Path $Root 'src\window_activity.cpp') `
    (Join-Path $Root 'src\xaml_bridge_client.cpp') `
    $IconResource `
    -static -static-libgcc -static-libstdc++ `
    -luser32 -lgdi32 -lshell32 -ldwmapi -ladvapi32 -lole32 -luuid -lcomctl32 -luxtheme `
    -o (Join-Path $Build 'ChameleonBar.exe')
if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }

Write-Host "Built: $Build\ChameleonBar.exe"
Write-Host "Built: $Build\ChameleonBar.XamlBridge.dll"
