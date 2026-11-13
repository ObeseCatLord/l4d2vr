# Developer helper (not shipped): wait until Left 4 Dead 2 exits, then copy a
# freshly built d3d9.dll into the game folder.
# Usage: powershell -File copy_d3d9_after_exit.ps1 [-Source <d3d9.dll>] [-GameDir <L4D2 folder>] [-Log <file>]
param(
    [string]$Source = 'D:\l4d2vr\Release\d3d9.dll',
    [string]$GameDir = 'E:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2',
    [string]$Log = (Join-Path $env:TEMP 'copy_d3d9_after_exit.log')
)
$dst = Join-Path $GameDir 'd3d9.dll'
if (-not (Test-Path -LiteralPath $Source)) { throw "Source DLL not found: $Source (pass -Source)" }
if (-not (Test-Path -LiteralPath $GameDir)) { throw "Game folder not found: $GameDir (pass -GameDir)" }
"waiting for left4dead2 to exit: $(Get-Date -Format o)" | Out-File -FilePath $Log -Encoding UTF8
while (Get-Process left4dead2 -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 2 }
try {
    Copy-Item -LiteralPath $Source -Destination $dst -Force -ErrorAction Stop
    "copied $Source to $dst at $(Get-Date -Format o)" | Out-File -FilePath $Log -Append -Encoding UTF8
    Get-Item -LiteralPath $dst | Select-Object FullName,Length,LastWriteTime | Out-String | Out-File -FilePath $Log -Append -Encoding UTF8
} catch {
    "copy failed at $(Get-Date -Format o): $($_.Exception.Message)" | Out-File -FilePath $Log -Append -Encoding UTF8
    Write-Error "copy failed: $($_.Exception.Message) (log: $Log)"
    exit 1
}
