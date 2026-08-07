#!/usr/bin/env python3
"""
navien_diagnostic.py

Extracts 3 weeks of InfluxDB data and produces a diagnostic report
to understand why on-device measured scheduling efficiency is low.

Analyses:
  1. Demand event distribution heatmap (day-of-week × hour-of-day, UTC and local)
  2. Per demand event: was recirculation active within the prior 15 min?
  3. Wasted recirc cycles (ran but no tap followed within 15 min)
  4. Schedule slot coverage vs actual demand (from most recent learner broadcast)
  5. Suggested problem areas and parameter hints

Usage:
  python3 navien_diagnostic.py                          # last 3 weeks, UTC display
  python3 navien_diagnostic.py --weeks 4                # extend window
  python3 navien_diagnostic.py --utc_offset -420        # show times in local TZ (PDT = -420 min)
  python3 navien_diagnostic.py --min_duration_min 1.0   # filter short events (recirc artifacts)
  python3 navien_diagnostic.py --csv events.csv         # also save raw events to CSV
  python3 navien_diagnostic.py --influxdb_host 192.168.1.10
"""

import argparse
import csv
import sys
from datetime import datetime, timedelta, timezone
from collections import defaultdict

import config  # Navien shared config

# ---------------------------------------------------------------------------
# InfluxDB helpers
# ---------------------------------------------------------------------------

def get_client(args):
    from influxdb import InfluxDBClient
    return InfluxDBClient(
        host=args.influxdb_host,
        port=args.influxdb_port,
        database=args.influxdb_db,
    )


def query_water(client, start_utc: datetime, end_utc: datetime):
    """
    Fetch water measurement fields needed for demand-event and recirc analysis.
    Returns a list of dicts sorted by time ascending.
    """
    start_s = start_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
    end_s   = end_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
    q = (
        f"SELECT consumption_active, recirculation_active, recirculation_running, flow_lpm "
        f"FROM water "
        f"WHERE time >= '{start_s}' AND time < '{end_s}' "
        f"ORDER BY time ASC"
    )
    result = client.query(q)
    return list(result.get_points(measurement="water"))


def query_learner_latest(client):
    """
    Return the most recent learner broadcast record, or None.
    Contains fields like sun_slots, mon_slots, ... as 'HH:MM-HH:MM,...' strings.
    """
    q = "SELECT * FROM learner ORDER BY time DESC LIMIT 1"
    result = client.query(q)
    pts = list(result.get_points(measurement="learner"))
    return pts[0] if pts else None


# ---------------------------------------------------------------------------
# Demand-event extraction
# ---------------------------------------------------------------------------

RECIRC_HOT_WINDOW_MIN = config.RECIRC_WINDOW_MINUTES  # 15 min


def parse_points(points):
    """
    Convert raw InfluxDB point list to list of dicts with parsed datetimes.
    Handles both Z-suffix and +00:00 timezone formats.
    """
    rows = []
    for p in points:
        ts = p["time"]
        if ts.endswith("Z"):
            dt = datetime.fromisoformat(ts[:-1]).replace(tzinfo=timezone.utc)
        else:
            dt = datetime.fromisoformat(ts).astimezone(timezone.utc)
        rows.append({
            "dt":                   dt,
            "consumption_active":   int(p.get("consumption_active")   or 0),
            "recirculation_active": int(p.get("recirculation_active") or 0),
            "recirculation_running":int(p.get("recirculation_running")or 0),
            "flow_lpm":             float(p.get("flow_lpm")           or 0.0),
        })
    return sorted(rows, key=lambda r: r["dt"])


def extract_demand_events(rows, cold_gap_min=10, min_duration_min=0.0):
    """
    Detect consumption 0→1 transitions that follow at least cold_gap_min of
    inactivity.  For each demand event, determine whether recirculation was
    active (mode on OR pump running) in the preceding RECIRC_HOT_WINDOW_MIN.

    min_duration_min: drop events shorter than this (0 = keep all).
    Use ~1.0 to filter recirc pump artifacts (brief flow pulses every ~17 min).

    Returns list of dicts:
      dt, dow, hour_utc, minute_utc, duration_min, recirc_covered, flow_peak_lpm
    """
    cold_gap = timedelta(minutes=cold_gap_min)
    hot_window = timedelta(minutes=RECIRC_HOT_WINDOW_MIN)

    last_consumption_end = None
    in_run = False
    run_start = None
    run_rows = []
    demand_events = []

    def _finish_run(run_rows, rows_context):
        if not run_rows:
            return
        start_dt  = run_rows[0]["dt"]
        end_dt    = run_rows[-1]["dt"]
        duration  = (end_dt - start_dt).total_seconds() / 60.0

        # Was recirc active in the RECIRC_HOT_WINDOW_MIN before this run started?
        window_start = start_dt - hot_window
        recirc_covered = False
        for r in rows_context:
            if r["dt"] < window_start:
                continue
            if r["dt"] >= start_dt:
                break
            if r["recirculation_active"] or r["recirculation_running"]:
                recirc_covered = True
                break

        flow_peak = max((r["flow_lpm"] for r in run_rows), default=0.0)

        if duration < min_duration_min:
            return  # filter out recirc-pump artifacts and trivial taps

        demand_events.append({
            "dt":            start_dt,
            "dow":           start_dt.weekday() + 1 if start_dt.weekday() < 6 else 0,
            # ^ convert Python Mon=0 to Sun=0 (tm_wday convention)
            "dow_name":      ["Sun","Mon","Tue","Wed","Thu","Fri","Sat"][
                              (start_dt.isoweekday() % 7)],
            "hour_utc":      start_dt.hour,
            "minute_utc":    start_dt.minute,
            "duration_min":  round(duration, 1),
            "recirc_covered":recirc_covered,
            "flow_peak_lpm": round(flow_peak, 1),
        })

    for i, row in enumerate(rows):
        active = row["consumption_active"]
        if active and not in_run:
            # Start of a new run
            # Is this a new demand event (preceded by enough inactivity)?
            if last_consumption_end is None or (row["dt"] - last_consumption_end) >= cold_gap:
                in_run = True
                run_rows = [row]
            else:
                # Continuation within cold_gap — merge into same event, don't double-count
                if in_run:
                    run_rows.append(row)
        elif active and in_run:
            run_rows.append(row)
        elif not active and in_run:
            _finish_run(run_rows, rows)
            last_consumption_end = run_rows[-1]["dt"]
            in_run = False
            run_rows = []
        elif not active and not in_run:
            if row["consumption_active"] == 0 and last_consumption_end is not None:
                pass  # normal idle

    if in_run:
        _finish_run(run_rows, rows)

    return demand_events


# ---------------------------------------------------------------------------
# Recirc cycle analysis
# ---------------------------------------------------------------------------

def extract_recirc_cycles(rows):
    """
    Find contiguous runs where recirculation_running == 1.
    For each, check if a consumption_active event follows within RECIRC_HOT_WINDOW_MIN.
    Returns list of dicts: dt_start, dt_end, duration_min, had_tap.
    """
    hot_window = timedelta(minutes=RECIRC_HOT_WINDOW_MIN)
    cycles = []
    in_cycle = False
    cycle_start = None

    for i, row in enumerate(rows):
        running = row["recirculation_running"]
        if running and not in_cycle:
            in_cycle = True
            cycle_start = row["dt"]
        elif not running and in_cycle:
            cycle_end = rows[i - 1]["dt"] if i > 0 else row["dt"]
            duration = (cycle_end - cycle_start).total_seconds() / 60.0

            # Look for any consumption in the hot window after cycle end
            window_end = cycle_end + hot_window
            had_tap = any(
                r["consumption_active"]
                for r in rows
                if cycle_end < r["dt"] <= window_end and r["consumption_active"]
            )

            cycles.append({
                "dt_start":    cycle_start,
                "dt_end":      cycle_end,
                "duration_min":round(duration, 1),
                "had_tap":     had_tap,
            })
            in_cycle = False

    return cycles


# ---------------------------------------------------------------------------
# Slot schedule parsing
# ---------------------------------------------------------------------------

DAY_PREFIXES = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
DAY_NAMES_FULL = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]


def parse_learner_slots(learner_row):
    """
    Parse slot strings like 'HH:MM-HH:MM,HH:MM-HH:MM' from a learner record.
    Returns dict: dow (0=Sun) → list of (start_min, end_min) in minutes-since-midnight UTC.
    """
    schedule = {}
    for i, pfx in enumerate(DAY_PREFIXES):
        key = f"{pfx}_slots"
        raw = learner_row.get(key, "") or ""
        slots = []
        for seg in raw.split(","):
            seg = seg.strip()
            if not seg:
                continue
            try:
                start_s, end_s = seg.split("-")
                sh, sm = map(int, start_s.split(":"))
                eh, em = map(int, end_s.split(":"))
                slots.append((sh * 60 + sm, eh * 60 + em))
            except ValueError:
                pass
        schedule[i] = slots
    return schedule


def is_covered_by_slot(dt_utc, schedule, utc_offset_min=0):
    """
    Return True if dt_utc falls within any scheduled slot for its day-of-week.
    Schedule uses UTC slots; dow is derived from UTC time.
    """
    dow = dt_utc.isoweekday() % 7  # Sun=0, Mon=1 .. Sat=6
    slots = schedule.get(dow, [])
    minute = dt_utc.hour * 60 + dt_utc.minute
    for start_m, end_m in slots:
        if start_m <= end_m:
            if start_m <= minute < end_m:
                return True
        else:
            # Slot wraps midnight
            if minute >= start_m or minute < end_m:
                return True
    return False


# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

def local_hour(dt_utc, utc_offset_min):
    local = dt_utc + timedelta(minutes=utc_offset_min)
    return local.hour, local.weekday()  # (hour, Python weekday Mon=0)


def heatmap_row(counts, total):
    BAR = "█"
    cells = []
    for h in range(24):
        n = counts.get(h, 0)
        frac = n / total if total > 0 else 0
        if frac == 0:
            cells.append("  .")
        elif frac < 0.03:
            cells.append("  ▁")
        elif frac < 0.06:
            cells.append("  ▃")
        elif frac < 0.10:
            cells.append("  ▅")
        elif frac < 0.15:
            cells.append("  ▇")
        else:
            cells.append(f"  {BAR}")
    return " ".join(cells)


def fmt_slot(start_m, end_m, utc_offset_min=0):
    def hm(m):
        return f"{(m // 60) % 24:02d}:{m % 60:02d}"
    utc_str = f"{hm(start_m)}-{hm(end_m)}"
    if utc_offset_min == 0:
        return utc_str
    local_start = start_m + utc_offset_min
    local_end   = end_m   + utc_offset_min
    return f"{hm(local_start)}-{hm(local_end)} (UTC {utc_str})"


# ---------------------------------------------------------------------------
# Main report
# ---------------------------------------------------------------------------

def run_report(args):
    client = get_client(args)

    end_utc   = datetime.now(timezone.utc).replace(second=0, microsecond=0)
    start_utc = end_utc - timedelta(weeks=args.weeks)

    print(f"\n{'='*70}")
    print(f"  Navien Scheduler Diagnostic")
    print(f"  Window: {start_utc.strftime('%Y-%m-%d')} → {end_utc.strftime('%Y-%m-%d')} UTC")
    print(f"  UTC offset: {args.utc_offset:+d} min"
          f"  ({args.utc_offset // 60:+d}h)"
          if args.utc_offset else f"  Showing UTC times")
    print(f"{'='*70}\n")

    # ------------------------------------------------------------------
    # Fetch data
    # ------------------------------------------------------------------
    print("Querying InfluxDB water measurement...", end=" ", flush=True)
    raw_points = query_water(client, start_utc, end_utc)
    print(f"{len(raw_points)} rows")

    if not raw_points:
        print("No data found. Check --influxdb_host and --influxdb_db.")
        sys.exit(1)

    rows = parse_points(raw_points)

    print("Querying latest learner broadcast...", end=" ", flush=True)
    learner_row = query_learner_latest(client)
    if learner_row:
        lr_time = learner_row.get("time", "unknown")
        print(f"found (time={lr_time})")
        # Warn if slot data is stale — device recomputes nightly so > 2 days old
        # means navien_listener.py probably isn't running continuously.
        try:
            lr_dt = datetime.fromisoformat(lr_time.replace("Z", "+00:00"))
            age_days = (datetime.now(timezone.utc) - lr_dt).days
            if age_days > 2:
                print(f"  *** WARNING: learner data is {age_days} days old. ***")
                print(f"  *** navien_listener.py may not be running. Slot coverage")
                print(f"  *** analysis (section 4) will use this stale schedule. ***")
        except Exception:
            pass
        schedule = parse_learner_slots(learner_row)
    else:
        print("NOT FOUND — no learner data in InfluxDB")
        schedule = {}

    # ------------------------------------------------------------------
    # 1. Demand-event extraction
    # ------------------------------------------------------------------
    dur_note = f" (min duration {args.min_duration_min} min)" if args.min_duration_min else ""
    print(f"\nExtracting demand events{dur_note}...", end=" ", flush=True)
    demand_events = extract_demand_events(rows,
                                      cold_gap_min=args.cold_gap_minutes,
                                      min_duration_min=args.min_duration_min)
    print(f"{len(demand_events)} events")

    covered   = [e for e in demand_events if e["recirc_covered"]]
    uncovered = [e for e in demand_events if not e["recirc_covered"]]
    measured_pct = 100.0 * len(covered) / len(demand_events) if demand_events else 0.0

    # ------------------------------------------------------------------
    # 2. Recirc cycle analysis
    # ------------------------------------------------------------------
    print("Extracting recirc cycles...", end=" ", flush=True)
    cycles = extract_recirc_cycles(rows)
    wasted  = [c for c in cycles if not c["had_tap"]]
    useful  = [c for c in cycles if c["had_tap"]]
    print(f"{len(cycles)} cycles ({len(useful)} useful, {len(wasted)} wasted)")

    # ------------------------------------------------------------------
    # Section 1: Overall summary
    # ------------------------------------------------------------------
    weeks = args.weeks
    print(f"\n{'─'*70}")
    print(f"  1. OVERALL SUMMARY  ({weeks}-week window)")
    print(f"{'─'*70}")
    print(f"  Demand events total:          {len(demand_events):>5}")
    print(f"  Covered by recirc (measured): {len(covered):>5}  ({measured_pct:.1f}%)")
    print(f"  Not covered (cold pipe):      {len(uncovered):>5}  ({100-measured_pct:.1f}%)")
    print(f"  Recirc cycles total:          {len(cycles):>5}")
    print(f"  Useful recirc cycles:         {len(useful):>5}")
    print(f"  Wasted recirc cycles:         {len(wasted):>5}"
          + (f"  ({100*len(wasted)/len(cycles):.0f}% waste)" if cycles else ""))

    avg_de_per_day = len(demand_events) / (weeks * 7)
    print(f"  Avg demand events/day:        {avg_de_per_day:.1f}")

    # ------------------------------------------------------------------
    # Section 2: Per-day-of-week breakdown
    # ------------------------------------------------------------------
    print(f"\n{'─'*70}")
    print(f"  2. PER-DAY-OF-WEEK BREAKDOWN")
    print(f"{'─'*70}")
    hdr = f"  {'Day':<11} {'Total':>7} {'Covered':>8} {'Meas%':>7} {'Wst Rcirc':>10} {'Slots (UTC)' if not args.utc_offset else 'Slots (local)'}"
    print(hdr)
    print("  " + "─" * (len(hdr) - 2))

    dow_names = ["Sun","Mon","Tue","Wed","Thu","Fri","Sat"]
    for dow in range(7):
        day_de   = [e for e in demand_events if e["dow_name"] == dow_names[dow]]
        day_cov  = [e for e in day_de if e["recirc_covered"]]
        day_pct  = 100.0 * len(day_cov) / len(day_de) if day_de else None

        day_cyc  = [c for c in cycles
                    if c["dt_start"].isoweekday() % 7 == dow]
        day_wst  = sum(1 for c in day_cyc if not c["had_tap"])

        slots = schedule.get(dow, [])
        slot_str = "  ".join(fmt_slot(s, e, args.utc_offset) for s, e in slots) if slots else "(none)"

        pct_str = f"{day_pct:.1f}%" if day_pct is not None else "  N/A"
        print(f"  {dow_names[dow]:<11} {len(day_de):>7} {len(day_cov):>8} {pct_str:>7} {day_wst:>10}   {slot_str}")

    # ------------------------------------------------------------------
    # Section 3: Demand-event heatmap by hour
    # ------------------------------------------------------------------
    tz_label = "UTC" if not args.utc_offset else f"UTC{args.utc_offset//60:+d}"
    print(f"\n{'─'*70}")
    print(f"  3. DEMAND-EVENT HEATMAP  (by day-of-week and hour, {tz_label})")
    print(f"     ▁=<3%  ▃=<6%  ▅=<10%  ▇=<15%  █=≥15%  of that day's events")
    print(f"{'─'*70}")

    print(f"  {'Day':<5} 00  01  02  03  04  05  06  07  08  09  10  11  12  13  14  15  16  17  18  19  20  21  22  23")

    for dow in range(7):
        day_de = [e for e in demand_events if e["dow_name"] == dow_names[dow]]
        if not day_de:
            continue
        hour_counts = defaultdict(int)
        for e in day_de:
            h = e["hour_utc"]
            if args.utc_offset:
                local_dt = e["dt"] + timedelta(minutes=args.utc_offset)
                h = local_dt.hour
            hour_counts[h] += 1

        # Compact 2-char representation per hour
        total = len(day_de)
        cells = []
        for h in range(24):
            n = hour_counts.get(h, 0)
            frac = n / total if total > 0 else 0
            if frac == 0:      cells.append(" .")
            elif frac < 0.03:  cells.append(" ▁")
            elif frac < 0.06:  cells.append(" ▃")
            elif frac < 0.10:  cells.append(" ▅")
            elif frac < 0.15:  cells.append(" ▇")
            else:              cells.append(" █")

        # Overlay scheduled slot hours with brackets
        if schedule.get(dow):
            slot_hours = set()
            for s_min, e_min in schedule[dow]:
                disp_start = (s_min + args.utc_offset) if args.utc_offset else s_min
                disp_end   = (e_min + args.utc_offset) if args.utc_offset else e_min
                for m in range(disp_start, disp_end, 60):
                    slot_hours.add((m // 60) % 24)
            marked = list(cells)
            for h in slot_hours:
                marked[h] = marked[h][0] + marked[h][1].replace(".", "░") if marked[h][1] == "." else marked[h]
        else:
            marked = cells

        row = "".join(marked)
        print(f"  {dow_names[dow]:<5}{row}   ({total} events)")

    print(f"\n  Note: scheduled slot hours are not highlighted in this view.")
    print(f"  Use section 4 to see coverage gaps.")

    # ------------------------------------------------------------------
    # Section 4: Slot coverage gap analysis
    # ------------------------------------------------------------------
    print(f"\n{'─'*70}")
    print(f"  4. SLOT COVERAGE ANALYSIS  (schedule vs observed demand)")
    print(f"{'─'*70}")

    if not schedule or not any(schedule.values()):
        print("  No learner schedule found — cannot analyse coverage.")
    else:
        print(f"  For each day, uncovered demand events are those where consumption")
        print(f"  did NOT fall inside a scheduled slot (±{int(RECIRC_HOT_WINDOW_MIN)} min window).")
        print()

        for dow in range(7):
            day_de = [e for e in demand_events if e["dow_name"] == dow_names[dow]]
            if not day_de:
                continue

            in_slot   = []
            near_slot = []
            outside   = []
            hot_window = timedelta(minutes=RECIRC_HOT_WINDOW_MIN)

            slots = schedule.get(dow, [])

            for e in day_de:
                minute = e["hour_utc"] * 60 + e["minute_utc"]
                in_s  = False
                near_s = False
                for s_min, e_min in slots:
                    if s_min <= minute < e_min:
                        in_s = True
                        break
                    # Within hot window after slot end
                    if e_min <= minute <= e_min + int(RECIRC_HOT_WINDOW_MIN):
                        near_s = True

                if in_s:
                    in_slot.append(e)
                elif near_s:
                    near_slot.append(e)
                else:
                    outside.append(e)

            total = len(day_de)
            pct_in   = 100 * len(in_slot)   / total if total else 0
            pct_near = 100 * len(near_slot) / total if total else 0
            pct_out  = 100 * len(outside)   / total if total else 0

            print(f"  {dow_names[dow]} ({total} demand events):")
            print(f"    Inside slot:           {len(in_slot):>3}  ({pct_in:.0f}%)")
            print(f"    Within {int(RECIRC_HOT_WINDOW_MIN)} min after slot: {len(near_slot):>3}  ({pct_near:.0f}%)")
            print(f"    Completely outside:    {len(outside):>3}  ({pct_out:.0f}%)")

            if outside:
                # Show the most common unscheduled hours
                out_hours = defaultdict(int)
                for e in outside:
                    h = e["hour_utc"]
                    if args.utc_offset:
                        h = (e["dt"] + timedelta(minutes=args.utc_offset)).hour
                    out_hours[h] += 1
                top = sorted(out_hours.items(), key=lambda x: -x[1])[:5]
                hour_strs = [f"{h:02d}:xx ({n})" for h, n in top]
                print(f"    Top uncovered hours ({tz_label}): {',  '.join(hour_strs)}")

            print()

    # ------------------------------------------------------------------
    # Section 5: Detailed demand-event listing (last 50)
    # ------------------------------------------------------------------
    print(f"{'─'*70}")
    print(f"  5. RECENT DEMAND EVENTS  (most recent {min(50, len(demand_events))})")
    print(f"{'─'*70}")
    print(f"  {'Timestamp (UTC)':<22} {'Local':<8} {'Dow':<5} "
          f"{'Dur(m)':>7} {'Flow':>6} {'Covered':>8} {'In-Slot':>8}")
    print(f"  {'─'*22} {'─'*8} {'─'*5} {'─'*7} {'─'*6} {'─'*8} {'─'*8}")

    recent = sorted(demand_events, key=lambda e: e["dt"])[-50:]
    for e in recent:
        utc_str = e["dt"].strftime("%Y-%m-%d %H:%M")
        if args.utc_offset:
            local_dt  = e["dt"] + timedelta(minutes=args.utc_offset)
            local_str = local_dt.strftime("%H:%M")
        else:
            local_str = "─"

        in_sched = is_covered_by_slot(e["dt"], schedule) if schedule else False
        cov_str  = "YES" if e["recirc_covered"] else "NO"
        slot_str = "YES" if in_sched else "no"
        print(f"  {utc_str:<22} {local_str:<8} {e['dow_name']:<5} "
              f"{e['duration_min']:>7.1f} {e['flow_peak_lpm']:>6.1f} "
              f"{cov_str:>8} {slot_str:>8}")

    # ------------------------------------------------------------------
    # Section 6: Wasted recirc cycles detail
    # ------------------------------------------------------------------
    print(f"\n{'─'*70}")
    print(f"  6. WASTED RECIRC CYCLES  (ran but no tap within {int(RECIRC_HOT_WINDOW_MIN)} min)")
    print(f"{'─'*70}")

    if not wasted:
        print("  None — all recirc cycles were followed by a tap event.")
    else:
        print(f"  {'Start (UTC)':<22} {'Local':<8} {'Dow':<5} {'Dur(m)':>7}")
        print(f"  {'─'*22} {'─'*8} {'─'*5} {'─'*7}")
        for c in sorted(wasted, key=lambda x: x["dt_start"])[-50:]:
            utc_str = c["dt_start"].strftime("%Y-%m-%d %H:%M")
            local_str = "─"
            if args.utc_offset:
                local_dt  = c["dt_start"] + timedelta(minutes=args.utc_offset)
                local_str = local_dt.strftime("%H:%M")
            dow = dow_names[c["dt_start"].isoweekday() % 7]
            print(f"  {utc_str:<22} {local_str:<8} {dow:<5} {c['duration_min']:>7.1f}")

    # ------------------------------------------------------------------
    # Section 7: Diagnosis hints
    # ------------------------------------------------------------------
    print(f"\n{'─'*70}")
    print(f"  7. DIAGNOSIS HINTS")
    print(f"{'─'*70}")

    gap_pct = 100 - measured_pct
    if gap_pct > 50:
        print(f"  [CRITICAL] Measured efficiency is only {measured_pct:.1f}%.")
        print(f"  The scheduler is running but recirc is not pre-heating pipes in time.")
    elif gap_pct > 25:
        print(f"  [HIGH] Large gap ({gap_pct:.1f}%) between schedule and demand.")
    else:
        print(f"  [OK] Gap is {gap_pct:.1f}% — within normal drift range.")

    recirc_waste_pct = 100 * len(wasted) / len(cycles) if cycles else 0
    if recirc_waste_pct > 60:
        print(f"  [HIGH WASTE] {recirc_waste_pct:.0f}% of recirc cycles have no tap follow-through.")
        print(f"  Slots fire at times when no one uses hot water — slots are misaligned.")
    elif recirc_waste_pct > 30:
        print(f"  [MODERATE WASTE] {recirc_waste_pct:.0f}% of recirc cycles are wasted.")

    # Check if uncovered events have a clear pattern
    if uncovered:
        out_hours = defaultdict(int)
        for e in uncovered:
            out_hours[e["hour_utc"]] += 1
        top_hour, top_count = max(out_hours.items(), key=lambda x: x[1])
        pct_in_top = 100 * top_count / len(uncovered)
        if pct_in_top > 20:
            lh = (top_hour + args.utc_offset // 60) % 24 if args.utc_offset else top_hour
            print(f"  [PATTERN] {pct_in_top:.0f}% of uncovered events fall around"
                  f" {top_hour:02d}:xx UTC ({lh:02d}:xx local).")
            print(f"  Consider adding/widening a slot at that hour.")

    if len(cycles) > 0 and len(wasted) / len(cycles) > 0.5:
        print(f"  [SUGGESTION] Run navien_schedule_learner.py --verbose to see")
        print(f"  per-day peak-finding detail and slot-width comparison.")
        print(f"  Consider --peak_half_width 45 (wider slots) or re-bootstrapping buckets.")

    print(f"\n  Common root causes for large predicted/measured gap:")
    print(f"    a) Slots fire correctly but pipes cool down before tap opens")
    print(f"       → increase peak_half_width or add preheat")
    print(f"    b) Consumption patterns have shifted; bucket data is stale")
    print(f"       → run navien_bucket_export.py --push --replace to reseed")
    print(f"    c) Timezone offset applied incorrectly")
    print(f"       → verify slot times in UTC match actual demand hours")
    print(f"    d) Recirc running too briefly; pipes cool before the tap")
    print(f"       → check Navien recirc cycle duration in Section 6")

    # ------------------------------------------------------------------
    # Optional CSV export
    # ------------------------------------------------------------------
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "timestamp_utc", "local_time", "dow", "duration_min",
                "flow_peak_lpm", "recirc_covered", "in_slot",
            ])
            writer.writeheader()
            for e in sorted(demand_events, key=lambda x: x["dt"]):
                local_str = ""
                if args.utc_offset:
                    local_str = (e["dt"] + timedelta(minutes=args.utc_offset)).strftime("%Y-%m-%d %H:%M")
                writer.writerow({
                    "timestamp_utc": e["dt"].strftime("%Y-%m-%d %H:%M"),
                    "local_time":    local_str,
                    "dow":           e["dow_name"],
                    "duration_min":  e["duration_min"],
                    "flow_peak_lpm": e["flow_peak_lpm"],
                    "recirc_covered":e["recirc_covered"],
                    "in_slot":       is_covered_by_slot(e["dt"], schedule) if schedule else False,
                })
        print(f"\n  CSV saved → {args.csv}")

    print(f"\n{'='*70}\n")
    client.close()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Navien scheduler diagnostic — extract and analyse InfluxDB data",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--influxdb_host",  default=config.INFLUX_HOST)
    parser.add_argument("--influxdb_port",  default=config.INFLUX_PORT, type=int)
    parser.add_argument("--influxdb_db",    default=config.INFLUX_DB)
    parser.add_argument("--weeks",          default=3, type=int,
                        help="How many weeks of history to analyse")
    parser.add_argument("--utc_offset",     default=0, type=int,
                        help="UTC offset in minutes for local-time display "
                             "(e.g. -420 for PDT / UTC-7, -480 for PST / UTC-8)")
    parser.add_argument("--cold_gap_minutes", default=10, type=int,
                        help="Inactivity gap (min) that defines a new demand event")
    parser.add_argument("--min_duration_min", default=0.0, type=float,
                        help="Drop events shorter than this many minutes "
                             "(use 1.0 to filter recirc pump artifacts)")
    parser.add_argument("--csv",            default=None, metavar="FILE",
                        help="Export raw demand events to CSV")
    args = parser.parse_args()
    run_report(args)


if __name__ == "__main__":
    main()
