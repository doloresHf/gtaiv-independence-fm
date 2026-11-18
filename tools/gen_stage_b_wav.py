#!/usr/bin/env python3
"""
Generate the fixed-format Stage B2 test asset:
  assets/IV_StageB_Target_48k.wav
Exact contract:
  - RIFF/WAVE canonical 44-byte header
  - WAVE_FORMAT_PCM (tag=1), 48000 Hz, 2 channels, 16-bit (blockAlign=4, byteRate=192000)
  - Exact PCM payload: 8,647,104 bytes (2,161,776 stereo frames = 45.037s)
  - Musical signature: 96 BPM warm electric-piano jazz 7th chord progression + crisp woodblock/bell ticks
    + smooth L/R stereo autopan (unmistakably distinct from Stage A arpeggio and B1 120 BPM synth bass)
  - Smooth 50ms cosine fade-in and 100ms (4,800-frame) cosine fade-out to exact 0 at final frame.
"""
import array
import math
import os
import wave

SAMPLE_RATE = 48000
TOTAL_FRAMES = 2161776  # 2161776 * 4 = 8,647,104 bytes exact


def generate_wav(out_path: str) -> None:
    two_pi = 2.0 * math.pi
    # Precompute 1-second lookup table doesn't capture irrational chords, so compute block-wise or use fast phase increments!
    # Even faster in pure Python / numpy if available; check numpy first for <0.5s generation!
    try:
        import numpy as np  # type: ignore

        f = np.arange(TOTAL_FRAMES, dtype=np.float64)
        t = f / SAMPLE_RATE

        # 96 BPM = 1.6 beats/sec -> 1 bar (4 beats) = 2.5s = 120,000 frames
        bar_frames = 120000
        chord_idx = (f.astype(np.int64) // bar_frames) % 4
        f_in_bar = f.astype(np.int64) % bar_frames

        # 4 lush jazz 7th chords: Fmaj7 -> Em7 -> Dm7 -> Cmaj7
        chords = np.array([
            [174.61, 220.00, 261.63, 329.63],  # Fmaj7
            [164.81, 196.00, 246.94, 293.66],  # Em7
            [146.83, 174.61, 220.00, 261.63],  # Dm7
            [130.81, 164.81, 196.00, 246.94],  # Cmaj7
        ], dtype=np.float64)

        f0 = chords[chord_idx, 0]
        f1 = chords[chord_idx, 1]
        f2 = chords[chord_idx, 2]
        f3 = chords[chord_idx, 3]

        # Smooth chord envelope per 2.5s bar (480-frame cosine attack & release to 0)
        env_bar = np.ones(TOTAL_FRAMES, dtype=np.float64)
        atk_mask = f_in_bar < 960
        rel_mask = f_in_bar >= (bar_frames - 960)
        env_bar[atk_mask] = 0.5 * (1.0 - np.cos(math.pi * f_in_bar[atk_mask] / 960.0))
        env_bar[rel_mask] = 0.5 * (1.0 - np.cos(math.pi * (bar_frames - f_in_bar[rel_mask]) / 960.0))
        env_bar *= (0.45 + 0.55 * np.exp(-1.4 * (f_in_bar / bar_frames)))

        epiano = env_bar * (
            0.32 * np.sin(two_pi * f0 * t)
            + 0.26 * np.sin(two_pi * f1 * t)
            + 0.22 * np.sin(two_pi * f2 * t)
            + 0.20 * np.sin(two_pi * f3 * t)
            + 0.08 * np.sin(two_pi * (f3 * 2.0) * t)
        )

        # Crisp high-frequency bell / woodblock click on every beat (30,000 frames = 0.625s) to test vehicle door LPF
        beat_frames = 30000
        f_in_beat = f.astype(np.int64) % beat_frames
        tick = np.zeros(TOTAL_FRAMES, dtype=np.float64)
        tick_mask = f_in_beat < 1440  # 30ms crisp transient
        x = f_in_beat[tick_mask] / 1440.0
        env_tick = np.sin(math.pi * x) * np.exp(-3.5 * x)
        t_tick = t[tick_mask]
        tick[tick_mask] = env_tick * (
            0.50 * np.sin(two_pi * 3840.0 * t_tick)
            + 0.35 * np.sin(two_pi * 6720.0 * t_tick)
            + 0.15 * np.sin(two_pi * 9250.0 * t_tick)
        )

        # Smooth stereo tremolo / autopan (0.8 Hz) so L and R weave noticeably across car speakers
        pan = 0.5 + 0.38 * np.sin(two_pi * 0.8 * t)
        left = epiano * (1.25 - 0.5 * pan) + tick * 0.85
        right = epiano * (0.75 + 0.5 * pan) + tick * 0.85

        # Global 50ms fade-in (2400 frames) and 100ms fade-out (4800 frames, exact 0 at last frame)
        global_env = np.ones(TOTAL_FRAMES, dtype=np.float64)
        fi = np.arange(2400, dtype=np.float64)
        global_env[:2400] = 0.5 * (1.0 - np.cos(math.pi * fi / 2400.0))
        fo = np.arange(4800, dtype=np.float64)
        # At fo[-1] == 4799, make sure last frame is exact 0.0
        global_env[-4800:] = 0.5 * (1.0 + np.cos(math.pi * (fo + 1.0) / 4800.0))
        global_env[-1] = 0.0

        left_i16 = np.clip(np.rint(left * global_env * 15500.0), -22000, 22000).astype(np.int16)
        right_i16 = np.clip(np.rint(right * global_env * 15500.0), -22000, 22000).astype(np.int16)

        interleaved = np.empty(TOTAL_FRAMES * 2, dtype=np.int16)
        interleaved[0::2] = left_i16
        interleaved[1::2] = right_i16
        raw_bytes = interleaved.tobytes()
    except ImportError:
        # Pure-Python fallback
        buf = array.array("h", [0] * (TOTAL_FRAMES * 2))
        chords_py = [
            (174.61, 220.00, 261.63, 329.63),
            (164.81, 196.00, 246.94, 293.66),
            (146.83, 174.61, 220.00, 261.63),
            (130.81, 164.81, 196.00, 246.94),
        ]
        for idx in range(TOTAL_FRAMES):
            t = idx / SAMPLE_RATE
            c = chords_py[(idx // 120000) % 4]
            fb = idx % 120000
            eb = 0.6
            if fb < 960:
                eb *= 0.5 * (1.0 - math.cos(math.pi * fb / 960.0))
            elif fb >= 119040:
                eb *= 0.5 * (1.0 - math.cos(math.pi * (120000 - fb) / 960.0))
            ep = eb * 0.25 * (
                math.sin(two_pi * c[0] * t)
                + math.sin(two_pi * c[1] * t)
                + math.sin(two_pi * c[2] * t)
                + math.sin(two_pi * c[3] * t)
            )
            ge = 1.0
            if idx < 2400:
                ge = 0.5 * (1.0 - math.cos(math.pi * idx / 2400.0))
            elif idx >= TOTAL_FRAMES - 4800:
                ge = 0.5 * (1.0 + math.cos(math.pi * (idx - (TOTAL_FRAMES - 4800) + 1.0) / 4800.0))
            val = int(max(-22000.0, min(22000.0, ep * ge * 16000.0)))
            buf[2 * idx] = val
            buf[2 * idx + 1] = val
        raw_bytes = buf.tobytes()

    assert len(raw_bytes) == 8647104, f"Unexpected PCM byte count: {len(raw_bytes)}"
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with wave.open(out_path, "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(raw_bytes)


if __name__ == "__main__":
    out = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "IV_StageB_Target_48k.wav"))
    generate_wav(out)
    sz = os.path.getsize(out)
    print(f"Generated {out} ({sz} bytes total, {sz - 44} PCM bytes)")
