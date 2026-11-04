# Records a play session for a trailer: the Oculus mirror (or, -Flat, the
# game's window) to video, the game's own sound to a WAV beside it, and the
# F2 marks made while playing, all in one folder that tools/make_reel.py
# cuts into a labelled highlight reel.
#
#   powershell -ExecutionPolicy Bypass -File tools\record_session.ps1 [-GreatBay] [-Flat]
#
# Video: ffmpeg's desktop duplication (ddagrab) of the window's place on the
# screen (RT64 presents through D3D12, which window capture records black),
# so the window is pinned on top and must stay uncovered. Sound: the game
# writes its own final mix (WR64_AUDIO_CAPTURE: four channels, the game's
# then the final mix with the custom music). Both start times go into
# session.json for lining them up. Stops when the game is closed.
param([switch]$GreatBay, [switch]$Flat, [string]$Out = '', [int]$Fps = 60, [string]$Build = 'build-windows', [string]$Rom = '')
$repo = Split-Path -Parent $PSScriptRoot
$cfg = "$env:LOCALAPPDATA\WaveRace64Recomp"
if ($Out -eq '') { $Out = Join-Path ([Environment]::GetFolderPath('Desktop')) ("Wave Race 64\recordings\" + (Get-Date -Format 'yyyy-MM-dd HHmm')) }
New-Item -ItemType Directory -Force $Out | Out-Null
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static IntPtr Find(string wanted) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var s = new StringBuilder(256); GetWindowText(h, s, 256);
            if (s.ToString().StartsWith(wanted)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
[Win]::SetProcessDPIAware() | Out-Null

# The game: VR unless -Flat, Great Bay if asked, its sound captured.
if (-not $Flat) { $env:WR64_VR = '1' } else { Remove-Item Env:WR64_VR -ErrorAction SilentlyContinue }
if ($GreatBay) { $env:WR64_GUEST = 'greatbay' } else { Remove-Item Env:WR64_GUEST -ErrorAction SilentlyContinue }
$env:WR64_AUDIO_CAPTURE = Join-Path $Out 'audio'
$marksBefore = if (Test-Path "$cfg\marks.txt") { (Get-Content "$cfg\marks.txt").Count } else { 0 }
# (-Rom skips the launcher, for a scripted test; otherwise press Start in it.)
if ($Rom) { $game = Start-Process -FilePath "$repo\$Build\WaveRace64Recomp.exe" -ArgumentList "`"$Rom`"" -WorkingDirectory "$repo\$Build" -PassThru }
else { $game = Start-Process -FilePath "$repo\$Build\WaveRace64Recomp.exe" -WorkingDirectory "$repo\$Build" -PassThru }

$title = 'Wave Race 64: Recompiled'
if (-not $Flat) {
    $mirror = Start-Process -FilePath 'C:\Program Files\Meta Horizon\Support\oculus-diagnostics\OculusMirror.exe' -ArgumentList '--RectilinearBothEyes' -PassThru
    $title = 'Oculus Mirror'
}
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 240 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $h = [Win]::Find($title) }
if ($h -eq [IntPtr]::Zero) { throw "no '$title' window" }
Start-Sleep -Seconds 2
[Win]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013) | Out-Null      # topmost, no move/size/activate
$r = New-Object Win+RECT; [Win]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object Win+POINT; [Win]::ClientToScreen($h, [ref]$p) | Out-Null
$w = ($r.Right - $r.Left) -band -2; $hh = ($r.Bottom - $r.Top) -band -2

# The recording: NVENC if the card has it, else x264.
$encoder = '-c:v libx264 -preset veryfast -crf 18'
$probe = & $ffmpeg -hide_banner -encoders 2>$null | Select-String 'h264_nvenc'
if ($probe) { $encoder = '-c:v h264_nvenc -preset p4 -cq 19' }
$video = Join-Path $Out 'video.mkv'
$args = "-hide_banner -loglevel warning -f lavfi -i ddagrab=output_idx=0:draw_mouse=0:framerate=${Fps}:offset_x=$($p.X):offset_y=$($p.Y):video_size=${w}x${hh},hwdownload,format=bgra $encoder -pix_fmt yuv420p `"$video`""
$psi = New-Object System.Diagnostics.ProcessStartInfo $ffmpeg, $args
$psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.CreateNoWindow = $true
$started = (Get-Date).ToUniversalTime()
$rec = [System.Diagnostics.Process]::Start($psi)
Write-Host "Recording '$title' ($w x $hh at $($p.X),$($p.Y)) to $Out. Close the game to stop."

$game.WaitForExit()
$rec.StandardInput.Write('q'); $rec.StandardInput.Flush()
if (-not $rec.WaitForExit(15000)) { $rec.Kill() }
if ($mirror -and -not $mirror.HasExited) { $mirror.Kill() }

# What make_reel.py needs: when the video started, when the sound did (the
# WAV's creation), and the marks made meanwhile.
$audio = Get-ChildItem $Out -Filter 'audio-*.wav' | Sort-Object Name
$marks = if (Test-Path "$cfg\marks.txt") { @(Get-Content "$cfg\marks.txt" | Select-Object -Skip $marksBefore) } else { @() }
[ordered]@{
    video = 'video.mkv'
    video_start_utc = $started.ToString('o')
    audio = @($audio | ForEach-Object { [ordered]@{ file = $_.Name; start_utc = $_.CreationTimeUtc.ToString('o') } })
    both_eyes = (-not $Flat)
    marks = $marks
    great_bay = [bool]$GreatBay
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $Out 'session.json')
Write-Host "Saved: $Out"
