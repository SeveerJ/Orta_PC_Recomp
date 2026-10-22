# orta_watch.ps1 -- lets Claude start test runs by itself.
#
# Start once from Developer PowerShell for VS 2022 and leave the window open:
#   powershell -ExecutionPolicy Bypass -File .\orta_watch.ps1
#
# Every few seconds it looks for a file called go.flag in the orta folder.
# Claude creates that file when it wants a run; this script then deletes it,
# runs orta_loop.ps1 (translate / build / run as usual), saves everything the
# loop printed to loop_output.txt, and writes watch_status.txt so Claude can
# tell when the run has finished. Close the window (or Ctrl+C) to stop.

$orta   = 'G:\xboxrecomp\xboxrecomp-main\orta'
$go     = Join-Path $orta 'go.flag'
$status = Join-Path $orta 'watch_status.txt'
$out    = Join-Path $orta 'loop_output.txt'

Set-Location $orta
"idle $(Get-Date -Format o)" | Set-Content $status
Write-Host 'Watching for runs requested by Claude. Leave this window open.' -ForegroundColor Cyan

while ($true) {
    if (Test-Path $go) {
        Remove-Item $go -ErrorAction SilentlyContinue
        "running $(Get-Date -Format o)" | Set-Content $status
        Write-Host "`n$(Get-Date -Format T)  Claude started a run..." -ForegroundColor Yellow
        powershell -ExecutionPolicy Bypass -File (Join-Path $orta 'orta_loop.ps1') *> $out
        $code = $LASTEXITCODE
        "finished $(Get-Date -Format o) exit $code" | Set-Content $status
        Write-Host "$(Get-Date -Format T)  Run finished. Waiting for the next one." -ForegroundColor Green
    }
    Start-Sleep -Seconds 5
}
