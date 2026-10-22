# orta_loop.ps1
# One full round of the Orta recompilation loop:
#   1. collect missing function addresses from the last run's log
#   2. if there are new ones, re-run the pipeline (disasm -> func_id -> abi_analysis -> recomp)
#   3. rebuild the .exe (quick if nothing changed)
#   4. run it
#   5. print a short summary
#
# Run from Developer PowerShell for VS 2022, in the orta folder:
#   powershell -ExecutionPolicy Bypass -File .\orta_loop.ps1

$toolkit  = 'G:\xboxrecomp\xboxrecomp-main\xboxrecomp-main'
$orta     = 'G:\xboxrecomp\xboxrecomp-main\orta'
$seeds    = Join-Path $toolkit 'game_files\seeds_manual.json'
$log      = Join-Path $orta 'stderr.txt'
$sections = 'MMATRIX,D3D,D3DX,DSOUND,WMADEC,PSGSFD00,PSGSFD_I,PSGSFD_B,PSGSFD_P,XGRPH,XPP'

# The toolkit's watchdog is a one-shot timer: after this many seconds it
# dumps the guest stack and ENDS the program, hung or not.
$watchdogSeconds = 86400   # play session: effectively no limit
# Safety net in case even the watchdog does not end it.
$runLimitSeconds = 86400
# How many system calls get written to the log (the toolkit default is 200).
# Kept low: writing every call to the log on G: may be what slows the game down.
$kernelLogBudget = 200

function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }

function Fail($what) {
    Write-Host "`n$what failed. Send Claude the lines above." -ForegroundColor Red
    Pop-Location -ErrorAction SilentlyContinue
    exit 1
}

# Runs a tool, shows only the last few lines of its output, stops on failure.
function RunTool($name, [scriptblock]$cmd) {
    Step $name
    $out = & $cmd 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $out | Select-Object -Last 25 | ForEach-Object { "$_" }
        Fail $name
    }
    $out | Select-Object -Last 2 | ForEach-Object { "$_" }
}

# ---- 1. Collect missing functions ------------------------------------------
Step 'Collecting missing functions from the last run'

$known = @()
if (Test-Path $seeds) {
    $known = @((Get-Content $seeds -Raw | ConvertFrom-Json) | ForEach-Object { $_.start })
}

$found = @()
if (Test-Path $log) {
    $found = @(Select-String -Path $log -Pattern 'Failed to resolve VA (0x[0-9A-Fa-f]+)' |
        ForEach-Object { '0x' + $_.Matches[0].Groups[1].Value.Substring(2).ToUpper() } |
        Where-Object { $v = [Convert]::ToUInt32($_, 16); $v -ge 0x11000 -and $v -lt 0x30D000 })   # real code only; a null pointer call is not a function
    # Code the game reached that was never translated (a branch past a ret, a
    # jump-table case): "[STUB] code at 0x... skipped". Seeding it fixed the
    # stage-end cutscene, the boss and the sound resume, so do it automatically.
    $found += @(Select-String -Path $log -Pattern '\[STUB\] code at 0x([0-9A-Fa-f]{8}) skipped' |
        ForEach-Object { '0x' + $_.Matches[0].Groups[1].Value.ToUpper() } |
        Where-Object { $v = [Convert]::ToUInt32($_, 16); $v -ge 0x11000 -and $v -lt 0x30D000 })
}

$new = @($found | Sort-Object -Unique | Where-Object { $known -notcontains $_ })
$all = @(($known + $new) | Sort-Object -Unique)

Write-Host "Already known: $($known.Count)   New from last run: $($new.Count)   Total: $($all.Count)"

# Branch targets inside other functions: become alias entries, not splits.
$env:XBOXRECOMP_ALIAS_SEEDS = Join-Path $toolkit 'game_files\alias_seeds.json'

# Re-translate when the translator itself has changed since the C was made.
$genStamp = Join-Path $orta 'last_translation.stamp'
$translatorChanged = -not (Test-Path $genStamp)
if (Test-Path $genStamp) {
    $genTime = (Get-Item $genStamp).LastWriteTime
    foreach ($f in 'tools\disasm\functions.py', 'game_files\alias_seeds.json', 'tools\recomp\lifter.py', 'tools\recomp\translator.py', 'game_files\trace_functions.json', 'game_files\seeds_manual.json', '..\orta\src\recomp_manual.c') {
        if (-not (Test-Path (Join-Path $toolkit $f))) { continue }
        if ((Get-Item (Join-Path $toolkit $f)).LastWriteTime -gt $genTime) { $translatorChanged = $true }
    }
}
if ($translatorChanged) { Write-Host 'The translator has been updated since the last translation.' }

if ($new.Count -gt 0 -or $translatorChanged) {
    if ($new.Count -gt 0) { Write-Host ("New: " + ($new -join ', ')) }
    # "observed": every seed here is an address the game was seen to reach, so
    # the disassembler keeps it even where its sweep had decoded data as code.
    $json = '[' + (($all | ForEach-Object { '{"start": "' + $_ + '", "observed": true}' }) -join ', ') + ']'
    Set-Content -Path $seeds -Value $json -Encoding ASCII

    # ---- 2. Pipeline -------------------------------------------------------
    Push-Location $toolkit
    RunTool 'Disassembling (1-2 min)' {
        py -3 -m tools.disasm game_files/default.xbe --text-only --extra-sections $sections --seed-functions game_files/seeds_manual.json --force
    }
    RunTool 'Labelling functions' { py -3 -m tools.func_id game_files/default.xbe }
    RunTool 'Checking calling conventions' { py -3 -m tools.abi_analysis game_files/default.xbe }
    RunTool 'Translating to C (3-4 min)' {
        if (Test-Path 'game_files/trace_functions.json') {
            py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../orta/src/recomp/gen --exclude-manual ../orta/src/recomp_manual.c --trace-functions game_files/trace_functions.json
        } else {
            py -3 -m tools.recomp game_files/default.xbe --all --split 250 --gen-dir ../orta/src/recomp/gen --exclude-manual ../orta/src/recomp_manual.c
        }
    }
    Set-Content -Path $genStamp -Value (Get-Date -Format o)
    Pop-Location
} else {
    Write-Host 'Nothing new to translate, going straight to build and run.'
}

# ---- 3. Build ----------------------------------------------------------------
Push-Location $orta
Get-Process orta_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Step 'Building the .exe (quick if nothing changed)'
$buildOut = cmake --build build64 --config Release 2>&1
if ($LASTEXITCODE -ne 0) {
    $buildOut | Select-String ' error ' | Select-Object -First 30 | ForEach-Object { $_.Line }
    Fail 'Build'
}
Write-Host 'Build OK'

# ---- 4. Run ------------------------------------------------------------------
Step "Running the game (up to $watchdogSeconds s)"
$env:RECOMP_WATCHDOG_SECS     = "$watchdogSeconds"
$env:RECOMP_KERNEL_LOG_BUDGET = "$kernelLogBudget"
# Run the game's graphics commands and open a window on what it draws.
$env:RECOMP_PB_EXEC           = '1'
# Draw with the PC's graphics card (D3D11) instead of the CPU. If it cannot
# start (no D3D11 / shader compiler) the log says why and the CPU path is used.
# Set to '0' to go back to the software renderer.
$env:RECOMP_GPU = '1'
# ...and run the game's vertex programs on the graphics card too ('0' = on the CPU).
$env:RECOMP_GPU_VSH = '1'
# Sample where every thread spends its time; printed to stderr once a minute.
$env:RECOMP_PROFILE = '1'
# Unattended testing: a run that stops drawing for 60 s ends itself.
$env:RECOMP_STALL_EXIT_SECS = '180'
# Speed: ack D3D's flush at once so the game and the GPU executor overlap,
# and run the executor as soon as PUT moves.
$env:RECOMP_FAST_FLUSH = '1'
$env:RECOMP_PUT_SETTLE_US = '0'
# D3D's busy event (device+0x2564) is a KEVENT it builds itself; give it a real
# host event so BlockOnTime's wait can succeed instead of failing and retrying.
$env:RECOMP_TITLE_KEVENTS = '1'
# Vblank acknowledge: PCRTC_INTR behaves as write-1-to-clear (trapped page).
$env:RECOMP_PCRTC_TRAP = '1'
$env:RECOMP_PB_METHOD_TIME = '1'
$env:RECOMP_APU_STATS = '1'
$env:RECOMP_BUMP_TWOS = '1'   # ripple map bytes are two's complement (test 2 disproved offset-binary)
$env:RECOMP_BUMP_PROJ = '1'   # best river so far (test 3)
$env:RECOMP_BUMP_NOPERSP = '1'   # water test 14: screen-space reflection coords (test 8) + test 12
$env:RECOMP_PXSET = (Join-Path $orta 'pxset.txt')   # pixel-setup census (dev only)
$env:RECOMP_REFL_NODEPTH = '1'   # water test 12: reflection drawn without depth test
Remove-Item Env:RECOMP_REFL_CENTRE -ErrorAction SilentlyContinue   # tests 15/16 shifted the mirror wrongly
$env:RECOMP_GPU_SURFDUMP = 'G:\xboxrecomp\xboxrecomp-main\orta\texdump\surf'   # water build-up targets
# Fire the graphics chip's 60 Hz "vertical blank" interrupt. Without it, D3D's
# Present waits forever for a screen refresh that never comes.
$env:RECOMP_VBLANK = '1'
# Open a window showing the game's screen, and save a snapshot of it to
# frame.bmp in the orta folder every ~5 seconds so Claude can look too.
$env:RECOMP_FB_WINDOW = '1'
$env:RECOMP_FB_DUMP   = (Join-Path $orta 'frame.bmp')
$env:RECOMP_FB_WINDOW_DUMP_EVERY = '300'
# This run: save EVERY frame the game shows, from start-up, so Claude can see
# the whole opening sequence (logos, warning, title). Old snapshots removed first.
Get-ChildItem -Path $orta -Filter 'frame.bmp*.bmp' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
Remove-Item Env:RECOMP_FB_DUMP_FLIPS -ErrorAction SilentlyContinue
# Save each texture the game uses (first use) to orta\texdump, so Claude can
# check how textures decode. Cleared at the start of every run.
$texdump = Join-Path $orta 'texdump'
New-Item -ItemType Directory -Force -Path $texdump | Out-Null
Get-ChildItem -Path $texdump -Filter '*.bmp' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
$env:RECOMP_TEX_DUMP = (Join-Path $texdump 't')
# Log every change of the game's mode variable (9 = normal start-up path).
Remove-Item Env:RECOMP_WATCH -ErrorAction SilentlyContinue
Remove-Item Env:RECOMP_HWWATCH -ErrorAction SilentlyContinue
Remove-Item Env:RECOMP_HWWATCH_BIT -ErrorAction SilentlyContinue
# End the run after the pause (5000) / unpause (6000) test.
Remove-Item Env:RECOMP_EXIT_AT_FLIP -ErrorAction SilentlyContinue   # play session: no auto-stop
# The precise 60 Hz vblank clock slowed the opening movie to a crawl
# (0.5 fps); back to the old clock until that is understood.
$env:RECOMP_VBLANK_LEGACY = '0'
$env:RECOMP_TIMER_RES = '0'
$env:RECOMP_WAITDUMP = '10'
$env:RECOMP_VBLSTAT = '1'   # vblank / vblank-event counts every 10 s   # wait-time census every 10 s (movie investigation)
# Internal resolution: 3 = 1920x1440 for a 640x480 frame (1-4). The window shows
# it through a GPU swap chain; RECOMP_GPU_SHOT saves full-resolution frames.
$env:RECOMP_GPU_SCALE = '5'
Remove-Item Env:RECOMP_GPU_SHOT -ErrorAction SilentlyContinue   # = (Join-Path $orta 'hires') saves full-res shots
Get-ChildItem -Path $orta -Filter 'hires*.bmp' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
$env:RECOMP_APU_PHYS_CONTIG_FIRST = '1'
$env:RECOMP_CONTIG_LOG = '1'
$env:RECOMP_DSOUND_LISTCHECK = '0x0109B0E4'
# Guest memory to print when the watchdog fires: the game-input object
# (MGameInput8) whose method calls loop forever, its method table, and the
# buffer at 0x44848C that gets handed to it.
# Let freed memory actually go back to the heap (the toolkit's own option).
$env:RECOMP_HEAP_RECLAIM = '1'
# Report the sound chip's codec as ready and run the toolkit's emulated audio
# processor, so DirectSound's set-up (inside Orta's MGameSound8) can get past
# its first hardware check.
$env:RECOMP_AC97_READY = '1'
# For functions listed in trace_functions.json: print their arguments on entry.
$env:RECOMP_TRACE_ARGS  = '5'
Remove-Item Env:RECOMP_TRACE_DEREF -ErrorAction SilentlyContinue
# 0x342714 is Orta's "keep running" flag: 1 means init succeeded.
# Scripted controller: press Start at the title, A for New Game, A for
# Normal, then Start to skip the opening text. Numbers are frame counts.
# Set to '' to turn it off. Keyboard still works as normal.
Remove-Item Env:RECOMP_AUTOPAD -ErrorAction SilentlyContinue   # play session: no scripted presses
$env:RECOMP_APU_RAW = 'G:\xboxrecomp\xboxrecomp-main\orta\apu_out.raw'
$env:RECOMP_PEEK = '0x342718,0x342414,0x3427C4,0x342684,0x342444,0x3424A4,0x342758,0x485B2C,0x485B30,0x485B28,0x485B24,0x504C88,0x504C94'
$env:RECOMP_SEMA_DEVPTR = '0x2BD708'
# The kernel import slot DirectSound uses for ExAllocatePoolWithTag (0x30CA58)
# ends up zero. Trap writes to it and log who made them.
# DirectSound posts command 3 to the audio DSP at scratch+0x810 and waits for
# the DSP to clear it. The toolkit's DSP is a stub, so complete it at once.
Remove-Item Env:RECOMP_DSP_ACK -ErrorAction SilentlyContinue   # live word registered by the title hook
# Frame trace: 70 s into the run, log every draw of one whole frame.
$flag = Join-Path $orta 'ftrace.flag'
Remove-Item $flag -ErrorAction SilentlyContinue
Remove-Item Env:RECOMP_FRAME_TRACE -ErrorAction SilentlyContinue
$timer = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $orta 'build64\Release\orta_recomp.exe') `
    -WorkingDirectory $orta -NoNewWindow -PassThru `
    -RedirectStandardOutput (Join-Path $orta 'stdout.txt') `
    -RedirectStandardError $log
$handle = $p.Handle   # keeps the exit code readable after it ends
if (-not $p.WaitForExit($runLimitSeconds * 1000)) {
    Write-Host 'Still running after the safety limit, stopping it.' -ForegroundColor Yellow
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit()
}
$timer.Stop()
$secs = [int]$timer.Elapsed.TotalSeconds
$code = $p.ExitCode
Pop-Location

# ---- 5. Summary --------------------------------------------------------------
Step 'Summary of this run'
$how = switch ($code) {
    0       { 'the game exited normally' }
    3       { 'the watchdog ended it (time limit reached)' }
    default { 'it crashed or was stopped' }
}
Write-Host ("Ran for {0} s, exit code {1}: {2}" -f $secs, $code, $how)

if (Test-Path $log) {
    # Select-String -Path streams the file; piping a 40 MB log through
    # Get-Content took ten minutes after a long play session.
    $lines = Get-Content $log -Encoding UTF8 -Tail 4000
    $kcalls = @(Select-String -Path $log -Pattern '\[KERNEL\] #(\d+):' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    $misses = @(Select-String -Path $log -Pattern 'Failed to resolve VA (0x[0-9A-Fa-f]+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
    $icalls = (Select-String -Path $log -Pattern 'icalls so far: (\d+)' | Select-Object -Last 1)
    Write-Host ("System calls logged: {0}" -f ($(if ($kcalls.Count) { ($kcalls | Measure-Object -Maximum).Maximum } else { 0 })))
    if ($icalls) { Write-Host ("Indirect calls made: " + $icalls.Matches[0].Groups[1].Value) }
    Write-Host "Missing functions this run: $($misses.Count)"
    Write-Host "`n--- Important lines ---"
    $lines | Select-String 'ICALL\] Failed|scripted|is not code|called from|CRASH|WATCHDOG|Xbox VA of fault|  in sub_|  regs:|  peek:|WATCH|SEMA|PAD|GRAPH|ABI\]|STUB|APC\]| fps \(' |
        Select-Object -Last 30 | ForEach-Object { $_.Line }
    Write-Host "`n--- Last 25 log lines (stack dump hidden) ---"
    $lines | Where-Object { $_ -notmatch '^\s+GS ' } | Select-Object -Last 25
} else {
    Write-Host 'No log found.'
}
