# Removes exactly what install_probes.ps1 installed (install_manifest.json). Run with both games closed:
#   powershell -ExecutionPolicy Bypass -File uninstall_probes.ps1
# A file whose hash no longer matches what was installed is left in place and reported.
$ErrorActionPreference = "Stop"
$ManifestPath = Join-Path $PSScriptRoot "install_manifest.json"
if (-not (Test-Path $ManifestPath)) { Write-Host "Nothing to uninstall (no install_manifest.json)."; exit 0 }
$running = Get-Process -Name "Control", "Control_DX12", "Control_DX11", "CONTROLResonant" -ErrorAction SilentlyContinue
if ($running) { Write-Host "UNINSTALL STOPPED: close the game(s) first: $($running.Name -join ', ')" -ForegroundColor Red; exit 1 }

$m = Get-Content -Raw $ManifestPath | ConvertFrom-Json
$kept = @()
foreach ($f in @($m.files)) {
	if (-not (Test-Path -LiteralPath $f.path)) { Write-Host "  already gone: $($f.path)"; continue }
	if ((Get-FileHash -Algorithm SHA256 -LiteralPath $f.path).Hash -ne $f.sha256) {
		Write-Host "  KEPT (changed since install, not ours anymore?): $($f.path)" -ForegroundColor Yellow
		$kept += $f
		continue
	}
	Remove-Item -LiteralPath $f.path
	Write-Host "  removed $($f.path)"
}
foreach ($d in @($m.dirs)) {
	if ((Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) {
		Remove-Item -LiteralPath $d
		Write-Host "  removed folder $d"
	} elseif (Test-Path -LiteralPath $d) {
		Write-Host "  folder not empty, kept: $d" -ForegroundColor Yellow
	}
}
if ($kept) {
	@{ files = $kept; dirs = @($m.dirs | Where-Object { Test-Path -LiteralPath $_ }) } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $ManifestPath
} else {
	Remove-Item $ManifestPath
}
Write-Host "UNINSTALL DONE. Save backups in backups\ were left untouched." -ForegroundColor Green
