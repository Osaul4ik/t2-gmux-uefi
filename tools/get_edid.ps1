# Dumps the EDID of every monitor Windows has seen to .\edid_*.bin (+ prints hex).
# Run in PowerShell. Pick the internal panel (usually "APP" manufacturer, e.g. APPAE..).
$i = 0
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY' -Recurse -ErrorAction SilentlyContinue |
  Where-Object { $_.PSChildName -eq 'Device Parameters' } |
  ForEach-Object {
    $e = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).EDID
    if ($e) {
      $i++
      $f = "edid_$i.bin"
      [IO.File]::WriteAllBytes((Join-Path (Get-Location) $f), [byte[]]$e)
      "{0}  ->  {1}  ({2} bytes)" -f $_.PSPath, $f, $e.Length
    }
  }
