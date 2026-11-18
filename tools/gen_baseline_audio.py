#!/usr/bin/env python3
"""
Generate deterministic 45-second stereo WAV baseline tracks and prepend clean ID3v2.3 tags
after Windows Media Foundation MP3 encoding for Stage A GTA IV Independence FM testing.
"""
import math
import os
import struct
import sys
import wave

SAMPLE_RATE = 44100
DURATION_SEC = 45.0
NUM_SAMPLES = int(SAMPLE_RATE * DURATION_SEC)


def make_id3v23_tag(title: str, artist: str, album: str, track_no: str) -> bytes:
    def make_text_frame(frame_id: bytes, text: str) -> bytes:
        # Encoding 0x01 = UTF-16 with BOM
        payload = b"\x01" + text.encode("utf-16")
        size = len(payload)
        return frame_id + struct.pack(">I", size) + b"\x00\x00" + payload

    frames = (
        make_text_frame(b"TIT2", title)
        + make_text_frame(b"TPE1", artist)
        + make_text_frame(b"TALB", album)
        + make_text_frame(b"TRCK", track_no)
    )
    # Synchsafe 28-bit integer
    total = len(frames)
    synchsafe = bytes([
        (total >> 21) & 0x7F,
        (total >> 14) & 0x7F,
        (total >> 7) & 0x7F,
        total & 0x7F,
    ])
    return b"ID3\x03\x00\x00" + synchsafe + frames


def synthesize_arpeggio_wav(out_path: str, notes_hz: list[float], marker_hz: float) -> None:
    note_step_sec = 0.5  # 2 notes per second
    buf = bytearray(NUM_SAMPLES * 4)  # 16-bit stereo

    for i in range(NUM_SAMPLES):
        t = i / SAMPLE_RATE
        step_idx = int(t / note_step_sec)
        t_in_note = t - step_idx * note_step_sec
        freq = notes_hz[step_idx % len(notes_hz)]

        # Smooth ADSR-like envelope per note to prevent clicks
        attack = min(1.0, t_in_note / 0.025)
        decay = math.exp(-3.2 * t_in_note)
        env = attack * (0.35 + 0.65 * decay)

        # Fundamental + warm 2nd/3rd harmonics (~ -14 dBFS RMS target)
        sig = (
            0.70 * math.sin(2.0 * math.pi * freq * t)
            + 0.20 * math.sin(2.0 * math.pi * (freq * 2.0) * t)
            + 0.10 * math.sin(2.0 * math.pi * (freq * 3.0) * t)
        ) * env * 0.28

        # Every 5.0 seconds, add a short 80ms marker chime
        t_mod5 = t % 5.0
        if t >= 0.1 and t_mod5 < 0.08:
            m_env = math.sin(math.pi * (t_mod5 / 0.08))
            sig += 0.22 * m_env * math.sin(2.0 * math.pi * marker_hz * t)

        # Global 0.3s fade-in / fade-out
        if t < 0.3:
            sig *= t / 0.3
        elif DURATION_SEC - t < 0.3:
            sig *= (DURATION_SEC - t) / 0.3

        # Gentle alternating stereo image (verifies L/R channel spatialization in vehicle)
        pan = 0.35 if (step_idx % 2 == 0) else 0.65
        left = max(-0.98, min(0.98, sig * (1.3 - 0.6 * pan)))
        right = max(-0.98, min(0.98, sig * (0.7 + 0.6 * pan)))

        l_i16 = int(left * 32767.0)
        r_i16 = int(right * 32767.0)
        struct.pack_into("<hh", buf, i * 4, l_i16, r_i16)

    with wave.open(out_path, "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(buf)


def prepend_id3(mp3_raw_path: str, mp3_final_path: str, title: str, artist: str, album: str, track_no: str) -> None:
    with open(mp3_raw_path, "rb") as f:
        raw = f.read()
    # Strip existing ID3 if MediaTranscoder wrote one
    if raw[:3] == b"ID3" and len(raw) >= 10:
        sz = ((raw[6] & 0x7F) << 21) | ((raw[7] & 0x7F) << 14) | ((raw[8] & 0x7F) << 7) | (raw[9] & 0x7F)
        raw = raw[10 + sz:]
    tag = make_id3v23_tag(title, artist, album, track_no)
    with open(mp3_final_path, "wb") as f:
        f.write(tag + raw)


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "wav"
    assets_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets"))
    os.makedirs(assets_dir, exist_ok=True)

    if mode == "wav":
        t1_wav = os.path.join(assets_dir, "IV_Probe_Target_45s.wav")
        t2_wav = os.path.join(assets_dir, "IV_Probe_Control_45s.wav")
        print(f"Synthesizing {t1_wav} ...")
        synthesize_arpeggio_wav(t1_wav, [220.00, 277.18, 329.63, 415.30], 880.00)
        print(f"Synthesizing {t2_wav} ...")
        synthesize_arpeggio_wav(t2_wav, [146.83, 174.61, 220.00, 261.63], 587.33)
        print("WAV generation complete.")
    elif mode == "tag":
        prepend_id3(
            os.path.join(assets_dir, "IV_Probe_Target_45s_raw.mp3"),
            os.path.join(assets_dir, "IV_Probe_Target_45s.mp3"),
            "IV Probe Target 45s",
            "StageA Baseline",
            "GTAIV WMVCore Probe",
            "1",
        )
        prepend_id3(
            os.path.join(assets_dir, "IV_Probe_Control_45s_raw.mp3"),
            os.path.join(assets_dir, "IV_Probe_Control_45s.mp3"),
            "IV Probe Control 45s",
            "StageA Baseline",
            "GTAIV WMVCore Probe",
            "2",
        )
        print("ID3v2.3 tagging complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
