#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE.
"""Builds the public presentation video on the CI (run from anywhere, after the host app is built):

  - every scene of scenes.py: a title card, or the real app (host build, 1080p) driven by a script against
    the mock engine's made-up catalogue (MOCK_DEMO: invented titles and drawn covers only);
  - an English voice-over: Kokoro-82M (Apache-2.0, kokoro-onnx); espeak-ng when it is not available;
  - the app's own navigation sounds under the voice;
  - subtitles in the app's nine languages: French burned into one copy, all of them as tracks of the other.

Inputs: ./e2a-app-host, .deps/glyphs_cjk.bin, .deps/kokoro/{kokoro-v1.0.onnx,voices-v1.0.bin} (optional),
DejaVu fonts (.deps/dejavu). Outputs in presentation/out/."""
import os
import re
import shutil
import subprocess
import sys
import textwrap
import time

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from scenes import ISO639_2, LANGS, SCENES  # noqa: E402

OUT, WORK = os.path.join(HERE, "out"), os.path.join(HERE, "work")
FPS, W, H = 30, 1920, 1080
LEAD, TAIL, FADE = 0.45, 0.9, 0.3      # voice starts LEAD s into a scene, the scene lasts TAIL s after it
VOICE = os.environ.get("E2A_VOICE", "af_heart")
FONT = os.path.join(ROOT, ".deps/dejavu/ttf/DejaVuSans.ttf")
FONT_B = os.path.join(ROOT, ".deps/dejavu/ttf/DejaVuSans-Bold.ttf")


def run(cmd, **kw):
    print("+", " ".join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    return subprocess.run(cmd, check=True, **kw)


def duration(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         check=True, capture_output=True, text=True).stdout
    return float(out.strip())


# ------------------------------------------------------------------ voice
_kokoro = None


def speak(text, path):
    """English speech into a wav; returns the engine used"""
    global _kokoro
    model = os.path.join(ROOT, ".deps/kokoro/kokoro-v1.0.onnx")
    voices = os.path.join(ROOT, ".deps/kokoro/voices-v1.0.bin")
    try:
        import soundfile as sf
        from kokoro_onnx import Kokoro
        if _kokoro is None:
            _kokoro = Kokoro(model, voices)
        samples, rate = _kokoro.create(text, voice=VOICE, speed=1.0, lang="en-us")
        sf.write(path, samples, rate)
        return f"Kokoro-82M ({VOICE})"
    except Exception as e:  # noqa: BLE001 - any failure: the robotic voice still makes a video
        print(f"kokoro unavailable ({e}), espeak-ng instead", flush=True)
        run(["espeak-ng", "-v", "en-us", "-s", "150", "-w", path, text])
        return "espeak-ng"


# ------------------------------------------------------------------ title cards
def card(lines, path):
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=(int(9 + 12 * t), int(13 + 2 * t), int(30 + 16 * t)))
    glow = Image.new("RGB", (W, H))
    g = ImageDraw.Draw(glow)
    g.ellipse([180, -200, 980, 600], fill=(40, 90, 210))
    g.ellipse([1150, 500, 1950, 1300], fill=(120, 50, 170))
    img = ImageChops.add(img, ImageChops.multiply(glow.filter(ImageFilter.GaussianBlur(160)), Image.new("RGB", (W, H), (150, 150, 150))))
    icon = Image.open(os.path.join(ROOT, "app/sce_sys/icon0.png")).convert("RGBA").crop((54, 60, 458, 458)).resize((300, 296), Image.LANCZOS)
    mask = Image.new("L", icon.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, icon.size[0] - 1, icon.size[1] - 1], radius=36, fill=255)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([W // 2 - 150, 222, W // 2 + 150, 520], radius=36, fill=(0, 0, 0, 150))
    img = Image.alpha_composite(img.convert("RGBA"), shadow.filter(ImageFilter.GaussianBlur(24)))
    img.paste(icon, (W // 2 - 150, 200), mask)
    d = ImageDraw.Draw(img)
    d.text((W // 2, 640), lines[0], font=ImageFont.truetype(FONT_B, 116), fill=(240, 244, 252), anchor="mm")
    d.text((W // 2, 750), lines[1], font=ImageFont.truetype(FONT, 46), fill=(170, 182, 205), anchor="mm")
    d.text((W // 2, 840), lines[2], font=ImageFont.truetype(FONT, 36), fill=(125, 178, 255), anchor="mm")
    img.convert("RGB").save(path)


# ------------------------------------------------------------------ app recordings
def nominal_ms(script):
    total = 0
    for tok in script.split():
        if tok.startswith("wait:"):
            total += int(tok[5:])
        elif not tok.startswith("shot:"):
            total += 20 if tok.startswith("!") else 266
    return total


def record(scene, index, need, version, assets):
    """the app on its own clock: video (1080p) and its sound, at least `need` seconds after the cut"""
    port = 18300 + index
    env = dict(os.environ, MOCK_DEMO="1", MOCK_STORAGE="several", **scene.get("mock", {}))
    mock = subprocess.Popen([sys.executable, os.path.join(ROOT, "app/host/mock_engine.py"), str(port), version], env=env)
    time.sleep(1.2)
    try:
        if os.path.exists("/tmp/e2a_app_settings.txt"):
            os.remove("/tmp/e2a_app_settings.txt")
        extra = max(0, int((scene["skip"] + need) * 1000) - nominal_ms(scene["script"]) + 700)
        vid, wav = os.path.join(WORK, f"{scene['id']}_raw.mp4"), os.path.join(WORK, f"{scene['id']}_raw.wav")
        shots = os.path.join(WORK, "shots", scene["id"])
        os.makedirs(shots, exist_ok=True)
        env2 = dict(os.environ, E2A_LANG=str(scene.get("lang", 1)), E2A_ENGINE_PORT=str(port), E2A_ASSETS=assets, E2A_SHOTS=shots,
                    E2A_SCRIPT=scene["script"] + f" wait:{extra}", E2A_VIDEO=vid, E2A_VIDEO_FULL="1", E2A_AUDIO_WAV=wav)
        run([os.path.join(ROOT, "e2a-app-host")], env=env2, timeout=300)
    finally:
        mock.kill()
        mock.wait()
    return vid, wav


# ------------------------------------------------------------------ one scene, encoded alike
def encode(video_args, sfx, voice, dur, out):
    fo = max(0.0, dur - FADE)
    flt = (f"[0:v]fps={FPS},scale={W}:{H},format=yuv420p,fade=t=in:st=0:d={FADE},fade=t=out:st={fo:.3f}:d={FADE}[v];"
           f"[1:a]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo,volume=0.32[s];"
           f"[2:a]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo,adelay={int(LEAD * 1000)}:all=1[n];"
           f"[s][n]amix=inputs=2:normalize=0:duration=longest,apad,atrim=0:{dur:.3f},"
           f"afade=t=in:d=0.2,afade=t=out:st={fo:.3f}:d={FADE}[a]")
    run(["ffmpeg", "-loglevel", "error", "-y", *video_args, *sfx, "-i", voice, "-filter_complex", flt, "-map", "[v]", "-map", "[a]",
         "-t", f"{dur:.3f}", "-r", str(FPS), "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
         "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2", out])


# ------------------------------------------------------------------ subtitles
def stamp(t):
    ms = int(round(t * 1000))
    return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}"


def split_cue(text, lang):
    """at most ~90 characters a cue: cut at a sentence or comma near the middle"""
    if len(text) <= 90:
        return [text]
    seps = r"(?<=[.!?。！？])\s*|(?<=[,，、:：;])\s*" if lang in ("zh",) else r"(?<=[.!?])\s+|(?<=[,:;—])\s+|(?<= )(?=—)"
    cuts = [m.end() for m in re.finditer(seps, text) if 0 < m.end() < len(text)]
    if lang == "th":   # Thai: spaces separate phrases
        cuts = [m.end() for m in re.finditer(r" ", text)]
    if not cuts:
        return [text]
    mid = min(cuts, key=lambda c: abs(c - len(text) / 2))
    return [p for p in (text[:mid].strip(), text[mid:].strip()) if p]


def wrap(text, lang):
    if lang in ("zh", "th"):
        return text
    return "\n".join(textwrap.wrap(text, 46))


def write_srt(lang, timings, path):
    n, out = 0, []
    for scene, start, voice_len, dur in timings:
        parts = split_cue(scene["text"][lang], lang)
        total = sum(len(p) for p in parts)
        t = start + LEAD
        end_all = min(start + LEAD + voice_len + 0.35, start + dur - 0.1)
        span = end_all - t
        for p in parts:
            d = span * len(p) / total
            n += 1
            out.append(f"{n}\n{stamp(t)} --> {stamp(t + d - 0.04)}\n{wrap(p, lang)}\n")
            t += d
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


# ------------------------------------------------------------------ main
def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    os.makedirs(os.path.join(OUT, "subtitles"), exist_ok=True)
    version = subprocess.run(["make", "-s", "version"], cwd=ROOT, capture_output=True, text=True).stdout.strip() or "dev"
    assets = os.path.join(WORK, "assets")
    os.makedirs(assets)
    with open(os.path.join(assets, "engine_version.txt"), "w") as f:
        f.write(version + "\n")
    glyphs = os.path.join(ROOT, ".deps/glyphs_cjk.bin")
    if os.path.exists(glyphs):
        shutil.copy(glyphs, assets)

    timings, clips, engines = [], [], set()
    for i, sc in enumerate(SCENES):
        voice = os.path.join(WORK, f"{sc['id']}_voice.wav")
        engines.add(speak(sc["say"], voice))
        vlen = duration(voice)
        dur = round(max(sc["min"], LEAD + vlen + TAIL) * FPS) / FPS
        clip = os.path.join(WORK, f"{i:02d}_{sc['id']}.mp4")
        silence = ["-f", "lavfi", "-t", f"{dur:.3f}", "-i", "anullsrc=r=48000:cl=stereo"]
        if sc["kind"] == "card":
            png = os.path.join(WORK, f"{sc['id']}.png")
            card([ln.format(version=version) for ln in sc["card"]], png)
            encode(["-loop", "1", "-framerate", str(FPS), "-t", f"{dur:.3f}", "-i", png], silence, voice, dur, clip)
        else:
            vid, wav = record(sc, i, dur, version, assets)
            sfx = ["-ss", f"{sc['skip']:.3f}", "-t", f"{dur:.3f}", "-i", wav] if os.path.exists(wav) else silence
            encode(["-ss", f"{sc['skip']:.3f}", "-t", f"{dur:.3f}", "-i", vid], sfx, voice, dur, clip)
        clips.append(clip)
        timings.append((sc, sum(t[3] for t in timings), vlen, dur))
        print(f"scene {sc['id']}: {dur:.2f} s (voice {vlen:.2f} s)", flush=True)

    lst = os.path.join(WORK, "clips.txt")
    with open(lst, "w") as f:
        f.writelines(f"file '{c}'\n" for c in clips)
    base = os.path.join(WORK, "presentation.mp4")
    run(["ffmpeg", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", "-movflags", "+faststart", base])

    srts = {}
    for lang in LANGS:
        srts[lang] = os.path.join(OUT, "subtitles", f"exfat2ampr-presentation.{lang}.srt")
        write_srt(lang, timings, srts[lang])

    # one file, English voice, the nine subtitle tracks (French first, shown by default)
    order = ["fr"] + [lg for lg in LANGS if lg != "fr"]
    args = ["ffmpeg", "-loglevel", "error", "-y", "-i", base]
    for lang in order:
        args += ["-i", srts[lang]]
    args += ["-map", "0:v", "-map", "0:a"]
    for k in range(len(order)):
        args += ["-map", f"{k + 1}:s"]
    args += ["-c:v", "copy", "-c:a", "copy", "-c:s", "mov_text", "-metadata:s:a:0", "language=eng"]
    for k, lang in enumerate(order):
        args += [f"-metadata:s:s:{k}", f"language={ISO639_2[lang]}"]
    args += ["-disposition:s:0", "default", "-movflags", "+faststart", os.path.join(OUT, "exfat2ampr-presentation.mp4")]
    run(args)

    # the same with the French subtitles drawn into the picture (any player, any site)
    shutil.copy(srts["fr"], os.path.join(WORK, "fr.srt"))
    style = "FontName=DejaVu Sans,FontSize=17,PrimaryColour=&H00FFFFFF,OutlineColour=&H00101010,BorderStyle=1,Outline=1.6,Shadow=0,MarginV=26"
    run(["ffmpeg", "-loglevel", "error", "-y", "-i", base, "-vf", f"subtitles=fr.srt:force_style='{style}'", "-c:v", "libx264",
         "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "copy", "-movflags", "+faststart",
         os.path.join(OUT, "exfat2ampr-presentation-sous-titres-fr.mp4")], cwd=WORK)

    # for review (work artifact): every 3 s on one sheet, and the middle of each scene at full size
    burned = os.path.join(OUT, "exfat2ampr-presentation-sous-titres-fr.mp4")
    run(["ffmpeg", "-loglevel", "error", "-y", "-i", burned, "-vf", "fps=1/3,scale=480:-1,tile=4x8", "-frames:v", "1",
         os.path.join(WORK, "review_sheet.png")])
    for k, (sc, start, vlen, dur) in enumerate(timings):
        for tag, at in (("a", start + min(dur - 0.4, LEAD + vlen * 0.3)), ("b", start + min(dur - 0.4, LEAD + vlen * 0.8))):
            run(["ffmpeg", "-loglevel", "error", "-y", "-ss", f"{at:.2f}", "-i", burned, "-frames:v", "1",
                 os.path.join(WORK, f"review_{k:02d}{tag}_{sc['id']}.png")])

    total = sum(t[3] for t in timings)
    with open(os.path.join(OUT, "LISEZMOI.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"exFAT2AMPR — vidéo de présentation (version {version}), {total:.0f} s, 1920x1080, 30 i/s\n\n"
                "exfat2ampr-presentation-sous-titres-fr.mp4 : voix anglaise, sous-titres français incrustés.\n"
                "exfat2ampr-presentation.mp4 : voix anglaise, 9 pistes de sous-titres à choisir dans le lecteur "
                "(français par défaut ; anglais, espagnol, italien, russe, portugais, chinois, thaï, vietnamien).\n"
                "subtitles/ : les mêmes sous-titres en .srt (YouTube, etc.).\n\n"
                f"Voix : synthèse vocale, {', '.join(sorted(engines))}. Kokoro-82M est sous licence Apache-2.0.\n"
                "Images : l'app réelle enregistrée sur le simulateur de la CI, avec un catalogue de jeux inventés "
                "(titres et jaquettes dessinés, aucun jeu réel).\n\nTexte dit (anglais) et sous-titres français :\n\n")
        for sc, start, vlen, dur in timings:
            f.write(f"[{stamp(start)[:8]}] {sc['text']['en']}\n           {sc['text']['fr']}\n\n")
    print(f"done: {total:.1f} s, voice: {', '.join(sorted(engines))}")


if __name__ == "__main__":
    main()
