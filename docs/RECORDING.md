# Recording a session and cutting a highlight reel

Two tools, for trailers and showcase videos.

## 1. Record

```
powershell -ExecutionPolicy Bypass -File tools\record_session.ps1 [-GreatBay] [-Flat]
```

Starts the game (in VR unless `-Flat`; with `-GreatBay`, Great Bay on Free
Roam's Dolphin Park), opens the Oculus mirror and records it, and saves the
game's own sound beside it. Press Start in the launcher and play; **press F2
at anything worth keeping** -- each press is a highlight mark. Close the game
to stop. Everything lands in `Desktop\Wave Race 64\recordings\<date time>`:
`video.mkv`, the sound (`audio-*.wav`) and `session.json` (start times and
the marks).

The recording is the mirror window's place on the screen (desktop
duplication: RT64 presents through D3D12, which window capture records
black), so **keep the mirror window uncovered**; it is pinned on top. Needs
ffmpeg on the PATH (`winget install Gyan.FFmpeg`).

## 2. Cut

```
python tools\make_reel.py "<session folder>"                 # lists the marks and their times
python tools\make_reel.py "<session folder>" reel.json [out.mp4]
```

`reel.json` lists the clips in order:

```json
[
  {"title": "Wave Race 64 VR", "length": 3},
  {"mark": 3, "before": 4, "after": 8, "label": "The seafloor"},
  {"at": "12:30", "length": 10, "label": "Riders with their own minds"},
  {"mark": 7, "before": 2, "after": 12, "label": "Great Bay Coast, from Majora's Mask"}
]
```

Each clip is cut with its sound (the game's final mix, the custom music
included), the mirror's left eye only, scaled to 1920x1080, its label along
the bottom, faded in and out, then all are joined into `reel.mp4`.
