# orta_package.ps1 -- builds the playtest package, then puts the normal play
# loop back as orta_loop.ps1. Run by the watcher in place of the play loop.
$orta    = 'G:\xboxrecomp\xboxrecomp-main\orta'
$toolkit = 'G:\xboxrecomp\xboxrecomp-main\xboxrecomp-main'
$pkg     = 'G:\xboxrecomp\Panzer Dragoon Orta PC'
$report  = Join-Path $orta 'package_report.txt'
Set-Location $orta
"started $(Get-Date -Format o)" | Set-Content $report
Write-Host '=== Building the release .exe ==='
$b = cmake --build build64 --config Release 2>&1
if ($LASTEXITCODE -ne 0) {
    'BUILD FAILED' | Add-Content $report
    $b | Select-String ' error ' | Select-Object -First 30 | ForEach-Object { $_.Line } | Add-Content $report
} else {
    Write-Host '=== Assembling the package ==='
    New-Item -ItemType Directory -Force -Path $pkg | Out-Null
    Copy-Item (Join-Path $orta 'build64\Release\orta_recomp.exe') (Join-Path $pkg 'Panzer Dragoon Orta.exe') -Force
    foreach ($f in 'engine.ini', 'settings.ini', 'README.txt') {
        Copy-Item (Join-Path $orta "package\$f") (Join-Path $pkg $f) -Force
    }
    Write-Host '=== Copying the game files (a few minutes) ==='
    robocopy (Join-Path $toolkit 'game_files') (Join-Path $pkg 'game_files') /E /XD UDATA TDATA /XF *.json /NFL /NDL /NJH /NP | Out-Null
    "robocopy exit $LASTEXITCODE" | Add-Content $report
    $files = Get-ChildItem $pkg -Recurse -File
    $gb = ($files | Measure-Object -Property Length -Sum).Sum / 1GB
    ('OK: {0} files, {1:N2} GB in {2}' -f $files.Count, $gb, $pkg) | Add-Content $report
    if (Test-Path (Join-Path $pkg 'game_files\default.xbe')) { 'default.xbe present' | Add-Content $report }
}
Copy-Item (Join-Path $orta 'orta_loop_play.ps1') (Join-Path $orta 'orta_loop.ps1') -Force
"finished $(Get-Date -Format o)" | Add-Content $report
