# Build and flash muse-waveshare-s3-185b with WireGuard (sdkconfig.wg.local)
$ErrorActionPreference = 'Stop'

Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'idf_monitor|miniterm|esptool' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

Set-Location 'C:\esp\muse-gadget-sdk\esp32'

$buildDir = 'build-muse-waveshare-s3-185b'
$defaults = 'sdkconfig.defaults;devices\sdkconfig.muse;devices\sdkconfig.muse-waveshare-s3-185b'
$wgLocal = 'devices\sdkconfig.wg.local'
if (Test-Path $wgLocal) {
  $defaults = "$defaults;$wgLocal"
  Write-Host "Including $wgLocal"
} else {
  Write-Warning "Missing $wgLocal — WireGuard will stay off"
}

# Existing sdkconfig ignores new SDKCONFIG_DEFAULTS keys. Merge WG lines in.
$sc = Join-Path $buildDir 'sdkconfig'
if ((Test-Path $sc) -and (Test-Path $wgLocal)) {
  $cfg = Get-Content $sc -Raw
  Get-Content $wgLocal | ForEach-Object {
    $line = $_.Trim()
    if (-not $line -or $line.StartsWith('#')) { return }
    if ($line -match '^(CONFIG_[A-Z0-9_]+)=') {
      $key = $Matches[1]
      $escaped = [regex]::Escape($key) + '=.*'
      if ($cfg -match "(?m)^#? ?$escaped") {
        $cfg = [regex]::Replace($cfg, "(?m)^#? ?$escaped", $line)
      } else {
        $cfg = $cfg.TrimEnd() + "`n" + $line + "`n"
      }
    }
  }
  Set-Content -Path $sc -Value $cfg -NoNewline
  Write-Host "Merged WireGuard options into $sc"
}

Write-Host '=== Building ==='
idf.py -B $buildDir -D "SDKCONFIG_DEFAULTS=$defaults" build
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

$port = 'COM3'
Write-Host "=== Flashing $port (no NVS erase — keep pairing) ==="
idf.py -B $buildDir -p $port flash
if ($LASTEXITCODE -ne 0) { throw "flash failed: $LASTEXITCODE" }

Write-Host '=== DONE ==='
