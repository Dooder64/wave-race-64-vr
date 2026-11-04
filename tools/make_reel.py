#!/usr/bin/env python3
"""Cuts a session tools/record_session.ps1 recorded into a labelled
highlight reel.

    python tools/make_reel.py <session folder>            # lists its F2 marks
    python tools/make_reel.py <session folder> <reel.json> [out.mp4]

reel.json is a list of clips, each:

    {"mark": 3, "before": 4, "after": 8, "label": "Great Bay: Link diving"}
    {"at": "12:30", "length": 10, "label": "Seafloor"}       (minutes:seconds into the video)
    {"title": "Wave Race 64 VR", "length": 3}                  (a title card)

Each is cut from the video with its sound (the game's final mix, from the
WAV beside it, lined up by their start times), the left eye only when the
mirror showed both, scaled to 1920x1080, the label shown along the bottom,
faded in and out; then they are joined.
"""

import datetime
import json
import os
import re
import struct
import subprocess
import sys

FONT = 'C\\:/Windows/Fonts/segoeuib.ttf'
W, H, FPS = 1920, 1080, 60


def when(text):
    return datetime.datetime.fromisoformat(text.replace('Z', '+00:00'))


def fix_wav(path):
    """A WAV whose sizes were never written (the game ended before it closed
    the file): its RIFF and data sizes set from the file's length."""
    with open(path, 'r+b') as f:
        data = f.read(64)
        if data[:4] != b'RIFF':
            return
        at = data.find(b'data')
        if at < 0:
            return
        size = os.path.getsize(path)
        f.seek(4)
        f.write(struct.pack('<I', size - 8))
        f.seek(at + 4)
        f.write(struct.pack('<I', size - at - 8))


def label_file(folder, text, name):
    """The text for a drawtext, in a file of its own (no quoting to get wrong)."""
    path = os.path.join(folder, name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    return os.path.abspath(path).replace('\\', '/').replace(':', '\\:')


def session(folder):
    s = json.load(open(os.path.join(folder, 'session.json'), encoding='utf-8-sig'))
    start = when(s['video_start_utc'])
    marks = []
    for line in s.get('marks') or []:
        m = re.match(r'mark (\d+): (\S+ \S+) UTC (.*)', line)
        if m:
            t = datetime.datetime.strptime(m.group(2), '%Y-%m-%d %H:%M:%S').replace(tzinfo=datetime.timezone.utc)
            marks.append({'n': len(marks) + 1, 'seconds': (t - start).total_seconds(), 'where': m.group(3)})
    # The sound, one track from the video's start: the game writes a new
    # WAV each time its output rate changes (32000, then 26900 in a race);
    # each part's final mix (its channels 3 and 4), at 48 kHz, where it began.
    parts = s.get('audio') or []
    audio = None
    if parts:
        track = os.path.join(folder, 'sound.wav')
        if not os.path.exists(track):
            args = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error']
            chains = []
            for k, part in enumerate(parts):
                fix_wav(os.path.join(folder, part['file']))
                args += ['-i', os.path.join(folder, part['file'])]
                delay = max(0, int(round((when(part['start_utc']) - start).total_seconds() * 1000)))
                skip = max(0.0, (start - when(part['start_utc'])).total_seconds())
                chains.append(f"[{k}:a]pan=stereo|c0=c2|c1=c3,aresample=48000,atrim=start={skip:.3f},asetpts=PTS-STARTPTS,"
                              f"adelay={delay}|{delay}[a{k}]")
            mix = ''.join(f'[a{k}]' for k in range(len(parts)))
            args += ['-filter_complex', ';'.join(chains) + f';{mix}amix=inputs={len(parts)}:normalize=0[out]', '-map', '[out]', track]
            subprocess.run(args, check=True)
        audio = {'file': 'sound.wav'}
    return s, marks, audio, 0.0


def seconds(text):
    parts = [float(p) for p in str(text).split(':')]
    total = 0.0
    for p in parts:
        total = total * 60 + p
    return total


def run(args):
    print(' '.join(args[:6]), '...')
    subprocess.run(args, check=True)


def clip(folder, s, audio, offset, c, out):
    name = os.path.splitext(os.path.basename(out))[0] + '.txt'
    if 'title' in c:
        length = float(c.get('length', 3))
        text = label_file(folder, c['title'], name)
        run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', f'color=c=black:s={W}x{H}:r={FPS}:d={length}',
             '-f', 'lavfi', '-i', f'anullsrc=r=48000:cl=stereo', '-t', str(length),
             '-vf', f"drawtext=fontfile='{FONT}':textfile='{text}':fontsize=96:fontcolor=white:x=(w-text_w)/2:y=(h-text_h)/2,"
                    f"fade=in:st=0:d=0.5,fade=out:st={length - 0.5}:d=0.5",
             '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest', out])
        return
    start = c['seconds'] - float(c.get('before', 0))
    length = float(c.get('length', float(c.get('before', 0)) + float(c.get('after', 8))))
    start = max(0.0, start)
    eye = 'crop=iw/2-16:ih-16:8:8,' if s.get('both_eyes') else ''
    text = label_file(folder, c['label'], name) if c.get('label') else ''
    # (Commas in the fade's expression escaped: the filter graph splits on them.)
    label = (f",drawtext=fontfile='{FONT}':textfile='{text}':fontsize=52:fontcolor=white:x=70:y=h-140:"
             f"box=1:boxcolor=black@0.45:boxborderw=20:alpha='min(1\\,min(t/0.4\\,({length}-t)/0.4))'") if text else ''
    vf = (f"{eye}scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,fps={FPS}{label},"
          f"fade=in:st=0:d=0.3,fade=out:st={max(0.0, length - 0.3)}:d=0.3")
    args = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-ss', f'{start:.3f}', '-t', f'{length:.3f}',
            '-i', os.path.join(folder, s['video'])]
    if audio:
        args += ['-ss', f'{max(0.0, start - offset):.3f}', '-t', f'{length:.3f}', '-i', os.path.join(folder, audio['file']),
                 '-filter_complex', f"[0:v]{vf}[v];[1:a]afade=in:st=0:d=0.3,afade=out:st={max(0.0, length - 0.3)}:d=0.3[a]",
                 '-map', '[v]', '-map', '[a]']
    else:
        args += ['-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=stereo', '-vf', vf, '-shortest']
    args += ['-c:v', 'libx264', '-crf', '18', '-preset', 'medium', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', '-ar', '48000', out]
    run(args)


def main():
    folder = sys.argv[1]
    s, marks, audio, offset = session(folder)
    if len(sys.argv) < 3:
        print(f'{len(marks)} marks (sound starts {offset:+.2f} s from the video):')
        for m in marks:
            print(f"  mark {m['n']}: {int(m['seconds'] // 60)}:{m['seconds'] % 60:05.2f} into the video -- {m['where']}")
        return
    clips = json.load(open(sys.argv[2], encoding='utf-8'))
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(folder, 'reel.mp4')
    parts = []
    for k, c in enumerate(clips):
        if 'mark' in c:
            c['seconds'] = marks[int(c['mark']) - 1]['seconds']
        elif 'at' in c:
            c['seconds'] = seconds(c['at'])
            c.setdefault('before', 0)
        part = os.path.join(folder, f'clip_{k:02d}.mp4')
        clip(folder, s, audio, offset, c, part)
        parts.append(part)
    with open(os.path.join(folder, 'clips.txt'), 'w', encoding='utf-8') as f:
        for p in parts:
            f.write(f"file '{os.path.abspath(p)}'\n")
    run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', os.path.join(folder, 'clips.txt'),
         '-c', 'copy', out])
    print('reel:', out)


if __name__ == '__main__':
    main()
