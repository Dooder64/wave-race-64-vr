#!/usr/bin/env python3
"""Cut a highlight reel out of OBS recordings and write it as WebM.

    python make_reel.py markers RECORDING.mp4
        Lists the chapter markers OBS put in a recording (the "Add Chapter
        Marker" hotkey, Hybrid MP4 output), each with a suggested clip line.

    python make_reel.py reel CLIPS.txt OUT.webm [--crossfade 0.5] [--fast]
        Builds the reel. CLIPS.txt has one clip a line:

            RECORDING.mp4  START  END  [caption text]

        A file name with spaces goes in double quotes.
        START and END as seconds (83.5) or m:ss (1:23.5); '#' starts a
        comment. Each clip is scaled to 1920x1080 at 60 fps and the clips
        are joined with short crossfades, sound and picture. A caption, if
        given, shows in the lower left for the clip's first three seconds.
        The result is VP9 video and Opus sound; --fast encodes quicker at a
        little lower quality, for a preview.

        For a site with limits: --silent drops the sound, --height 540 and
        --fps 30 shrink the picture, and --max-bytes N encodes in two
        passes to fill but not pass N bytes. 4chan's /v/, for instance:
            --silent --max-bytes 4194304 --height 540 --fps 30
        (and at most 120 seconds).

Needs ffmpeg and ffprobe (winget: Gyan.FFmpeg).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    root = os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Packages')
    for base, _dirs, files in os.walk(root):
        if name + '.exe' in files:
            return os.path.join(base, name + '.exe')
    sys.exit(name + ' not found: install it with  winget install Gyan.FFmpeg')


FFMPEG, FFPROBE = tool('ffmpeg'), tool('ffprobe')
FONT = 'C\\:/Windows/Fonts/arialbd.ttf'


def seconds(text):
    parts = [float(p) for p in text.split(':')]
    total = 0.0
    for p in parts:
        total = total * 60 + p
    return total


def markers(recording):
    out = subprocess.run([FFPROBE, '-v', 'error', '-show_chapters', '-of', 'json', recording],
                         capture_output=True, text=True, check=True).stdout
    # OBS adds a 'Start' chapter at 0 and a 'Dummy' one at the end itself.
    chapters = [c for c in json.loads(out).get('chapters', [])
                if c.get('tags', {}).get('title') not in ('Start', 'Dummy')]
    if not chapters:
        print('No chapter markers in', recording, '(record as Hybrid MP4 and use the marker hotkey).')
        return
    for c in chapters:
        t = float(c['start_time'])
        start, end = max(0.0, t - 8.0), t + 4.0
        title = c.get('tags', {}).get('title', '')
        print('%s  %d:%04.1f  %d:%04.1f  %s' % (os.path.basename(recording), start // 60, start % 60,
                                               end // 60, end % 60, title))


def has_audio(recording):
    out = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'a', '-show_entries', 'stream=index',
                          '-of', 'csv=p=0', recording], capture_output=True, text=True).stdout
    return out.strip() != ''


def reel(clip_file, output, crossfade, fast, silent=False, max_bytes=None, height=1080, fps=60):
    base = os.path.dirname(os.path.abspath(clip_file))
    clips = []
    for line in open(clip_file, encoding='utf-8-sig'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('"'):  # a file name with spaces, in quotes
            name, _, rest = line[1:].partition('"')
            fields = [name] + rest.split(None, 2)
        else:
            fields = line.split(None, 3)
        if len(fields) < 3:
            sys.exit('bad clip line: ' + line)
        path = fields[0] if os.path.isabs(fields[0]) else os.path.join(base, fields[0])
        clips.append((path, seconds(fields[1]), seconds(fields[2]), fields[3] if len(fields) > 3 else ''))
    if not clips:
        sys.exit('no clips in ' + clip_file)

    work = tempfile.mkdtemp(prefix='wr64reel_')
    parts = []
    for i, (path, start, end, caption) in enumerate(clips):
        length = end - start
        if length <= crossfade * 2:
            sys.exit('clip %d is too short for the crossfade' % (i + 1))
        video = 'scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,fps=60,format=yuv420p'
        if caption:
            text = caption.replace('\\', '\\\\').replace(':', '\\:').replace("'", "’")
            video += (",drawtext=fontfile='%s':text='%s':x=60:y=h-120:fontsize=44:fontcolor=white:"
                      "box=1:boxcolor=black@0.45:boxborderw=14:enable='lt(t,3)'" % (FONT, text))
        part = os.path.join(work, 'part%02d.mkv' % i)
        cmd = [FFMPEG, '-y', '-v', 'error', '-ss', str(start), '-t', str(length), '-i', path]
        if not has_audio(path):
            cmd += ['-f', 'lavfi', '-t', str(length), '-i', 'anullsrc=r=48000:cl=stereo']
        cmd += ['-vf', video, '-af', 'aresample=48000', '-map', '0:v:0', '-map', '0:a:0' if has_audio(path) else '1:a:0',
                '-c:v', 'libx264', '-crf', '14', '-preset', 'fast', '-c:a', 'pcm_s16le', part]
        print('cutting clip %d of %d' % (i + 1, len(clips)))
        subprocess.run(cmd, check=True)
        parts.append((part, length))

    # Joined with crossfades: each fade starts that long before the end of
    # what has been joined so far.
    inputs, filters = [], []
    for part, _ in parts:
        inputs += ['-i', part]
    v, a, total = '[0:v]', '[0:a]', parts[0][1]
    for i in range(1, len(parts)):
        offset = total - crossfade
        filters.append('%s[%d:v]xfade=transition=fade:duration=%s:offset=%.3f[v%d]' % (v, i, crossfade, offset, i))
        if not silent:
            filters.append('%s[%d:a]acrossfade=d=%s[a%d]' % (a, i, crossfade, i))
        v, a = '[v%d]' % i, '[a%d]' % i
        total += parts[i][1] - crossfade
    graph = ';'.join(filters) if filters else '[0:v]null[v0]' + ('' if silent else ';[0:a]anull[a0]')
    if not filters:
        v, a = '[v0]', '[a0]'
    if height != 1080 or fps != 60:
        graph += ';%sscale=-2:%d:flags=lanczos,fps=%d[vout]' % (v, height, fps)
        v = '[vout]'
    quality = ['-deadline', 'realtime', '-cpu-used', '8'] if fast else ['-deadline', 'good', '-cpu-used', '2']
    sound = ['-an'] if silent else ['-map', a, '-c:a', 'libopus', '-b:a', '160k']
    head = [FFMPEG, '-y', '-v', 'error', '-stats'] + inputs + ['-filter_complex', graph, '-map', v,
                                                              '-c:v', 'libvpx-vp9', '-row-mt', '1'] + quality
    print('joining %d clips into %s (%.0f seconds)' % (len(parts), output, total))
    if max_bytes:
        # Two passes at the bitrate that fills the limit, less 4% for the
        # container and the encoder overshooting, and the sound's share.
        kbits = max_bytes * 8 / 1000 / total * 0.96 - (0 if silent else 160)
        log = os.path.join(work, 'pass')
        rate = ['-b:v', '%dk' % kbits, '-maxrate', '%dk' % (kbits * 1.5), '-bufsize', '%dk' % (kbits * 2),
                '-passlogfile', log]
        subprocess.run(head + rate + ['-pass', '1', '-an', '-f', 'webm', os.devnull], check=True)
        subprocess.run(head + rate + ['-pass', '2'] + sound + [output], check=True)
        if os.path.getsize(output) > max_bytes:
            sys.exit('%s came out at %d bytes, over the %d limit: try a lower --height'
                     % (output, os.path.getsize(output), max_bytes))
    else:
        subprocess.run(head + ['-b:v', '0', '-crf', '30'] + sound + [output], check=True)
    shutil.rmtree(work, ignore_errors=True)
    print('done:', output)


if __name__ == '__main__':
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == 'markers':
        for r in args[1:]:
            markers(r)
    elif len(args) >= 3 and args[0] == 'reel':
        def opt(name, default, kind=float):
            return kind(args[args.index(name) + 1]) if name in args else default
        reel(args[1], args[2], opt('--crossfade', 0.5), '--fast' in args, '--silent' in args,
             opt('--max-bytes', None, int), opt('--height', 1080, int), opt('--fps', 60, int))
    else:
        print(__doc__)
