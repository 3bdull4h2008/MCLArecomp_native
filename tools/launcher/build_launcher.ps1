# Build MCLA Launcher (WPF WebView2, per-pixel alpha) -> build/mcla_launcher.exe
# Replaces the old GDI+ launcher. Game exe (build/mcla.exe) is never touched.
$ErrorActionPreference = 'Stop'
$root  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$src   = Join-Path $PSScriptRoot 'LauncherHost.cs'
$build = Join-Path $root 'build'
$out   = Join-Path $build 'mcla_launcher.exe'
$fx    = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$wpf   = Join-Path $fx 'WPF'
$deps  = Join-Path $PSScriptRoot 'deps'

$csc = @(
  'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe',
  'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe',
  'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw 'Roslyn csc.exe not found' }

$refs = @(
  ('/r:"' + $fx  + '\mscorlib.dll"'),
  ('/r:"' + $fx  + '\System.dll"'),
  ('/r:"' + $fx  + '\System.Core.dll"'),
  ('/r:"' + $fx  + '\System.Drawing.dll"'),
  ('/r:"' + $fx  + '\System.Web.Extensions.dll"'),
  ('/r:"' + $fx  + '\System.Xaml.dll"'),
  ('/r:"' + $wpf + '\WindowsBase.dll"'),
  ('/r:"' + $wpf + '\PresentationCore.dll"'),
  ('/r:"' + $wpf + '\PresentationFramework.dll"'),
  ('/r:"' + (Join-Path $deps 'Microsoft.Web.WebView2.Core.dll') + '"'),
  ('/r:"' + (Join-Path $deps 'Microsoft.Web.WebView2.Wpf.dll') + '"')
)
$icon = Join-Path $PSScriptRoot 'launcher.ico'
$iconArg = @()
if (Test-Path $icon) { $iconArg = @('/win32icon:"' + $icon + '"') }

& $csc /nologo /target:winexe /langversion:latest /platform:x64 /out:"$out" @iconArg @refs $src
if ($LASTEXITCODE -ne 0) { throw "csc failed: $LASTEXITCODE" }

# Stage WebView2 runtime deps + UI + icon next to the exe
Copy-Item (Join-Path $deps 'Microsoft.Web.WebView2.Core.dll') $build -Force
Copy-Item (Join-Path $deps 'Microsoft.Web.WebView2.Wpf.dll')  $build -Force
Copy-Item (Join-Path $deps 'WebView2Loader.dll')              $build -Force
Copy-Item $icon $build -Force

$uiSrc = Join-Path $PSScriptRoot 'ui'
$uiDst = Join-Path $build 'launcher\ui'
New-Item -ItemType Directory -Force -Path $uiDst | Out-Null
Copy-Item (Join-Path $uiSrc '*') $uiDst -Force -Recurse

Write-Host "Built $out"
