#!/usr/bin/env python3
"""
Generate and WMF-verify deterministic 20-minute (1,200.0 s) stereo MP3 carrier tracks
for Stage D Phase D2 of GTAIV-AppleMusic-Radio using in-process Windows Media Foundation
(MFCreateSinkWriterFromURL) and wmvcore.dll (WMCreateSyncReader).

Pure Python 3 standard library implementation (zero external package dependencies).
"""

from __future__ import annotations

import argparse
import array
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
import struct
import sys
import time
import uuid

SAMPLE_RATE = 48_000
CHANNELS = 2
BITS_PER_SAMPLE = 16
BLOCK_ALIGN = CHANNELS * (BITS_PER_SAMPLE // 8)  # 4 bytes/frame
PCM_AVG_BYTES_PER_SEC = SAMPLE_RATE * BLOCK_ALIGN  # 192,000 B/s
MP3_BITRATE_BPS = 192_000
MP3_AVG_BYTES_PER_SEC = MP3_BITRATE_BPS // 8  # 24,000 B/s

CHUNK_SEC = 10.0
CHUNK_FRAMES = int(SAMPLE_RATE * CHUNK_SEC)  # 480,000 frames
CHUNK_BYTES = CHUNK_FRAMES * BLOCK_ALIGN  # 1,920,000 bytes
CHUNK_HNS = 100_000_000  # 10.0 s in 100-ns units

NOTE_SEC = 0.5
NOTE_FRAMES = int(SAMPLE_RATE * NOTE_SEC)  # 24,000 frames
NOTE_FADE_FRAMES = int(SAMPLE_RATE * 0.015)  # 720 frames (15 ms)
GLOBAL_FADE_FRAMES = int(SAMPLE_RATE * 0.3)  # 14,400 frames (0.3 s)
MARKER_START_FRAME = int(SAMPLE_RATE * 5.0)  # 240,000 (5.00 s)
MARKER_FRAMES = int(SAMPLE_RATE * 0.08)  # 3,840 frames (80 ms: [5.00, 5.08))

COINIT_MULTITHREADED = 0x0
RPC_E_CHANGED_MODE = ctypes.c_long(0x80010106 - 0x100000000).value
MF_VERSION = (0x0002 << 16) | 0x0070
NS_E_NO_MORE_SAMPLES = ctypes.c_long(0xC00D0BCF - 0x100000000).value
WAVE_FORMAT_PCM = 1
WMT_TYPE_QWORD = 4

CARRIER_SPECS = [
    {
        "name": "IV_Radio_Stream_01.mp3",
        "title": "IV Radio Stream 01 (20m)",
        "artist": "StageD Carrier",
        "album": "GTAIV AppleMusic Stream",
        "track": "1",
        "notes_hz": [220.00, 277.18, 329.63, 415.30],
        "marker_hz": 880.00,
    },
    {
        "name": "IV_Radio_Stream_02.mp3",
        "title": "IV Radio Stream 02 (20m)",
        "artist": "StageD Carrier",
        "album": "GTAIV AppleMusic Stream",
        "track": "2",
        "notes_hz": [146.83, 174.61, 220.00, 261.63],
        "marker_hz": 587.33,
    },
    {
        "name": "IV_Radio_Stream_03.mp3",
        "title": "IV Radio Stream 03 (20m)",
        "artist": "StageD Carrier",
        "album": "GTAIV AppleMusic Stream",
        "track": "3",
        "notes_hz": [196.00, 246.94, 293.66, 369.99],
        "marker_hz": 783.99,
    },
]

EXPECTED_SHORT_CARRIERS = [
    {
        "name": "IV_Probe_Target_45s.mp3",
        "bytes": 1081322,
        "sha256": "d60521db0b70b315e5c3956e379eb79fc80a47347d4dc94fe182b8756dbee8b4",
    },
    {
        "name": "IV_Probe_Control_45s.mp3",
        "bytes": 1081324,
        "sha256": "d3e5a85b67f9d3476a7826b03eda544c4d62ca37922fa4c246da8f946d5d7da0",
    },
    {
        "name": "IV_Probe_Third_45s.mp3",
        "bytes": 1081320,
        "sha256": "04fce701bb40f810a1a8f90447a36de831989a65b1d696bf59fdd4b6d9da456d",
    },
]


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_uint8 * 8),
    ]

    @classmethod
    def from_str(cls, s: str) -> "GUID":
        u = uuid.UUID(s)
        return cls(u.fields[0], u.fields[1], u.fields[2], (ctypes.c_uint8 * 8)(*u.bytes[8:]))


class WM_MEDIA_TYPE(ctypes.Structure):
    _fields_ = [
        ("majortype", GUID),
        ("subtype", GUID),
        ("bFixedSizeSamples", wintypes.BOOL),
        ("bTemporalCompression", wintypes.BOOL),
        ("lSampleSize", wintypes.ULONG),
        ("formattype", GUID),
        ("pUnk", ctypes.c_void_p),
        ("cbFormat", wintypes.DWORD),
        ("pbFormat", ctypes.c_void_p),
    ]


class WAVEFORMATEX(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("wFormatTag", wintypes.WORD),
        ("nChannels", wintypes.WORD),
        ("nSamplesPerSec", wintypes.DWORD),
        ("nAvgBytesPerSec", wintypes.DWORD),
        ("nBlockAlign", wintypes.WORD),
        ("wBitsPerSample", wintypes.WORD),
        ("cbSize", wintypes.WORD),
    ]


MF_MT_MAJOR_TYPE = GUID.from_str("48eba18e-f8c9-4687-bf11-0a74c9f96a8f")
MF_MT_SUBTYPE = GUID.from_str("f7e34c9a-42e8-4714-b74b-cb29d72c35e5")
MF_MT_AUDIO_NUM_CHANNELS = GUID.from_str("37e48bf5-645e-4c5b-89de-ada9e29b696a")
MF_MT_AUDIO_SAMPLES_PER_SECOND = GUID.from_str("5faeeae7-0290-4c31-9e8a-c534f68d9dba")
MF_MT_AUDIO_AVG_BYTES_PER_SECOND = GUID.from_str("1aab75c8-cfef-451c-ab95-ac034b8e1731")
MF_MT_AUDIO_BLOCK_ALIGNMENT = GUID.from_str("322de230-9eeb-43bd-ab7a-ff412251541d")
MF_MT_AUDIO_BITS_PER_SAMPLE = GUID.from_str("f2deb57f-40fa-4764-aa33-ed4f2d1ff669")

MFMediaType_Audio = GUID.from_str("73647561-0000-0010-8000-00AA00389B71")
MFAudioFormat_PCM = GUID.from_str("00000001-0000-0010-8000-00AA00389B71")
MFAudioFormat_MP3 = GUID.from_str("00000055-0000-0010-8000-00AA00389B71")

IID_IWMHeaderInfo3 = GUID.from_str("15CC68E3-27CC-4ECD-B222-3F5D02D80BD5")

ole32 = ctypes.windll.ole32
mfplat = ctypes.windll.mfplat
mfreadwrite = ctypes.windll.mfreadwrite
wmvcore = ctypes.windll.wmvcore

ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, wintypes.DWORD]
ole32.CoInitializeEx.restype = ctypes.c_long
ole32.CoUninitialize.argtypes = []
ole32.CoUninitialize.restype = None

mfplat.MFStartup.argtypes = [wintypes.ULONG, wintypes.DWORD]
mfplat.MFStartup.restype = ctypes.c_long
mfplat.MFShutdown.argtypes = []
mfplat.MFShutdown.restype = ctypes.c_long
mfplat.MFCreateMediaType.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
mfplat.MFCreateMediaType.restype = ctypes.c_long
mfplat.MFCreateMemoryBuffer.argtypes = [wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p)]
mfplat.MFCreateMemoryBuffer.restype = ctypes.c_long
mfplat.MFCreateSample.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
mfplat.MFCreateSample.restype = ctypes.c_long

mfreadwrite.MFCreateSinkWriterFromURL.argtypes = [
    wintypes.LPCWSTR,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p),
]
mfreadwrite.MFCreateSinkWriterFromURL.restype = ctypes.c_long

wmvcore.WMCreateSyncReader.argtypes = [
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(ctypes.c_void_p),
]
wmvcore.WMCreateSyncReader.restype = ctypes.c_long


def vcall(obj_ptr: ctypes.c_void_p, vtbl_idx: int, restype, *argtypes_and_vals):
    vtbl = ctypes.cast(obj_ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    argtypes = [ctypes.c_void_p] + [a for a, _ in argtypes_and_vals]
    args = [obj_ptr] + [v for _, v in argtypes_and_vals]
    fn = ctypes.WINFUNCTYPE(restype, *argtypes)(vtbl[vtbl_idx])
    return fn(*args)


def com_release(obj_ptr: ctypes.c_void_p) -> None:
    if obj_ptr and obj_ptr.value:
        vcall(obj_ptr, 2, ctypes.c_ulong)
        obj_ptr.value = None


def check_hr(hr: int, context: str) -> None:
    if hr < 0:
        raise RuntimeError(f"{context} failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")


def mf_set_guid(mt: ctypes.c_void_p, key: GUID, guid: GUID) -> None:
    hr = vcall(
        mt,
        24,
        ctypes.c_long,
        (ctypes.POINTER(GUID), ctypes.byref(key)),
        (ctypes.POINTER(GUID), ctypes.byref(guid)),
    )
    check_hr(hr, "IMFAttributes::SetGUID")


def mf_set_u32(mt: ctypes.c_void_p, key: GUID, val: int) -> None:
    hr = vcall(
        mt,
        21,
        ctypes.c_long,
        (ctypes.POINTER(GUID), ctypes.byref(key)),
        (ctypes.c_uint32, val),
    )
    check_hr(hr, "IMFAttributes::SetUINT32")


def make_id3v23_tag(title: str, artist: str, album: str, track_no: str) -> bytes:
    """Build clean UTF-16 ID3v2.3 header matching tools/gen_baseline_audio.py."""

    def make_text_frame(frame_id: bytes, text: str) -> bytes:
        payload = b"\x01" + text.encode("utf-16")
        size = len(payload)
        return frame_id + struct.pack(">I", size) + b"\x00\x00" + payload

    frames = (
        make_text_frame(b"TIT2", title)
        + make_text_frame(b"TPE1", artist)
        + make_text_frame(b"TALB", album)
        + make_text_frame(b"TRCK", track_no)
    )
    total = len(frames)
    synchsafe = bytes(
        [
            (total >> 21) & 0x7F,
            (total >> 14) & 0x7F,
            (total >> 7) & 0x7F,
            total & 0x7F,
        ]
    )
    return b"ID3\x03\x00\x00" + synchsafe + frames


def strip_leading_id3(raw: bytes) -> bytes:
    """Strip any auto-generated leading ID3v2 tag(s) written by Media Foundation."""
    offset = 0
    while len(raw) - offset >= 10 and raw[offset : offset + 3] == b"ID3":
        flags = raw[offset + 5]
        sz = (
            ((raw[offset + 6] & 0x7F) << 21)
            | ((raw[offset + 7] & 0x7F) << 14)
            | ((raw[offset + 8] & 0x7F) << 7)
            | (raw[offset + 9] & 0x7F)
        )
        footer = 10 if (flags & 0x10) else 0
        offset += 10 + sz + footer
    return raw[offset:]


def build_note_envelope() -> list[float]:
    """
    Precompute the 0.5-second (24,000-frame) per-note amplitude envelope:
    - 15 ms (720-frame) smooth cosine fade-in from 0.0 at t_in_note = 0.0
    - exponential sustain/decay (0.35 + 0.65 * exp(-3.2 * t_in_note))
    - 15 ms (720-frame) smooth cosine fade-out reaching exact 0.0 at note end
    """
    env = [0.0] * NOTE_FRAMES
    fade_out_start = NOTE_FRAMES - NOTE_FADE_FRAMES
    fade_out_denom = float(NOTE_FADE_FRAMES - 1)
    for k in range(NOTE_FRAMES):
        t_in_note = k / float(SAMPLE_RATE)
        sustain = 0.35 + 0.65 * math.exp(-3.2 * t_in_note)
        if k < NOTE_FADE_FRAMES:
            fade = 0.5 * (1.0 - math.cos(math.pi * (k / float(NOTE_FADE_FRAMES))))
        elif k >= fade_out_start:
            rem = (NOTE_FRAMES - 1 - k) / fade_out_denom
            fade = 0.5 * (1.0 - math.cos(math.pi * rem))
        else:
            fade = 1.0
        env[k] = fade * sustain * 0.28
    return env


def apply_cosine_fade_in_inplace(pcm_buf: bytearray, fade_frames: int) -> None:
    denom = float(fade_frames)
    for i in range(fade_frames):
        g = 0.5 * (1.0 - math.cos(math.pi * (i / denom)))
        off = i * 4
        l_i16, r_i16 = struct.unpack_from("<hh", pcm_buf, off)
        struct.pack_into("<hh", pcm_buf, off, int(l_i16 * g), int(r_i16 * g))


def apply_cosine_fade_out_inplace(pcm_buf: bytearray, total_frames: int, fade_frames: int) -> None:
    start_frame = total_frames - fade_frames
    denom = float(fade_frames - 1)
    for j in range(fade_frames):
        rem = (fade_frames - 1 - j) / denom
        g = 0.5 * (1.0 - math.cos(math.pi * rem))
        off = (start_frame + j) * 4
        l_i16, r_i16 = struct.unpack_from("<hh", pcm_buf, off)
        struct.pack_into("<hh", pcm_buf, off, int(l_i16 * g), int(r_i16 * g))


def synthesize_track_chunks(
    notes_hz: list[float],
    marker_hz: float,
    note_env: list[float],
) -> tuple[bytes, bytes, bytes, bytes]:
    """
    Synthesize 100% phase-continuous 10.0-second (480,000-frame) stereo PCM16 buffers:
      (steady_chunk, first_chunk, last_chunk, single_chunk)
    Each 0.5s note starts and ends at exact 0 amplitude with cosine tapers, and the
    80 ms sine-windowed marker chime sits cleanly inside [5.00s, 5.08s) using local
    time t_m = t_chunk - 5.0, guaranteeing zero discontinuity at every 0.5s and 10.0s boundary.
    """
    if len(notes_hz) != 4:
        raise ValueError(f"Expected 4 arpeggio frequencies, got {len(notes_hz)}")

    note_pcms: list[bytearray] = []
    note2_chime_window_sig = [0.0] * MARKER_FRAMES

    for step_mod4, freq in enumerate(notes_hz):
        pan = 0.35 if (step_mod4 % 2 == 0) else 0.65
        l_gain = 1.3 - 0.6 * pan
        r_gain = 0.7 + 0.6 * pan
        w1 = 2.0 * math.pi * freq
        w2 = 2.0 * w1
        w3 = 3.0 * w1

        nbuf = bytearray(NOTE_FRAMES * 4)
        capture_sig = step_mod4 == (10 % len(notes_hz))  # step_idx == 10 sits at 5.00s..5.50s

        for k in range(NOTE_FRAMES):
            t_in_note = k / float(SAMPLE_RATE)
            sig = (
                0.70 * math.sin(w1 * t_in_note)
                + 0.20 * math.sin(w2 * t_in_note)
                + 0.10 * math.sin(w3 * t_in_note)
            ) * note_env[k]

            if capture_sig and k < MARKER_FRAMES:
                note2_chime_window_sig[k] = sig

            left = max(-0.98, min(0.98, sig * l_gain))
            right = max(-0.98, min(0.98, sig * r_gain))
            struct.pack_into("<hh", nbuf, k * 4, int(left * 32767.0), int(right * 32767.0))

        note_pcms.append(nbuf)

    # 4 notes = 2.0s; 20 notes = 10.0s = 5 * 2.0s blocks
    block_2s = note_pcms[0] + note_pcms[1] + note_pcms[2] + note_pcms[3]
    steady_buf = bytearray(block_2s * 5)

    # Overlay 80 ms sine-windowed marker chime at t_chunk in [5.00, 5.08) (step_idx == 10, pan = 0.35)
    marker_pan = 0.35
    marker_l_gain = 1.3 - 0.6 * marker_pan
    marker_r_gain = 0.7 + 0.6 * marker_pan
    w_marker = 2.0 * math.pi * marker_hz

    for m in range(MARKER_FRAMES):
        t_m = m / float(SAMPLE_RATE)
        m_env = math.sin(math.pi * (t_m / 0.08))
        chime = 0.22 * m_env * math.sin(w_marker * t_m)
        sig = note2_chime_window_sig[m] + chime
        left = max(-0.98, min(0.98, sig * marker_l_gain))
        right = max(-0.98, min(0.98, sig * marker_r_gain))
        struct.pack_into(
            "<hh",
            steady_buf,
            (MARKER_START_FRAME + m) * 4,
            int(left * 32767.0),
            int(right * 32767.0),
        )

    # Sanity-assert exact zero boundary samples at start and end of steady_buf
    if steady_buf[:4] != b"\x00\x00\x00\x00" or steady_buf[-4:] != b"\x00\x00\x00\x00":
        raise AssertionError("Steady 10s chunk boundary samples are not exact zero")

    first_buf = bytearray(steady_buf)
    apply_cosine_fade_in_inplace(first_buf, GLOBAL_FADE_FRAMES)

    last_buf = bytearray(steady_buf)
    apply_cosine_fade_out_inplace(last_buf, CHUNK_FRAMES, GLOBAL_FADE_FRAMES)

    single_buf = bytearray(first_buf)
    apply_cosine_fade_out_inplace(single_buf, CHUNK_FRAMES, GLOBAL_FADE_FRAMES)

    return bytes(steady_buf), bytes(first_buf), bytes(last_buf), bytes(single_buf)


def encode_chunks_to_mp3(
    out_raw_mp3_path: str,
    num_chunks: int,
    steady_chunk: bytes,
    first_chunk: bytes,
    last_chunk: bytes,
    single_chunk: bytes,
) -> None:
    if os.path.exists(out_raw_mp3_path):
        os.remove(out_raw_mp3_path)

    out_mt = ctypes.c_void_p()
    in_mt = ctypes.c_void_p()
    writer = ctypes.c_void_p()

    try:
        check_hr(mfplat.MFCreateMediaType(ctypes.byref(out_mt)), "MFCreateMediaType(out)")
        mf_set_guid(out_mt, MF_MT_MAJOR_TYPE, MFMediaType_Audio)
        mf_set_guid(out_mt, MF_MT_SUBTYPE, MFAudioFormat_MP3)
        mf_set_u32(out_mt, MF_MT_AUDIO_SAMPLES_PER_SECOND, SAMPLE_RATE)
        mf_set_u32(out_mt, MF_MT_AUDIO_NUM_CHANNELS, CHANNELS)
        mf_set_u32(out_mt, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, MP3_AVG_BYTES_PER_SEC)

        check_hr(mfplat.MFCreateMediaType(ctypes.byref(in_mt)), "MFCreateMediaType(in)")
        mf_set_guid(in_mt, MF_MT_MAJOR_TYPE, MFMediaType_Audio)
        mf_set_guid(in_mt, MF_MT_SUBTYPE, MFAudioFormat_PCM)
        mf_set_u32(in_mt, MF_MT_AUDIO_SAMPLES_PER_SECOND, SAMPLE_RATE)
        mf_set_u32(in_mt, MF_MT_AUDIO_NUM_CHANNELS, CHANNELS)
        mf_set_u32(in_mt, MF_MT_AUDIO_BITS_PER_SAMPLE, BITS_PER_SAMPLE)
        mf_set_u32(in_mt, MF_MT_AUDIO_BLOCK_ALIGNMENT, BLOCK_ALIGN)
        mf_set_u32(in_mt, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, PCM_AVG_BYTES_PER_SEC)

        check_hr(
            mfreadwrite.MFCreateSinkWriterFromURL(
                ctypes.c_wchar_p(out_raw_mp3_path),
                None,
                None,
                ctypes.byref(writer),
            ),
            "MFCreateSinkWriterFromURL",
        )

        stream_idx = wintypes.DWORD(0)
        check_hr(
            vcall(
                writer,
                3,
                ctypes.c_long,
                (ctypes.c_void_p, out_mt),
                (ctypes.POINTER(wintypes.DWORD), ctypes.byref(stream_idx)),
            ),
            "IMFSinkWriter::AddStream",
        )
        check_hr(
            vcall(
                writer,
                4,
                ctypes.c_long,
                (wintypes.DWORD, stream_idx.value),
                (ctypes.c_void_p, in_mt),
                (ctypes.c_void_p, None),
            ),
            "IMFSinkWriter::SetInputMediaType",
        )
        check_hr(vcall(writer, 5, ctypes.c_long), "IMFSinkWriter::BeginWriting")

        for idx in range(num_chunks):
            if num_chunks == 1:
                pcm_bytes = single_chunk
            elif idx == 0:
                pcm_bytes = first_chunk
            elif idx == num_chunks - 1:
                pcm_bytes = last_chunk
            else:
                pcm_bytes = steady_chunk

            buf = ctypes.c_void_p()
            sample = ctypes.c_void_p()
            try:
                check_hr(
                    mfplat.MFCreateMemoryBuffer(CHUNK_BYTES, ctypes.byref(buf)),
                    "MFCreateMemoryBuffer",
                )
                p_data = ctypes.c_void_p()
                max_len = wintypes.DWORD(0)
                cur_len = wintypes.DWORD(0)
                check_hr(
                    vcall(
                        buf,
                        3,
                        ctypes.c_long,
                        (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(p_data)),
                        (ctypes.POINTER(wintypes.DWORD), ctypes.byref(max_len)),
                        (ctypes.POINTER(wintypes.DWORD), ctypes.byref(cur_len)),
                    ),
                    "IMFMediaBuffer::Lock",
                )
                ctypes.memmove(p_data.value, pcm_bytes, CHUNK_BYTES)
                check_hr(vcall(buf, 4, ctypes.c_long), "IMFMediaBuffer::Unlock")
                check_hr(
                    vcall(buf, 6, ctypes.c_long, (wintypes.DWORD, CHUNK_BYTES)),
                    "IMFMediaBuffer::SetCurrentLength",
                )

                check_hr(mfplat.MFCreateSample(ctypes.byref(sample)), "MFCreateSample")
                hns_time = idx * CHUNK_HNS
                hns_dur = CHUNK_HNS
                check_hr(
                    vcall(sample, 42, ctypes.c_long, (ctypes.c_void_p, buf)),
                    "IMFSample::AddBuffer",
                )
                check_hr(
                    vcall(sample, 36, ctypes.c_long, (ctypes.c_int64, hns_time)),
                    "IMFSample::SetSampleTime",
                )
                check_hr(
                    vcall(sample, 38, ctypes.c_long, (ctypes.c_int64, hns_dur)),
                    "IMFSample::SetSampleDuration",
                )

                check_hr(
                    vcall(
                        writer,
                        6,
                        ctypes.c_long,
                        (wintypes.DWORD, stream_idx.value),
                        (ctypes.c_void_p, sample),
                    ),
                    "IMFSinkWriter::WriteSample",
                )
            finally:
                com_release(sample)
                com_release(buf)

        check_hr(vcall(writer, 11, ctypes.c_long), "IMFSinkWriter::Finalize")
    finally:
        com_release(writer)
        com_release(in_mt)
        com_release(out_mt)


def verify_mp3_headers(mp3_path: str) -> tuple[int, str, int, str]:
    """
    Verify ID3v2.3 header and first MPEG-1 Layer III 48 kHz 192 kbps audio frame header
    (0xFF 0xFB 0xB4 ...), returning (file_bytes, sha256_hex, id3_total_bytes, first_mpeg_hdr_hex).
    """
    with open(mp3_path, "rb") as f:
        data = f.read()

    file_bytes = len(data)
    sha256_hex = hashlib.sha256(data).hexdigest().lower()

    if file_bytes < 14 or data[:6] != b"ID3\x03\x00\x00":
        raise AssertionError(f"{mp3_path}: Missing expected ID3v2.3 header prefix")

    id3_body_sz = (
        ((data[6] & 0x7F) << 21)
        | ((data[7] & 0x7F) << 14)
        | ((data[8] & 0x7F) << 7)
        | (data[9] & 0x7F)
    )
    id3_total_bytes = 10 + id3_body_sz
    if file_bytes < id3_total_bytes + 4:
        raise AssertionError(f"{mp3_path}: Truncated file after ID3v2.3 tag")

    mpeg_hdr = data[id3_total_bytes : id3_total_bytes + 4]
    # 0xFF 0xFB = sync + MPEG-1 + Layer III + no CRC
    # 0xB4 (ignoring padding bit 0x02) = bitrate index 1011 (192 kbps) + sample rate 01 (48,000 Hz)
    if mpeg_hdr[0] != 0xFF or mpeg_hdr[1] != 0xFB or (mpeg_hdr[2] & 0xFC) != 0xB4:
        raise AssertionError(
            f"{mp3_path}: Unexpected first MPEG frame header 0x{mpeg_hdr.hex()} "
            "(expected 0xfffbb4.. for MPEG-1 Layer III 48 kHz 192 kbps)"
        )

    return file_bytes, sha256_hex, id3_total_bytes, mpeg_hdr.hex()


def pcm_buffer_max_abs(p_buf_val: int, byte_len: int) -> int:
    raw_pcm = ctypes.string_at(p_buf_val, byte_len)
    samples = array.array("h")
    samples.frombytes(raw_pcm)
    if not samples:
        return 0
    lo = min(samples)
    hi = max(samples)
    return max(-lo, hi)


def verify_wmf_sync_reader(mp3_path: str, duration_target_sec: float, silent: bool = False) -> dict:
    """
    Open the final MP3 via wmvcore.dll WMCreateSyncReader and verify format negotiation,
    IWMHeaderInfo3 Duration, and GTA IV Complete Scan head + tail-20s seek/drain behavior.
    """
    reader = ctypes.c_void_p()
    props = ctypes.c_void_p()
    hi3 = ctypes.c_void_p()
    opened = False

    try:
        check_hr(wmvcore.WMCreateSyncReader(None, 0, ctypes.byref(reader)), "WMCreateSyncReader")
        check_hr(
            vcall(reader, 3, ctypes.c_long, (ctypes.c_wchar_p, os.path.abspath(mp3_path))),
            "IWMSyncReader::Open",
        )
        opened = True

        # SetReadStreamSamples(1, FALSE) -> uncompressed PCM output
        check_hr(
            vcall(reader, 10, ctypes.c_long, (wintypes.WORD, 1), (wintypes.BOOL, 0)),
            "IWMSyncReader::SetReadStreamSamples(1, FALSE)",
        )

        # GetOutputProps(0, &props)
        check_hr(
            vcall(
                reader,
                15,
                ctypes.c_long,
                (wintypes.DWORD, 0),
                (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(props)),
            ),
            "IWMSyncReader::GetOutputProps(0)",
        )
        cb = wintypes.DWORD(0)
        check_hr(
            vcall(
                props,
                4,
                ctypes.c_long,
                (ctypes.c_void_p, None),
                (ctypes.POINTER(wintypes.DWORD), ctypes.byref(cb)),
            ),
            "IWMMediaProps::GetMediaType(size)",
        )
        if cb.value < ctypes.sizeof(WM_MEDIA_TYPE):
            raise AssertionError(f"WM_MEDIA_TYPE buffer size too small: {cb.value}")

        mt_buf = (ctypes.c_ubyte * cb.value)()
        check_hr(
            vcall(
                props,
                4,
                ctypes.c_long,
                (ctypes.c_void_p, ctypes.cast(mt_buf, ctypes.c_void_p)),
                (ctypes.POINTER(wintypes.DWORD), ctypes.byref(cb)),
            ),
            "IWMMediaProps::GetMediaType(data)",
        )
        pmt = ctypes.cast(mt_buf, ctypes.POINTER(WM_MEDIA_TYPE)).contents
        if not pmt.pbFormat or pmt.cbFormat < ctypes.sizeof(WAVEFORMATEX):
            raise AssertionError("WM_MEDIA_TYPE missing WAVEFORMATEX payload")
        wfx = ctypes.cast(pmt.pbFormat, ctypes.POINTER(WAVEFORMATEX)).contents

        if (
            wfx.wFormatTag != WAVE_FORMAT_PCM
            or wfx.nSamplesPerSec != SAMPLE_RATE
            or wfx.nChannels != CHANNELS
            or wfx.wBitsPerSample != BITS_PER_SAMPLE
            or wfx.nBlockAlign != BLOCK_ALIGN
            or wfx.nAvgBytesPerSec != PCM_AVG_BYTES_PER_SEC
        ):
            raise AssertionError(
                f"Unexpected WMF output PCM format: tag={wfx.wFormatTag}, "
                f"rate={wfx.nSamplesPerSec}, ch={wfx.nChannels}, bits={wfx.wBitsPerSample}, "
                f"align={wfx.nBlockAlign}, avg={wfx.nAvgBytesPerSec}"
            )
        com_release(props)

        # Query IWMHeaderInfo3::GetAttributeByName("Duration")
        check_hr(
            vcall(
                reader,
                0,
                ctypes.c_long,
                (ctypes.POINTER(GUID), ctypes.byref(IID_IWMHeaderInfo3)),
                (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(hi3)),
            ),
            "QueryInterface(IWMHeaderInfo3)",
        )
        stream_num = wintypes.WORD(0)
        attr_type = ctypes.c_int(0)
        dur_qword = ctypes.c_uint64(0)
        cb_len = wintypes.WORD(8)
        check_hr(
            vcall(
                hi3,
                5,
                ctypes.c_long,
                (ctypes.POINTER(wintypes.WORD), ctypes.byref(stream_num)),
                (ctypes.c_wchar_p, "Duration"),
                (ctypes.POINTER(ctypes.c_int), ctypes.byref(attr_type)),
                (ctypes.c_void_p, ctypes.byref(dur_qword)),
                (ctypes.POINTER(wintypes.WORD), ctypes.byref(cb_len)),
            ),
            "IWMHeaderInfo3::GetAttributeByName(Duration)",
        )
        if attr_type.value != WMT_TYPE_QWORD or cb_len.value != 8:
            raise AssertionError(
                f"Unexpected Duration attribute type={attr_type.value}, len={cb_len.value}"
            )
        com_release(hi3)

        duration_100ns = int(dur_qword.value)
        duration_sec = duration_100ns / 10_000_000.0
        if not (duration_target_sec <= duration_sec <= duration_target_sec + 0.25):
            raise AssertionError(
                f"Duration out of bounds: got {duration_sec:.6f}s "
                f"(expected [{duration_target_sec:.6f}s, {duration_target_sec + 0.25:.6f}s])"
            )

        # 1) Head check: SetRange(0, 0), pull first 250 samples (> 5.3s)
        check_hr(
            vcall(reader, 5, ctypes.c_long, (ctypes.c_uint64, 0), (ctypes.c_int64, 0)),
            "IWMSyncReader::SetRange(0, 0)",
        )
        head_samples_target = 250
        head_pcm_bytes = 0
        head_max_abs = 0

        for s_idx in range(head_samples_target):
            nss = ctypes.c_void_p()
            pts = ctypes.c_uint64(0)
            dur = ctypes.c_uint64(0)
            flags = wintypes.DWORD(0)
            out_num = wintypes.DWORD(0)
            s_num = wintypes.WORD(0)
            try:
                hr = vcall(
                    reader,
                    7,
                    ctypes.c_long,
                    (wintypes.WORD, 1),
                    (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(nss)),
                    (ctypes.POINTER(ctypes.c_uint64), ctypes.byref(pts)),
                    (ctypes.POINTER(ctypes.c_uint64), ctypes.byref(dur)),
                    (ctypes.POINTER(wintypes.DWORD), ctypes.byref(flags)),
                    (ctypes.POINTER(wintypes.DWORD), ctypes.byref(out_num)),
                    (ctypes.POINTER(wintypes.WORD), ctypes.byref(s_num)),
                )
                check_hr(hr, f"Head GetNextSample[{s_idx}]")
                p_buf = ctypes.c_void_p()
                b_len = wintypes.DWORD(0)
                check_hr(
                    vcall(
                        nss,
                        7,
                        ctypes.c_long,
                        (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(p_buf)),
                        (ctypes.POINTER(wintypes.DWORD), ctypes.byref(b_len)),
                    ),
                    "INSSBuffer::GetBufferAndLength(head)",
                )
                if b_len.value == 0 or (b_len.value % BLOCK_ALIGN) != 0:
                    raise AssertionError(
                        f"Head sample {s_idx} invalid byte length {b_len.value}"
                    )
                head_pcm_bytes += int(b_len.value)
                peak = pcm_buffer_max_abs(p_buf.value, int(b_len.value))
                if peak > head_max_abs:
                    head_max_abs = peak
            finally:
                com_release(nss)

        if silent and head_max_abs != 0:
            raise AssertionError(f"Expected silent head PCM, peak={head_max_abs}")
        if not silent and head_max_abs <= 1000:
            raise AssertionError(f"Head audio peak too low: {head_max_abs} <= 1000")

        # 2) Tail-20s check: SetRange(duration_100ns - 200_000_000, 0), drain to NS_E_NO_MORE_SAMPLES
        if duration_target_sec >= 20.0:
            tail_window_100ns = 200_000_000
            min_tail_samples = 800
        else:
            tail_window_100ns = max(10_000_000, duration_100ns // 2)
            min_tail_samples = max(100, int(800 * (tail_window_100ns / 200_000_000.0)))

        tail_seek_start_100ns = duration_100ns - tail_window_100ns
        check_hr(
            vcall(
                reader,
                5,
                ctypes.c_long,
                (ctypes.c_uint64, tail_seek_start_100ns),
                (ctypes.c_int64, 0),
            ),
            "IWMSyncReader::SetRange(tail)",
        )

        tail_samples = 0
        tail_pcm_bytes = 0
        tail_max_abs = 0
        while True:
            nss = ctypes.c_void_p()
            pts = ctypes.c_uint64(0)
            dur = ctypes.c_uint64(0)
            flags = wintypes.DWORD(0)
            out_num = wintypes.DWORD(0)
            s_num = wintypes.WORD(0)
            try:
                hr = vcall(
                    reader,
                    7,
                    ctypes.c_long,
                    (wintypes.WORD, 1),
                    (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(nss)),
                    (ctypes.POINTER(ctypes.c_uint64), ctypes.byref(pts)),
                    (ctypes.POINTER(ctypes.c_uint64), ctypes.byref(dur)),
                    (ctypes.POINTER(wintypes.DWORD), ctypes.byref(flags)),
                    (ctypes.POINTER(wintypes.DWORD), ctypes.byref(out_num)),
                    (ctypes.POINTER(wintypes.WORD), ctypes.byref(s_num)),
                )
                if hr == NS_E_NO_MORE_SAMPLES:
                    break
                check_hr(hr, f"Tail GetNextSample[{tail_samples}]")

                p_buf = ctypes.c_void_p()
                b_len = wintypes.DWORD(0)
                check_hr(
                    vcall(
                        nss,
                        7,
                        ctypes.c_long,
                        (ctypes.POINTER(ctypes.c_void_p), ctypes.byref(p_buf)),
                        (ctypes.POINTER(wintypes.DWORD), ctypes.byref(b_len)),
                    ),
                    "INSSBuffer::GetBufferAndLength(tail)",
                )
                if b_len.value == 0 or (b_len.value % BLOCK_ALIGN) != 0:
                    raise AssertionError(
                        f"Tail sample {tail_samples} invalid byte length {b_len.value}"
                    )
                tail_samples += 1
                tail_pcm_bytes += int(b_len.value)
                peak = pcm_buffer_max_abs(p_buf.value, int(b_len.value))
                if peak > tail_max_abs:
                    tail_max_abs = peak
            finally:
                com_release(nss)

        if tail_samples <= min_tail_samples:
            raise AssertionError(
                f"Tail sample count too low: {tail_samples} <= {min_tail_samples}"
            )
        if silent and tail_max_abs != 0:
            raise AssertionError(f"Expected silent tail PCM, peak={tail_max_abs}")
        if not silent and tail_max_abs <= 1000:
            raise AssertionError(f"Tail audio peak too low: {tail_max_abs} <= 1000")

        return {
            "format_tag": int(wfx.wFormatTag),
            "sample_rate_hz": int(wfx.nSamplesPerSec),
            "channels": int(wfx.nChannels),
            "bits_per_sample": int(wfx.wBitsPerSample),
            "block_align": int(wfx.nBlockAlign),
            "avg_bytes_per_sec": int(wfx.nAvgBytesPerSec),
            "duration_100ns": duration_100ns,
            "duration_sec": round(duration_sec, 6),
            "head_samples": head_samples_target,
            "head_pcm_bytes": head_pcm_bytes,
            "head_max_abs": head_max_abs,
            "tail_seek_start_100ns": tail_seek_start_100ns,
            "tail_samples": tail_samples,
            "tail_pcm_bytes": tail_pcm_bytes,
            "tail_max_abs": tail_max_abs,
            "tail_eof_hr": "0xC00D0BCF",
        }
    finally:
        com_release(hi3)
        com_release(props)
        if opened and reader and reader.value:
            vcall(reader, 4, ctypes.c_long)
        com_release(reader)


def run_determinism_selfcheck(out_dir: str, note_env: list[float]) -> str:
    spec = CARRIER_SPECS[0]
    steady_c, first_c, last_c, single_c = synthesize_track_chunks(
        spec["notes_hz"], spec["marker_hz"], note_env
    )
    tmp_a = os.path.join(out_dir, "_determ_selfcheck_a.mp3")
    tmp_b = os.path.join(out_dir, "_determ_selfcheck_b.mp3")
    try:
        encode_chunks_to_mp3(tmp_a, 1, steady_c, first_c, last_c, single_c)
        encode_chunks_to_mp3(tmp_b, 1, steady_c, first_c, last_c, single_c)
        with open(tmp_a, "rb") as fa, open(tmp_b, "rb") as fb:
            bytes_a = fa.read()
            bytes_b = fb.read()
        sha_a = hashlib.sha256(bytes_a).hexdigest().lower()
        sha_b = hashlib.sha256(bytes_b).hexdigest().lower()
        if len(bytes_a) == 0 or sha_a != sha_b:
            raise AssertionError(
                f"MF MP3 encoder determinism self-check failed: sha_a={sha_a} vs sha_b={sha_b}"
            )
        return sha_a
    finally:
        for p in (tmp_a, tmp_b):
            if os.path.exists(p):
                os.remove(p)


def collect_short_carriers_reference(repo_root: str) -> list[dict]:
    assets_dir = os.path.join(repo_root, "assets")
    refs: list[dict] = []
    for expected in EXPECTED_SHORT_CARRIERS:
        p = os.path.join(assets_dir, expected["name"])
        if not os.path.isfile(p):
            raise FileNotFoundError(f"Missing frozen short carrier asset: {p}")
        with open(p, "rb") as f:
            data = f.read()
        actual_bytes = len(data)
        actual_sha = hashlib.sha256(data).hexdigest().lower()
        if actual_bytes != expected["bytes"] or actual_sha != expected["sha256"]:
            raise AssertionError(
                f"Frozen short carrier asset mismatch for {p}: "
                f"bytes={actual_bytes} (expected {expected['bytes']}), "
                f"sha256={actual_sha} (expected {expected['sha256']})"
            )
        refs.append(
            {
                "name": expected["name"],
                "bytes": actual_bytes,
                "sha256": actual_sha,
            }
        )
    return refs


def build_planned_ini(target_files_csv: str) -> dict:
    ini_lines = [
        "[StageB]",
        "Mode=ipc",
        "",
        "[StageC]",
        r"MappingName=Local\GTAIV_StageC_IpcRing_v1",
        f"TargetFiles={target_files_csv}",
        "SteadyLeadMs=3500",
        "HeartbeatTimeoutMs=500",
        "FadeMs=10",
        "AllowHotReattach=1",
    ]
    content = "\r\n".join(ini_lines) + "\r\n"
    raw_ascii = content.encode("ascii")
    return {
        "encoding": "ASCII",
        "line_ending": "CRLF",
        "bytes": len(raw_ascii),
        "sha256": hashlib.sha256(raw_ascii).hexdigest().lower(),
        "content": content,
    }


def parse_args() -> argparse.Namespace:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    default_out = os.path.join(repo_root, "build", "carriers")

    parser = argparse.ArgumentParser(
        description="Generate and WMF-verify Stage D 20-minute MP3 carriers."
    )
    parser.add_argument(
        "--out-dir",
        default=default_out,
        help="Output directory for generated MP3 carriers and MANIFEST.json (default: <repo>/build/carriers).",
    )
    parser.add_argument(
        "--duration-sec",
        type=float,
        default=1200.0,
        help="Target carrier duration in seconds; must be a positive multiple of 10.0 (default: 1200.0).",
    )
    parser.add_argument(
        "--skip-determinism-check",
        action="store_true",
        help="Skip the 10.0s MFCreateSinkWriterFromURL determinism self-check.",
    )
    parser.add_argument("--silent", action="store_true", help="Generate silent player-release carriers.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    out_dir = os.path.abspath(args.out_dir)
    duration_sec = float(args.duration_sec)

    if not math.isfinite(duration_sec) or duration_sec <= 0.0:
        raise ValueError(f"--duration-sec must be positive, got {duration_sec}")
    chunks_float = duration_sec / CHUNK_SEC
    num_chunks = int(round(chunks_float))
    if num_chunks < 1 or abs(chunks_float - num_chunks) > 1e-9:
        raise ValueError(
            f"--duration-sec must be an exact positive multiple of {CHUNK_SEC:.1f}s, got {duration_sec}"
        )

    os.makedirs(out_dir, exist_ok=True)
    short_carriers_ref = collect_short_carriers_reference(repo_root)
    note_env = build_note_envelope()

    hr_co = ole32.CoInitializeEx(None, COINIT_MULTITHREADED)
    if hr_co < 0 and hr_co != RPC_E_CHANGED_MODE:
        check_hr(hr_co, "CoInitializeEx")
    co_initialized = hr_co >= 0

    mf_started = False
    try:
        check_hr(mfplat.MFStartup(MF_VERSION, 0), "MFStartup")
        mf_started = True

        determ_sha256 = None
        if not args.skip_determinism_check:
            t0 = time.perf_counter()
            determ_sha256 = run_determinism_selfcheck(out_dir, note_env)
            dt_ms = (time.perf_counter() - t0) * 1000.0
            print(
                f"[SELF-CHECK] 10.0s MF MP3 determinism verified in {dt_ms:.1f} ms "
                f"(sha256={determ_sha256})"
            )

        carrier_entries: list[dict] = []
        for spec in CARRIER_SPECS:
            name = spec["name"]
            stem = os.path.splitext(name)[0]
            raw_path = os.path.join(out_dir, f"{stem}_raw.mp3")
            final_path = os.path.join(out_dir, name)

            t_start = time.perf_counter()
            if args.silent:
                steady_c = first_c = last_c = single_c = bytes(CHUNK_BYTES)
            else:
                steady_c, first_c, last_c, single_c = synthesize_track_chunks(
                    spec["notes_hz"], spec["marker_hz"], note_env
                )
            encode_chunks_to_mp3(raw_path, num_chunks, steady_c, first_c, last_c, single_c)

            try:
                with open(raw_path, "rb") as f_raw:
                    raw_mp3 = f_raw.read()
                mpeg_stream = strip_leading_id3(raw_mp3)
                id3_tag = make_id3v23_tag(
                    spec["title"], spec["artist"], spec["album"], spec["track"]
                )
                with open(final_path, "wb") as f_out:
                    f_out.write(id3_tag)
                    f_out.write(mpeg_stream)
            finally:
                if os.path.exists(raw_path):
                    os.remove(raw_path)

            encode_elapsed = time.perf_counter() - t_start

            file_bytes, sha256_hex, id3_bytes, first_hdr_hex = verify_mp3_headers(final_path)
            wmf_metrics = verify_wmf_sync_reader(final_path, duration_sec, silent=args.silent)

            print(
                f"[VERIFIED] {name} | {file_bytes} B | sha256={sha256_hex} | "
                f"dur={wmf_metrics['duration_sec']:.6f}s | "
                f"head_peak={wmf_metrics['head_max_abs']} | "
                f"tail_samples={wmf_metrics['tail_samples']} (peak={wmf_metrics['tail_max_abs']}) | "
                f"{encode_elapsed:.2f}s"
            )

            carrier_entries.append(
                {
                    "name": name,
                    "title": spec["title"],
                    "artist": spec["artist"],
                    "album": spec["album"],
                    "track": spec["track"],
                    "notes_hz": [] if args.silent else spec["notes_hz"],
                    "marker_hz": None if args.silent else spec["marker_hz"],
                    "bytes": file_bytes,
                    "sha256": sha256_hex,
                    "id3v23_bytes": id3_bytes,
                    "first_mpeg_header_hex": first_hdr_hex,
                    "encode_elapsed_sec": round(encode_elapsed, 3),
                    "wmf_verification": wmf_metrics,
                }
            )

        target_files_csv = ",".join(c["name"] for c in carrier_entries)
        planned_ini = build_planned_ini(target_files_csv)

        manifest = {
            "generated_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "duration_target_sec": duration_sec,
            "chunks_per_track": num_chunks,
            "chunk_duration_sec": CHUNK_SEC,
            "encoder_format": {
                "sample_rate_hz": SAMPLE_RATE,
                "channels": CHANNELS,
                "pcm_bits_per_sample": BITS_PER_SAMPLE,
                "mp3_bitrate_kbps": MP3_BITRATE_BPS // 1000,
                "mp3_avg_bytes_per_sec": MP3_AVG_BYTES_PER_SEC,
                "determinism_selfcheck_10s_sha256": determ_sha256,
            },
            "target_files_csv": target_files_csv,
            "planned_ini": planned_ini,
            "managed_short_carriers": short_carriers_ref,
            "carriers": carrier_entries,
        }

        manifest_path = os.path.join(out_dir, "MANIFEST.json")
        manifest_tmp = os.path.join(out_dir, "MANIFEST.json.tmp")
        with open(manifest_tmp, "w", encoding="utf-8", newline="\n") as fm:
            json.dump(manifest, fm, indent=2)
            fm.write("\n")
        os.replace(manifest_tmp, manifest_path)
        print(f"[MANIFEST] Wrote {manifest_path}")
        return 0
    finally:
        if mf_started:
            mfplat.MFShutdown()
        if co_initialized:
            ole32.CoUninitialize()


if __name__ == "__main__":
    raise SystemExit(main())
