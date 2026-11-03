param(
    [string]$BspRoot = ""
)

$ErrorActionPreference = 'Stop'

if (-not $BspRoot) {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
    $BspRoot = Join-Path $repo '.auto/plbuild/app_bsp/ps7_cortexa9_0'
}

$src = Join-Path $PSScriptRoot 'axidma_v9_1/xaxidma_g.c'
$dst = Join-Path $BspRoot 'libsrc/axidma_v9_1/src/xaxidma_g.c'

if (-not (Test-Path -LiteralPath $src)) { throw "patch source missing: $src" }
if (-not (Test-Path -LiteralPath $dst)) { throw "BSP file not found: $dst" }

Copy-Item -LiteralPath $src -Destination $dst -Force

$text = Get-Content -LiteralPath $dst -Raw
if ($text -notmatch '0x40410000U') { throw 'TX entry missing after copy' }
if ($text -notmatch '0x40400000U') { throw 'RX entry missing after copy' }
if ($text -notmatch '26\s*/\*\s*SgLengthWidth') { throw 'SG length 26 missing after copy' }

Write-Output "patched: $dst"
Write-Output ("sha256: " + (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash)
