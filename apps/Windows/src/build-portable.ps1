# Build a stand-alone portable DC-OGX-W.exe, no MSI/setup program.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Assert-Tool($tool) {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "Required tool '$tool' is missing. See BUILD_WINDOWS.md."
  }
}
Assert-Tool 'node'
Assert-Tool 'npm.cmd'
Assert-Tool 'rustc'
Assert-Tool 'cargo'

Write-Host 'Checking environment...' -ForegroundColor Cyan
node --version
npm.cmd --version
rustc --version
cargo --version

Write-Host 'Installing Tauri CLI development dependency...' -ForegroundColor Cyan
npm.cmd install --no-audit --no-fund
if ($LASTEXITCODE -ne 0) { throw 'npm install failed' }

Write-Host 'Building optimized portable EXE...' -ForegroundColor Cyan
npm.cmd run build
if ($LASTEXITCODE -ne 0) { throw 'Tauri build failed' }

$release = Join-Path $root 'src-tauri\target\release\dc-ogx-w.exe'
if (-not (Test-Path $release)) { throw "Build finished but $release was not found" }
$dest = Join-Path $root 'dist-portable'
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item $release (Join-Path $dest 'DC-OGX-W.exe') -Force
Copy-Item (Join-Path $root 'PORTABLE_README.txt') (Join-Path $dest 'README.txt') -Force
Write-Host "Portable output: $dest" -ForegroundColor Green
Get-Item (Join-Path $dest 'DC-OGX-W.exe') | Select-Object FullName,Length
