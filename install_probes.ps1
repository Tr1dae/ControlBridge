# Installs the Phase 0 probes (see README.md). Run with both games closed:
#   powershell -ExecutionPolicy Bypass -File install_probes.ps1 [-Game control|resonant|both]
#
# Before copying anything it backs up both games' Steam saves to backups\<time>\. It never
# overwrites a file it didn't install itself, and records what it installed (with hashes) in
# install_manifest.json so uninstall_probes.ps1 removes exactly that.
param(
	[ValidateSet("control", "resonant", "both")] [string]$Game = "both",
	[string]$ControlDir = "E:\SteamLibrary\steamapps\common\Control",
	[string]$ResonantDir = "X:\SteamLibrary\steamapps\common\CONTROL Resonant",
	[string]$SteamDir = "C:\Program Files (x86)\Steam"
)
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$ManifestPath = Join-Path $Root "install_manifest.json"
$LogDir = Join-Path $Root "logs"

function Fail($msg) { Write-Host "INSTALL STOPPED: $msg" -ForegroundColor Red; exit 1 }
function Sha($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }

$running = Get-Process -Name "Control", "Control_DX12", "Control_DX11", "CONTROLResonant" -ErrorAction SilentlyContinue
if ($running) { Fail "close the game(s) first: $($running.Name -join ', ')" }

$manifest = @{ files = @(); dirs = @() }
if (Test-Path $ManifestPath) {
	$old = Get-Content -Raw $ManifestPath | ConvertFrom-Json
	$manifest.files = @($old.files)
	$manifest.dirs = @($old.dirs)
}
function Owned($dest) { $manifest.files | Where-Object { $_.path -eq $dest } | Select-Object -First 1 }

# Copies $src to $dest unless $dest is someone else's file.
function Place($src, $dest) {
	if (Test-Path -LiteralPath $dest) {
		$mine = Owned $dest
		if (-not $mine -or (Sha $dest) -ne $mine.sha256) {
			Fail "$dest already exists and was not installed by Control Bridge (another mod?). Move it away or decide what to do first."
		}
	}
	Copy-Item -LiteralPath $src -Destination $dest -Force
	$manifest.files = @($manifest.files | Where-Object { $_.path -ne $dest }) + @([pscustomobject]@{ path = $dest; sha256 = (Sha $dest) })
	Write-Host "  installed $dest"
}

function Ini($dest) {
	$tmp = Join-Path $env:TEMP "controlbridge.ini"
	@("[ControlBridge]", "LogDir=$LogDir", "FrameCsv=1", "FrameCsvEvery=1", "FrameCsvMinutes=20") | Set-Content -Encoding ASCII $tmp
	Place $tmp $dest
	Remove-Item $tmp
}

# ---- 1. back up saves ----------------------------------------------------------------------------
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $Root "backups\$stamp"
foreach ($app in @(@{ id = "870780"; name = "Control" }, @{ id = "3669870"; name = "CONTROL Resonant" })) {
	$found = Get-ChildItem -Path (Join-Path $SteamDir "userdata\*\$($app.id)\remote") -Directory -ErrorAction SilentlyContinue
	foreach ($r in $found) {
		$user = Split-Path (Split-Path (Split-Path $r.FullName -Parent) -Parent) -Leaf
		$dst = Join-Path $backup "saves\$($app.id)_$user"
		New-Item -ItemType Directory -Force $dst | Out-Null
		Copy-Item -Path (Join-Path $r.FullName "*") -Destination $dst -Recurse -Force
		Write-Host "  backed up $($app.name) saves ($($r.FullName)) -> $dst"
	}
	if (-not $found) { Write-Host "  no Steam saves found for $($app.name)" -ForegroundColor Yellow }
}
New-Item -ItemType Directory -Force $LogDir | Out-Null

# ---- 2. Control (2019): winmm.dll proxy next to Control_DX12.exe --------------------------------
if ($Game -in "control", "both") {
	Write-Host "Control (2019): $ControlDir"
	if (-not (Test-Path (Join-Path $ControlDir "Control_DX12.exe"))) { Fail "Control_DX12.exe not found in $ControlDir" }
	$bin = Join-Path $Root "bin\control\winmm.dll"
	if (-not (Test-Path $bin)) { Fail "build first (build.bat): $bin missing" }
	Place $bin (Join-Path $ControlDir "winmm.dll")
	Ini (Join-Path $ControlDir "controlbridge.ini")
}

# ---- 3. CONTROL Resonant: crloader mod folder ------------------------------------------------------
if ($Game -in "resonant", "both") {
	Write-Host "CONTROL Resonant: $ResonantDir"
	if (-not (Test-Path (Join-Path $ResonantDir "CONTROLResonant.exe"))) { Fail "CONTROLResonant.exe not found in $ResonantDir" }
	if (-not (Test-Path (Join-Path $ResonantDir "crmods"))) { Fail "crloader (f2g DLL Mod Loader) is not installed: no crmods folder" }
	$bin = Join-Path $Root "bin\resonant\cb_probe_resonant.dll"
	if (-not (Test-Path $bin)) { Fail "build first (build.bat): $bin missing" }
	$dir = Join-Path $ResonantDir "crmods\ControlBridgeProbe"
	if (-not (Test-Path $dir)) {
		New-Item -ItemType Directory $dir | Out-Null
		$manifest.dirs = @($manifest.dirs | Where-Object { $_ -ne $dir }) + @($dir)
	}
	Place $bin (Join-Path $dir "cb_probe_resonant.dll")
	Ini (Join-Path $dir "controlbridge.ini")
}

$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $ManifestPath
Write-Host "INSTALL OK. Logs will appear in $LogDir. Saves backed up to $backup" -ForegroundColor Green
