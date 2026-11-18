#!/usr/bin/env python3
"""
Analyze IVRadioProbe.jsonl from Stage A, Stage B, and Stage C testing and summarize:
- Process attach/detach & Stage B / Stage C startup config
- Reader lifecycles (Sync vs Async, paths opened, total samples/bytes)
- Stage B per-reader decisions, seek alignment, sample guards, and replaced counters
- Queried COM interfaces (wrapped vs unwrapped)
- Negotiated audio WAVEFORMATEX parameters
- Sample pacing & pre-read buffer lead (media PTS delta vs wall-clock delta)
- SetRange / Seek / EOF behavior
- Stage B Contract Audit
- Stage C IPC Stream & Dual-Slot Audit (when Stage C events or mode="ipc" are present)
"""
import collections
import json
import os
import statistics
import sys


def _is_target_filename(path: str | None) -> bool:
    if not path or path == "<unopened>":
        return False
    leaf = path.replace("/", "\\").rsplit("\\", 1)[-1]
    return leaf.lower() == "iv_probe_target_45s.mp3"


def _ev_int(ev: dict, *keys: str, default: int = 0) -> int:
    for k in keys:
        v = ev.get(k)
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return int(v)
    return default


def _ev_str(ev: dict, *keys: str, default: str = "") -> str:
    for k in keys:
        v = ev.get(k)
        if v is not None and str(v) != "":
            return str(v)
    return default


def analyze(jsonl_path: str) -> int:
    if not os.path.exists(jsonl_path):
        print(f"[ERROR] File not found: {jsonl_path}")
        return 1

    events = []
    bad_lines = 0
    with open(jsonl_path, "r", encoding="utf-8", errors="replace") as f:
        for line_no, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                events.append(json.loads(raw))
            except json.JSONDecodeError as e:
                bad_lines += 1
                print(f"[WARN] Malformed JSON on line {line_no}: {e}")

    has_stage_b = any(str(ev.get("event", "")).startswith("StageB_") for ev in events)
    has_stage_c = any(
        ev.get("event") in ("StageC_ConfigLoaded", "StageC_StreamEvent")
        or ev.get("mode") == "ipc"
        or ev.get("eff_mode") == "ipc"
        or ev.get("req_mode") == "ipc"
        for ev in events
    )
    if has_stage_c:
        stage_label = "Stage C / B / A"
    elif has_stage_b:
        stage_label = "Stage B / A"
    else:
        stage_label = "Stage A"
    print(f"=== IVRadioProbe {stage_label} Analysis ({len(events)} events, {bad_lines} malformed) ===")

    by_event = collections.Counter(ev.get("event") for ev in events)
    print("\n[1] Event Counts:")
    for k, v in by_event.most_common():
        print(f"  {str(k):28s}: {v}")

    config_events = [ev for ev in events if ev.get("event") == "StageB_ConfigLoaded"]
    if config_events:
        print(f"\n[1b] StageB_ConfigLoaded ({len(config_events)}):")
        for idx, cfg in enumerate(config_events, 1):
            print(
                f"  #{idx}: req_mode={cfg.get('req_mode')} eff_mode={cfg.get('eff_mode')} "
                f"status={cfg.get('status')} ini_path={cfg.get('ini_path')!r} "
                f"wav_path={cfg.get('wav_path', '<none>')!r}"
            )

    stagec_configs = [ev for ev in events if ev.get("event") == "StageC_ConfigLoaded"]
    if stagec_configs:
        print(f"\n[1c] StageC_ConfigLoaded ({len(stagec_configs)}):")
        for idx, cfg in enumerate(stagec_configs, 1):
            print(
                f"  #{idx}: mapping={cfg.get('mapping_name', cfg.get('mapping', '<default>'))!r} "
                f"target_count={cfg.get('target_count', '?')} "
                f"steady_lead_ms={cfg.get('steady_lead_ms')} hb_timeout_ms={cfg.get('heartbeat_timeout_ms')} "
                f"fade_ms={cfg.get('fade_ms')} hot_reattach={cfg.get('allow_hot_reattach')}"
            )

    readers = collections.defaultdict(list)
    for ev in events:
        rid = ev.get("reader_id", 0)
        if rid > 0:
            readers[rid].append(ev)

    guard_skips_total = []
    decisions_all = []
    reader_summaries = {}

    print(f"\n[2] Reader Instances ({len(readers)} total):")
    for rid in sorted(readers):
        evs = readers[rid]
        kind = evs[0].get("kind")
        t_start = evs[0]["ts_us"] / 1e6
        t_end = evs[-1]["ts_us"] / 1e6
        tids = sorted({e["tid"] for e in evs})
        path = next((e.get("path") or e.get("url") for e in evs if e["event"] == "Open"), "<unopened>")
        iids = collections.Counter(f"{e.get('iid')}(wrapped={e.get('wrapped')})" for e in evs if e["event"] == "QueryInterface")
        attrs = [f"{e.get('attr')}={e.get('val')}" for e in evs if e["event"].startswith("GetAttribute") and e.get("val") and not str(e.get("val")).startswith("<null")]
        formats = [
            f"tag={e.get('format_tag')} {e.get('sample_rate')}Hz {e.get('channels')}ch {e.get('bits_per_sample')}bit"
            for e in evs if e["event"] in ("GetOutputProps", "SetOutputProps_Req") and "sample_rate" in e
        ]
        ranges = [(e.get("start_100ns", 0) / 1e7, e.get("dur_100ns", 0) / 1e7) for e in evs if e["event"] == "SetRange"]
        samples = [e for e in evs if e["event"] in ("GetNextSample", "Async_OnSample")]
        close_ev = next((e for e in evs if e["event"] == "Close"), None)
        destroyed = next((e for e in evs if e["event"].endswith("_Destroyed")), None)
        tot_samples = destroyed.get("total_samples") if destroyed else (samples[-1].get("seq") if samples else 0)
        tot_bytes = destroyed.get("total_bytes") if destroyed else (samples[-1].get("total_bytes") if samples else 0)

        stageb_decisions = [e for e in evs if e["event"] == "StageB_ReaderDecision"]
        stageb_seeks = [e for e in evs if e["event"] == "StageB_SeekAligned"]
        stageb_skips = [e for e in evs if e["event"] == "StageB_SampleGuardSkip"]
        guard_skips_total.extend((rid, sk) for sk in stageb_skips)
        for dec in stageb_decisions:
            decisions_all.append((rid, path, dec))

        counter_src = destroyed if (destroyed and "replaced_samples" in destroyed) else (
            close_ev if (close_ev and "replaced_samples" in close_ev) else None
        )

        reader_summaries[rid] = {
            "path": path,
            "tot_samples": tot_samples or 0,
            "tot_bytes": tot_bytes or 0,
            "decisions": stageb_decisions,
            "counter_src": counter_src,
            "samples": samples,
        }

        print(f"\n  --- Reader #{rid} ({kind}) [{t_start:.3f}s -> {t_end:.3f}s, wall={t_end - t_start:.3f}s] TIDs={tids} ---")
        print(f"    Path      : {path}")
        print(f"    QIs       : {dict(iids)}")
        if formats:
            print(f"    Formats   : {sorted(set(formats))}")
        if attrs:
            print(f"    Attrs     : {attrs[:10]}")
        if ranges:
            print(f"    SetRange  : {ranges}")
        print(f"    Totals    : samples={tot_samples}, bytes={tot_bytes}")

        if stageb_decisions:
            for dec in stageb_decisions:
                print(
                    f"    StageBDec : replace_active={dec.get('replace_active')} reason={dec.get('reason')} "
                    f"open_tid={dec.get('open_tid')} sample_tid={dec.get('sample_tid')} "
                    f"target_match={dec.get('target_match')}"
                )
        if stageb_seeks:
            seek_strs = [
                f"(req={s.get('req_start_100ns')}, actual_pts={s.get('actual_pts_100ns')}, frame={s.get('aligned_frame')})"
                for s in stageb_seeks
            ]
            print(f"    SeekAlign : {', '.join(seek_strs)}")
        if stageb_skips:
            for sk in stageb_skips:
                print(
                    f"    [WARN] GuardSkip: seq={sk.get('seq')} guard_reason={sk.get('guard_reason')} bytes={sk.get('bytes')}"
                )
        if counter_src is not None:
            print(
                f"    StageBCnt : replaced_samples={counter_src.get('replaced_samples', 0)}, "
                f"replaced_bytes={counter_src.get('replaced_bytes', 0)}, "
                f"passthrough_samples={counter_src.get('passthrough_samples', 0)}"
            )

        if len(samples) >= 2:
            ok_samples = [s for s in samples if s.get("hr", "0x00000000") == "0x00000000"]
            if len(ok_samples) >= 2:
                wall_span_s = (ok_samples[-1]["ts_us"] - ok_samples[0]["ts_us"]) / 1e6
                media_span_s = (ok_samples[-1]["pts_100ns"] - ok_samples[0]["pts_100ns"]) / 1e7
                deltas_ms = [s["delta_wall_us"] / 1000.0 for s in ok_samples[1:min(len(ok_samples), 200)]]
                med_ms = statistics.median(deltas_ms) if deltas_ms else 0.0
                max_ms = max(deltas_ms) if deltas_ms else 0.0
                lead_s = [
                    ((s["pts_100ns"] - ok_samples[0]["pts_100ns"]) / 1e7) - ((s["ts_us"] - ok_samples[0]["ts_us"]) / 1e6)
                    for s in ok_samples
                ]
                print(
                    f"    Pacing    : media_span={media_span_s:.3f}s over wall_span={wall_span_s:.3f}s | "
                    f"early_delta_median={med_ms:.2f}ms max={max_ms:.2f}ms | "
                    f"prebuffer_lead_max={max(lead_s):.3f}s"
                )
            eofs = [s for s in samples if s.get("hr") != "0x00000000"]
            if eofs:
                print(f"    Non-OK HR : {[(s.get('seq'), s.get('hr')) for s in eofs]}")

    # [3] Stage B Contract Audit
    same_thread_violations = []
    non_target_violations = []
    for rid, path, dec in decisions_all:
        replace_active = bool(dec.get("replace_active", False))
        open_tid = dec.get("open_tid")
        sample_tid = dec.get("sample_tid")
        target_match = dec.get("target_match")
        if open_tid is not None and sample_tid is not None and open_tid == sample_tid and replace_active:
            same_thread_violations.append((rid, open_tid, dec.get("reason")))
        is_target = bool(target_match) if target_match is not None else _is_target_filename(path)
        if not is_target and replace_active:
            non_target_violations.append((rid, path, dec.get("reason")))

    print("\n[3] Stage B Contract Audit:")
    c_json = (bad_lines == 0)
    c_guard = (len(guard_skips_total) == 0)
    c_same_tid = (len(same_thread_violations) == 0)
    c_non_target = (len(non_target_violations) == 0)

    print(f"  [{'PASS' if c_json else 'FAIL'}] Malformed JSON lines          : {bad_lines} (expected 0)")
    print(f"  [{'PASS' if c_guard else 'FAIL'}] StageB_SampleGuardSkip events : {len(guard_skips_total)} (expected 0)")
    if decisions_all:
        print(
            f"  [{'PASS' if c_same_tid else 'FAIL'}] Same-thread (open_tid==sample_tid) replace_active==False : "
            f"{len(same_thread_violations)} violation(s)"
        )
        print(
            f"  [{'PASS' if c_non_target else 'FAIL'}] Non-target files replace_active==False                 : "
            f"{len(non_target_violations)} violation(s)"
        )
    else:
        print("  [INFO] No StageB_ReaderDecision events present (Stage A baseline log or zero samples decoded).")

    c_stagec_ok = True
    if has_stage_c:
        stream_events = [ev for ev in events if ev.get("event") == "StageC_StreamEvent"]

        attach_sessions = []
        seen_attach_keys = set()
        disconnect_events = []
        lease_claims = []
        lease_releases = []
        preload_resyncs = []
        underrun_ev_count = 0
        underrun_frames_max = 0
        overrun_ev_count = 0
        overrun_skipped_max = 0
        overwrite_conflict_count = 0
        preload_skipped_sum = 0
        preload_skipped_max = 0

        for ev in stream_events:
            kind_str = _ev_str(ev, "stream_event", "event_name", "action", "kind_detail", "reason", "status", "type", "kind").lower()
            gen = _ev_int(ev, "gen", "session_generation", "generation", default=0)
            sid = _ev_int(ev, "session_id", "sid", default=0)

            if "attach" in kind_str or kind_str == "producer_attached" or kind_str == "session_attached":
                key = (gen, sid)
                if key not in seen_attach_keys:
                    seen_attach_keys.add(key)
                    attach_sessions.append((gen, sid, ev.get("reader_id", 0)))
            if "disconnect" in kind_str or "offline" in kind_str:
                disconnect_events.append((ev.get("reader_id", 0), kind_str, gen))
            if "claim" in kind_str or "primary_acquired" in kind_str or "lease_acquired" in kind_str or "stale_takeover" in kind_str:
                lease_claims.append((ev.get("reader_id", 0), kind_str, gen))
            if "release" in kind_str or "lease_released" in kind_str or "primary_released" in kind_str:
                lease_releases.append((ev.get("reader_id", 0), kind_str, gen))
            if "preload_resync" in kind_str:
                skipped = _ev_int(ev, "skipped_frames", "preload_skipped_frames", "frames_skipped", "skipped", default=0)
                preload_resyncs.append((ev.get("reader_id", 0), skipped, gen))
                preload_skipped_sum += skipped
            if "underrun" in kind_str:
                underrun_ev_count += max(1, _ev_int(ev, "delta_events", default=1))
                underrun_frames_max += _ev_int(ev, "missing", default=0)
            if "overrun" in kind_str:
                overrun_ev_count += max(1, _ev_int(ev, "delta_events", default=1))
                overrun_skipped_max += _ev_int(ev, "skipped", default=0)
            if "overwrite_conflict" in kind_str or "conflict" in kind_str:
                overwrite_conflict_count += max(1, _ev_int(ev, "delta_events", default=1))

            underrun_frames_max = max(underrun_frames_max, _ev_int(ev, "underrun_frames", default=0))
            overrun_skipped_max = max(overrun_skipped_max, _ev_int(ev, "overrun_skipped_frames", default=0))
            preload_skipped_max = max(preload_skipped_max, _ev_int(ev, "preload_skipped_frames", default=0))
            overwrite_conflict_count = max(
                overwrite_conflict_count,
                _ev_int(ev, "overwrite_conflict_events", "overwrite_conflicts", default=0),
            )
            underrun_ev_count = max(underrun_ev_count, _ev_int(ev, "underrun_events", default=0))
            overrun_ev_count = max(overrun_ev_count, _ev_int(ev, "overrun_events", default=0))

        # Fallback: also inspect Destroyed/Close/other StageC telemetry fields if attached on reader lifecycle events
        for ev in events:
            if "overwrite_conflict_events" in ev:
                overwrite_conflict_count = max(overwrite_conflict_count, _ev_int(ev, "overwrite_conflict_events"))
            if "underrun_events" in ev:
                underrun_ev_count = max(underrun_ev_count, _ev_int(ev, "underrun_events"))
            if "overrun_events" in ev:
                overrun_ev_count = max(overrun_ev_count, _ev_int(ev, "overrun_events"))
            if "preload_skipped_frames" in ev:
                preload_skipped_max = max(preload_skipped_max, _ev_int(ev, "preload_skipped_frames"))

        effective_preload_skipped = max(preload_skipped_sum, preload_skipped_max)
        if overwrite_conflict_count > 0:
            c_stagec_ok = False

        # Compute per-reader undecided preload silence share for active IPC readers
        ipc_reader_rows = []
        for rid in sorted(reader_summaries):
            rs = reader_summaries[rid]
            decs = rs["decisions"]
            is_ipc_active = any(d.get("mode") == "ipc" and bool(d.get("replace_active", False)) for d in decs)
            if not is_ipc_active:
                continue
            tot_samp = rs["tot_samples"]
            tot_bytes = rs["tot_bytes"]
            # Estimate undecided preload window (seq <= 221) if explicit field not logged
            cnt_src = rs["counter_src"] or {}
            if "undecided_silence_samples" in cnt_src or "undecided_silence_bytes" in cnt_src:
                sil_samp = _ev_int(cnt_src, "undecided_silence_samples")
                sil_bytes = _ev_int(cnt_src, "undecided_silence_bytes")
            else:
                sil_samp = min(tot_samp, 221)
                if sil_samp == 0:
                    sil_bytes = 0
                elif tot_samp <= 221:
                    sil_bytes = tot_bytes
                else:
                    # 1st sample is 2496B (624 frames) + up to 220 * 4608B (1152 frames) = 1,016,256B
                    sil_bytes = min(tot_bytes, 2496 + (sil_samp - 1) * 4608)
            pct_samp = (100.0 * sil_samp / tot_samp) if tot_samp > 0 else 0.0
            pct_bytes = (100.0 * sil_bytes / tot_bytes) if tot_bytes > 0 else 0.0
            ipc_reader_rows.append((rid, rs["path"], sil_samp, tot_samp, pct_samp, sil_bytes, tot_bytes, pct_bytes))

        detach_evs = [ev for ev in events if ev.get("event") == "Process_Detach"]
        c_overwrite = (overwrite_conflict_count == 0)

        print("\n[4] Stage C IPC Stream & Dual-Slot Audit:")
        print("  (a) Process / Pipeline Stability:")
        print(f"    [{'PASS' if c_overwrite else 'FAIL'}] overwrite_conflict_events     : {overwrite_conflict_count} (expected 0)")
        print(f"    [INFO] Producer session attachments  : {len(attach_sessions)} -> {attach_sessions}")
        print(f"    [INFO] Producer disconnect events    : {len(disconnect_events)}")
        print(f"    [INFO] Primary lease claims/releases : claims={len(lease_claims)}, releases={len(lease_releases)}")
        print(f"    [INFO] Underrun / Overrun events     : underruns={underrun_ev_count} (frames={underrun_frames_max}), overruns={overrun_ev_count} (skipped={overrun_skipped_max})")
        print(f"    [INFO] Clean Process_Detach events   : {len(detach_evs)}")

        print("  (b) Audio Continuity Gap Metrics:")
        print(f"    [INFO] preload_resync events         : {len(preload_resyncs)}, skipped_source_frames={effective_preload_skipped}")
        if ipc_reader_rows:
            for rid, path, sil_s, tot_s, p_s, sil_b, tot_b, p_b in ipc_reader_rows:
                leaf = str(path).replace("/", "\\").rsplit("\\", 1)[-1]
                print(
                    f"    Reader #{rid} ({leaf}): undecided_preload_silence={sil_s}/{tot_s} samples ({p_s:.1f}%), "
                    f"{sil_b}/{tot_b} bytes ({p_b:.1f}%)"
                )
        else:
            print("    [INFO] No active Mode=ipc replaced readers recorded.")

    return 0 if (c_json and c_guard and c_same_tid and c_non_target and c_stagec_ok) else 1


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "IVRadioProbe.jsonl"
    raise SystemExit(analyze(target))
