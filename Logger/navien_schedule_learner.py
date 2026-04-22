#!/usr/bin/python3
"""
navien_schedule_learner.py

Queries InfluxDB for historical Navien water consumption events, clusters them
into per-day-of-week time windows, and pushes the resulting schedule to the
ESP32 via HTTP POST to /schedule.

The ESP32 caches the schedule in NVS flash and drives recirculation from it.

Usage:
    python3 navien_schedule_learner.py [options]

Scheduling (cron example — run every Sunday at 2am):
    0 2 * * 0  /home/pi/navien/venv/bin/python3 /home/pi/navien_schedule_learner.py --influxdb_host localhost --esp32_host navien.local --push

Dependencies:
    pip3 install influxdb requests
"""

import argparse
import json
import sys
from datetime import datetime, timedelta, timezone
from collections import defaultdict

import config   # Navien shared config (rates, InfluxDB connection, timing)


# ---------------------------------------------------------------------------
# Configuration defaults  (override via CLI args)
# InfluxDB connection and cost rates sourced from config.py
# ---------------------------------------------------------------------------
DEFAULT_INFLUXDB_HOST   = config.INFLUX_HOST
DEFAULT_INFLUXDB_PORT   = config.INFLUX_PORT
DEFAULT_INFLUXDB_DB     = config.INFLUX_DB
DEFAULT_ESP32_HOST      = "navien.local"   # mDNS name of the ESP32
DEFAULT_ESP32_PORT      = 8080
DEFAULT_WINDOW_WEEKS    = 4               # Half-width of rolling seasonal window (±4 weeks = 8-week band)
DEFAULT_PREHEAT_MINUTES = config.COLD_PIPE_DRAIN_MINUTES  # Pre-heat = drain time (start recirc this many minutes before demand)
DEFAULT_COLD_GAP        = 10              # Inactivity gap (minutes) after which pipes are considered cold
DEFAULT_MIN_DURATION_GENUINE = 6         # Min tap-on buckets (cold pipes) to count as genuine demand
                                         # At 10-second resolution: 6 buckets = 1 minute
DEFAULT_MIN_DURATION_RECIRC  = 3         # Min tap-on buckets (recirc running) to count as genuine demand
                                         # At 10-second resolution: 3 buckets = 30 seconds
DEFAULT_GAP_MINUTES     = 10             # Merge events closer than this into one window
DEFAULT_MIN_OCCURRENCES = 3              # Minimum raw hits to pass noise filter
DEFAULT_MIN_WEIGHTED_SCORE = 6.0         # Starting score threshold — strong/recent patterns qualify immediately
DEFAULT_MIN_SCORE_FLOOR    = 3.0         # Lowest score the adaptive threshold will relax to
# A floor of 3.0 = one hit per year at weight ×3 (current year only), the weakest
# signal worth scheduling for. Below this the pattern is too sparse to be useful.
MAX_SLOTS_PER_DAY       = 3              # Eve app hard limit (silently truncates a 4th slot)
# Recency weights: list of multipliers applied to each year, most-recent first.
# e.g. [3, 2] means current year ×3, last year ×2.
# Add more entries if you have more years of data (e.g. [3, 2, 1] for 3 years).
DEFAULT_RECENCY_WEIGHTS = [3, 2]

# ---------------------------------------------------------------------------
# Cost model (from config.py)
# Used to scale demand weights by the dollar value of getting recirc right/wrong.
# ---------------------------------------------------------------------------
# Wasted water cost per cold-start: 3 min of drain at typical residential flow.
# We use a representative 8 L/min (mid-range between handwash ~3 and shower ~13).
# This is the savings recirc delivers by having hot water ready at the tap.
AVG_FLOW_LPM            = 8.0
COLD_START_WASTE_USD    = (config.COLD_PIPE_DRAIN_MINUTES
                           * AVG_FLOW_LPM
                           * config.WATER_RATE_USD_PER_L)   # ~$0.097 per cold-start

# Wasted gas cost per recirc cycle that fires with no tap follow-through.
# A typical Navien recirc cycle runs ~2 minutes at ~5,000 kcal/hr (partial load).
RECIRC_CYCLE_MINUTES    = 2.0
RECIRC_PARTIAL_KCAL_HR  = 5000.0
RECIRC_WASTE_USD        = (RECIRC_CYCLE_MINUTES / 60.0
                           * RECIRC_PARTIAL_KCAL_HR
                           * config.GAS_RATE_USD_PER_KCAL)  # ~$0.0024 per wasted cycle

# Normalise both costs to [0,1] so they act as multipliers on demand_weight.
# We use the cold-start waste (larger of the two) as the reference ceiling.
_COST_REF               = COLD_START_WASTE_USD              # reference = max possible saving

DAY_NAMES = ["Sunday", "Monday", "Tuesday", "Wednesday",
             "Thursday", "Friday", "Saturday"]


# ---------------------------------------------------------------------------
# InfluxDB query
# ---------------------------------------------------------------------------
def _year_window_bounds(year, today, window_weeks):
    """
    Return (start_utc, end_utc) strings for the ±window_weeks band around
    today's day-of-year, anchored to `year`.

    Handles year-boundary wrap (e.g. window around Jan 3 includes late Dec
    of the prior year, and window around Dec 29 bleeds into early Jan of
    the next year).
    """
    from datetime import date, timedelta
    # Anchor: same month/day in the target year, handling Feb-29 gracefully
    try:
        anchor = date(year, today.month, today.day)
    except ValueError:
        anchor = date(year, today.month, 28)   # Feb 29 → Feb 28 in non-leap years
    delta = timedelta(weeks=window_weeks)
    start_dt = anchor - delta
    end_dt   = anchor + delta
    # Format as RFC3339 for InfluxDB WHERE clause
    return (start_dt.strftime("%Y-%m-%dT00:00:00Z"),
            end_dt.strftime("%Y-%m-%dT23:59:59Z"))


# Sub-range size for InfluxDB 1.x queries.  At dense 10s GROUP BY, 7 days is
# at most 60,480 points, below typical max-select-point limits (e.g. 100_000).
INFLUX_QUERY_CHUNK_DAYS = 7


def _parse_influx_time_utc(s):
    """Parse RFC3339 timestamp from Influx WHERE clauses (Z suffix)."""
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def _format_influx_time_utc(dt):
    """Format datetime as RFC3339 UTC for Influx WHERE clauses."""
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _extract_cold_starts(points, recency_weight, cold_gap_minutes,
                          min_duration_genuine, min_duration_recirc,
                          verbose=False):
    """
    Extract cold-start events from a chronologically sorted list of dicts,
    each containing:
        "time"               — RFC3339 UTC timestamp string (1-minute resolution)
        "consumption_active" — 1 if tap is on
        "recirculation_running" — 1 if recirculation pump was running at that minute

    A cold-start is the first active minute after >= cold_gap_minutes of inactivity.
    Each cold-start is assigned a combined weight = recency_weight × demand_weight,
    where demand_weight reflects how much this event is worth scheduling for:

        recirculation_running=0, run_duration < min_duration_genuine  → 0.5
            (accidental/trivial tap with cold pipes — still count, but half weight)
        recirculation_running=0, run_duration >= min_duration_genuine → 1.0
            (genuine demand with cold pipes — full weight)
        recirculation_running=1, run_duration < min_duration_recirc   → 0.0
            (too brief even with hot water present — ignore)
        recirculation_running=1, run_duration >= min_duration_recirc  → 1.0
            (genuine demand, pipes already hot from schedule — full weight,
             compensating for the artificially shortened duration)

    Run duration is measured as the number of consecutive active minutes
    following the cold-start before a gap appears.

    Returns a list of (dt_utc, combined_weight) tuples.
    """
    from datetime import timedelta

    # Build a map of timestamp → recirculation_running for fast lookup
    recirc_at = {}
    for p in points:
        recirc_at[p["time"]] = p.get("recirculation_running", 0) or 0

    # Extract only the active timestamps in order
    active_times = [p["time"] for p in points if p.get("consumption_active") == 1]

    # Build runs: groups of consecutive active minutes separated by cold gaps
    # Each run: (start_ts, [list of ts in run])
    runs = []
    current_run = []
    last_dt = None

    for ts_str in active_times:
        ts     = ts_str.rstrip("Z")
        dt_utc = datetime.fromisoformat(ts).replace(tzinfo=timezone.utc)
        if last_dt is None or (dt_utc - last_dt) >= timedelta(minutes=cold_gap_minutes):
            if current_run:
                runs.append(current_run)
            current_run = [ts_str]
        else:
            current_run.append(ts_str)
        last_dt = dt_utc
    if current_run:
        runs.append(current_run)

    cold_starts = []
    for run in runs:
        start_ts  = run[0]
        duration  = len(run)                        # buckets of continuous activity (10s each)
        recirc_on = recirc_at.get(start_ts, 0)

        if recirc_on:
            if duration < min_duration_recirc:
                demand_weight = 0.0                 # too brief, skip
            else:
                demand_weight = 1.0                 # genuine demand, pipes hot
        else:
            if duration < min_duration_genuine:
                demand_weight = 0.5                 # accidental/trivial tap
            else:
                demand_weight = 1.0                 # genuine demand, cold pipes

        if demand_weight == 0.0:
            continue

        # Cost multiplier: scale demand_weight by the dollar value of this event.
        # Cold-pipe event → scheduling recirc here would save COLD_START_WASTE_USD.
        # Recirc-hot event → scheduling recirc here has already proven useful;
        #   cost saving is proportional to the saved drain waste.
        # In both cases we normalise to [0, 1] against _COST_REF so the multiplier
        # sits naturally in the same range as the existing 0.5/1.0 demand weights.
        if recirc_on:
            # Pipes were hot — recirc worked; cost value = water saved at this tap
            cost_multiplier = min(1.0, COLD_START_WASTE_USD / _COST_REF)   # = 1.0 by definition
        else:
            # Pipes were cold — cost value = what would have been saved by recirc
            cost_multiplier = min(1.0, COLD_START_WASTE_USD / _COST_REF)   # = 1.0 for genuine
            # Short taps that didn't need hot water still get reduced cost value
            if duration < min_duration_genuine:
                cost_multiplier *= 0.5   # already half-weighted; cost also halved

        combined_weight = recency_weight * demand_weight * cost_multiplier

        ts_str2 = start_ts.rstrip("Z")
        dt_utc  = datetime.fromisoformat(ts_str2).replace(tzinfo=timezone.utc)
        cold_starts.append((dt_utc, combined_weight))

        if verbose:
            tag = ("recirc-on" if recirc_on else "cold-pipe")
            dur_sec = duration * 10
            dur_str = f"{dur_sec//60}min{dur_sec%60:02d}s" if dur_sec < 3600 else f"{dur_sec//3600}h{(dur_sec%3600)//60}min"
            print(f"  cold-start {dt_utc.strftime('%Y-%m-%d %H:%M UTC')} "
                  f"dur={dur_str} {tag} demand_w={demand_weight:.1f} "
                  f"cost_mult={cost_multiplier:.3f} combined_w={combined_weight:.2f}")

    return cold_starts


def fetch_consumption_events(args):
    """
    Returns a list of (utc_datetime, weight) tuples representing cold-start
    events: the first tap-on after ≥cold_gap_minutes of inactivity, across
    the rolling ±window_weeks seasonal band for all configured years.

    Why cold-starts only:
      Recirculation is only beneficial before a cold-start — warming pipes that
      are already warm from ongoing use wastes gas with no benefit to the user.
      By learning WHEN cold-starts occur (characteristic morning shower time,
      dinner prep, bedtime routine), we schedule recirculation to fire 5 minutes
      before each of those predicted first-tap times.

    Each year's cold-starts are weighted by recency so that this year's habits
    dominate the learned schedule.
    """
    from influxdb import InfluxDBClient

    client = InfluxDBClient(
        host=args.influxdb_host,
        port=args.influxdb_port,
        username=args.influxdb_user,
        password=args.influxdb_pass,
        database=args.influxdb_db,
    )

    today        = datetime.now(timezone.utc).date()
    weights      = args.recency_weights
    current_year = today.year

    year_weights = [(current_year - i, w) for i, w in enumerate(weights)]

    if args.verbose:
        print(f"[influx] Rolling window: ±{args.window_weeks} weeks around "
              f"{today.month:02d}-{today.day:02d}")
        print(f"[influx] Cold-start gap threshold: {args.cold_gap_minutes} minutes")
        for yr, w in year_weights:
            print(f"  Year {yr}: weight ×{w}")

    all_events = []

    for year, weight in year_weights:
        start_str, end_str = _year_window_bounds(year, today, args.window_weeks)

        # Fetch consumption_active AND recirculation_running at 10-second resolution.
        # Query in short calendar windows so each response stays under InfluxDB's
        # max-select-point limit; overlap preserves cold-gap / run detection across
        # chunk boundaries.  Results are merged by timestamp before cold-start extraction.
        if args.verbose:
            print(f"[influx] {year}: {start_str[:10]} → {end_str[:10]}")

        start_dt = _parse_influx_time_utc(start_str)
        end_dt = _parse_influx_time_utc(end_str)
        overlap = timedelta(minutes=max(args.cold_gap_minutes, 1) + 15)
        chunk_span = timedelta(days=INFLUX_QUERY_CHUNK_DAYS)

        points_by_time = {}
        cur = start_dt
        chunk_idx = 0

        try:
            while cur <= end_dt:
                chunk_end = min(cur + chunk_span, end_dt)
                cs = _format_influx_time_utc(cur)
                ce = _format_influx_time_utc(chunk_end)
                query = (
                    f"SELECT MAX(consumption_active) AS consumption_active, "
                    f"MAX(recirculation_running) AS recirculation_running "
                    f"FROM water "
                    f"WHERE time >= '{cs}' AND time <= '{ce}' "
                    f"GROUP BY time(10s) "
                    f"FILL(none) "
                    f"ORDER BY time ASC"
                )
                if args.verbose:
                    print(f"[influx]   chunk {chunk_idx}: {cs[:19]} → {ce[:19]}")
                result = client.query(query)
                for p in result.get_points():
                    points_by_time[p["time"]] = p
                chunk_idx += 1
                if chunk_end >= end_dt:
                    break
                nxt = chunk_end - overlap
                if nxt <= cur:
                    nxt = cur + timedelta(seconds=10)
                cur = nxt
        except Exception as e:
            print(f"[influx] Warning: query for {year} failed: {e}")
            continue

        points = sorted(points_by_time.values(), key=lambda p: p["time"])
        active_count = sum(1 for p in points if p.get("consumption_active") == 1)
        cold_starts = _extract_cold_starts(
            points, weight,
            cold_gap_minutes=args.cold_gap_minutes,
            min_duration_genuine=args.min_duration_genuine,
            min_duration_recirc=args.min_duration_recirc,
            verbose=args.verbose,
        )
        all_events.extend(cold_starts)

        if args.verbose:
            print(f"  → {active_count} active minutes → "
                  f"{len(cold_starts)} cold-start events")

    return all_events


# ---------------------------------------------------------------------------
# Clustering: events → per-day time windows
# ---------------------------------------------------------------------------
def events_to_minutes(events, verbose=False):
    """
    Convert a list of (utc_datetime, weight) tuples into two parallel dicts,
    both keyed by day-of-week (0=Sun..6=Sat) then by 5-minute bucket:

      raw_counts[dow][bucket]      — unweighted hit count (used for min_occurrences filter)
      weighted_scores[dow][bucket] — sum of recency weights (used for window scoring)

    Separating these lets min_occurrences filter on genuine recurrence (a pattern
    must appear regardless of which year), while scoring rewards recent years.
    """
    BIN = 5  # minutes
    raw_counts     = defaultdict(lambda: defaultdict(int))
    weighted_scores = defaultdict(lambda: defaultdict(float))

    for (dt, weight) in events:
        py_dow = dt.weekday()           # Python: Monday=0 .. Sunday=6
        dow    = (py_dow + 1) % 7      # SchedulerBase: Sunday=0 .. Saturday=6

        minute_of_day = dt.hour * 60 + dt.minute
        bucket = (minute_of_day // BIN) * BIN
        raw_counts[dow][bucket]      += 1
        weighted_scores[dow][bucket] += weight

    if verbose:
        print("\n[debug] Raw bucket hit counts per day (UTC):")
        for dow in range(7):
            hot = sorted((b, c) for b, c in raw_counts.get(dow, {}).items())
            if hot:
                times = ", ".join(
                    f"{b//60:02d}:{b%60:02d}×{c}" for b, c in hot
                )
                print(f"  {DAY_NAMES[dow]:10s}: {times}")
            else:
                print(f"  {DAY_NAMES[dow]:10s}: (no events)")

    return raw_counts, weighted_scores


def _find_peaks(score_map, min_separation, smooth_radius=2):
    """
    Find local score maxima in score_map (bucket→score dict), separated by at
    least min_separation minutes.

    Algorithm:
      1. Smooth scores with a simple sliding average over ±smooth_radius buckets
         to eliminate single-bucket spikes from noise.
      2. Scan for local maxima: a bucket is a peak if its smoothed score is
         greater than all neighbours within min_separation minutes.
      3. Use a greedy non-maximum suppression pass: sort candidates by score
         descending, accept a peak only if it is at least min_separation minutes
         from any already-accepted peak.

    Returns a list of (bucket, raw_score) tuples for accepted peaks, sorted by
    bucket time.
    """
    if not score_map:
        return []

    BIN = 5
    all_buckets = sorted(score_map)

    # Step 1: smooth scores
    smoothed = {}
    for b in all_buckets:
        neighbours = [score_map.get(b + d * BIN, 0)
                      for d in range(-smooth_radius, smooth_radius + 1)]
        smoothed[b] = sum(neighbours) / len(neighbours)

    # Step 2: find local maxima within ±min_separation
    radius_buckets = min_separation // BIN
    candidates = []
    for b in all_buckets:
        s = smoothed[b]
        if s == 0:
            continue
        is_local_max = all(
            smoothed.get(b + d * BIN, 0) <= s
            for d in range(-radius_buckets, radius_buckets + 1)
            if d != 0
        )
        if is_local_max:
            candidates.append((b, score_map[b]))   # (bucket, raw_score)

    # Step 3: greedy NMS — accept peaks in score order, suppress neighbours
    candidates.sort(key=lambda x: x[1], reverse=True)
    accepted = []
    for (b, sc) in candidates:
        if all(abs(b - a[0]) >= min_separation for a in accepted):
            accepted.append((b, sc))

    return sorted(accepted, key=lambda x: x[0])   # re-sort by time


def buckets_to_windows(raw_counts, weighted_scores, gap_minutes, min_occurrences,
                        preheat_minutes, peak_half_width=20, min_peak_separation=45,
                        min_weighted_score=DEFAULT_MIN_WEIGHTED_SCORE,
                        min_score_floor=DEFAULT_MIN_SCORE_FLOOR,
                        score_step=1.0,
                        verbose=False):
    """
    For each day-of-week, uses peak-finding to identify dominant activity clusters.

    Adaptive threshold: starts at min_weighted_score and steps down by score_step
    until MAX_SLOTS_PER_DAY peaks are found or min_score_floor is reached. This
    ensures days with irregular patterns (e.g. weekends) still produce useful slots
    rather than returning fewer slots than available, while regular weekdays with
    strong peaks satisfy the threshold immediately without relaxation.

    Returns a list of 7 dicts, each with a 'slots' list of:
        { startHour, startMinute, endHour, endMinute }
    Index 0 = Sunday, 6 = Saturday (matches SchedulerBase).
    """
    week = []

    for dow in range(7):
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})

        # Adaptive threshold loop: relax score threshold until we have enough
        # peaks or both floors are hit.
        # Phase 1: step score threshold down while keeping min_occurrences.
        # Phase 2: if score floor reached and still not enough peaks, also try
        #          min_occurrences=2 (appeared at least twice — weakest useful signal).
        threshold = min_weighted_score
        peaks = []
        final_threshold = threshold
        final_occurrences = min_occurrences

        for occ_floor in [min_occurrences, max(1, min_occurrences - 1)]:
            threshold = min_weighted_score
            while threshold >= min_score_floor:
                hot_weighted = {b: day_weighted[b]
                                for b in day_raw
                                if day_raw[b] >= occ_floor
                                and day_weighted.get(b, 0) >= threshold}

                if hot_weighted:
                    peaks = _find_peaks(hot_weighted,
                                        min_separation=min_peak_separation)

                final_threshold   = threshold
                final_occurrences = occ_floor

                if len(peaks) >= MAX_SLOTS_PER_DAY:
                    break
                if threshold <= min_score_floor:
                    break
                threshold = max(min_score_floor, threshold - score_step)

            if len(peaks) >= MAX_SLOTS_PER_DAY:
                break   # satisfied — don't relax occurrences further

        if not peaks:
            week.append({"slots": []})
            continue

        # Rank by score, keep top MAX_SLOTS_PER_DAY
        ranked = sorted(peaks, key=lambda p: p[1], reverse=True)
        kept   = ranked[:MAX_SLOTS_PER_DAY]

        if verbose:
            relaxed_parts = []
            if final_threshold < min_weighted_score:
                relaxed_parts.append(f"score→{final_threshold:.1f}")
            if final_occurrences < min_occurrences:
                relaxed_parts.append(f"occurrences→{final_occurrences}")
            relaxed = f" [relaxed: {', '.join(relaxed_parts)}]" if relaxed_parts else ""
            print(f"  [{DAY_NAMES[dow]}] {len(peaks)} peaks found "
                  f"(sep={min_peak_separation}min, ±{peak_half_width}min, "
                  f"threshold={final_threshold:.1f}, occ≥{final_occurrences}{relaxed}) "
                  f"→ keeping top {len(kept)}:")
            for p in sorted(peaks, key=lambda p: p[1], reverse=True):
                tag = " ✓" if p in kept else "  "
                t = p[0]
                print(f"    {tag} peak@{t//60:02d}:{t%60:02d}  "
                      f"score={p[1]:.1f}  "
                      f"window={max(0,t-peak_half_width)//60:02d}:"
                      f"{max(0,t-peak_half_width)%60:02d}–"
                      f"{min(1439,t+peak_half_width)//60:02d}:"
                      f"{min(1439,t+peak_half_width)%60:02d}")

        # Build ±peak_half_width windows, apply preheat, sort by time.
        # Round to the nearest 10-minute boundary so that the firmware's
        # 10-minute-resolution offset encoding (value / 6 = hour,
        # value % 6 × 10 = minute) stores the intended times exactly.
        kept_chrono = sorted(kept, key=lambda p: p[0])
        slots = []
        for (peak_bucket, _score) in kept_chrono:
            win_start = max(0,    peak_bucket - peak_half_width)
            win_end   = min(1439, peak_bucket + peak_half_width)
            start_min = max(0,    win_start   - preheat_minutes)
            start_min = round(start_min / 10) * 10
            win_end   = min(1430, round(win_end / 10) * 10)
            slots.append({
                "startHour":   start_min // 60,
                "startMinute": start_min % 60,
                "endHour":     win_end   // 60,
                "endMinute":   win_end   % 60,
            })

        week.append({"slots": slots})

    return week


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Cost/efficiency estimation for the proposed schedule
# ---------------------------------------------------------------------------
def _slot_covers_minute(slot, minute_of_day):
    """Return True if minute_of_day falls within a schedule slot."""
    slot_start = slot["startHour"] * 60 + slot["startMinute"]
    slot_end   = slot["endHour"]   * 60 + slot["endMinute"]
    return slot_start <= minute_of_day < slot_end


def _slot_near_minute(slot, minute_of_day, hot_window_min=15):
    """
    Return True if minute_of_day falls inside the slot OR within hot_window_min
    minutes after the slot ends (pipes stay hot after recirc stops).
    """
    slot_start = slot["startHour"] * 60 + slot["startMinute"]
    slot_end   = slot["endHour"]   * 60 + slot["endMinute"]
    return slot_start <= minute_of_day < slot_end + hot_window_min


def _score_day(day_raw, day_weighted, slots, historical_days=16, hot_window_min=15):
    """
    Reframed efficiency: of the cold-starts that are *schedulable*
    (fall within a slot OR within hot_window_min minutes after slot end),
    what fraction did the schedule actually cover (i.e. fell inside the slot)?

    All raw counts are normalised by historical_days to give per-day expectations.
    Returns a dict of per-day metrics.
    """
    all_buckets = sorted(day_raw.keys())

    schedulable_raw = 0
    covered_raw     = 0

    for b in all_buckets:
        near   = any(_slot_near_minute(s, b, hot_window_min) for s in slots)
        inside = any(_slot_covers_minute(s, b) for s in slots)
        r = day_raw[b]
        if near:
            schedulable_raw += r
        if inside:
            covered_raw += r

    # Wasted recirc cycles: 15-min sub-windows inside slots with no demand
    wasted_recirc_cycles = 0
    for slot in slots:
        slot_start = slot["startHour"] * 60 + slot["startMinute"]
        slot_end   = slot["endHour"]   * 60 + slot["endMinute"]
        for m in range(slot_start, slot_end, 15):
            has_demand = any(
                m <= b < m + 15
                for b in all_buckets
                if day_weighted.get(b, 0) > 0
            )
            if not has_demand:
                wasted_recirc_cycles += 1

    # Normalise to per-day expectations
    schedulable_per_day = schedulable_raw / historical_days
    covered_per_day     = covered_raw     / historical_days
    missed_per_day      = (schedulable_raw - covered_raw) / historical_days

    # Reframed efficiency: of schedulable demand, fraction covered
    efficiency = (covered_per_day / schedulable_per_day * 100.0
                  if schedulable_per_day > 0 else None)

    # Dollar costs
    missed_waste = missed_per_day       * COLD_START_WASTE_USD
    gas_waste    = wasted_recirc_cycles * RECIRC_WASTE_USD
    total_waste  = missed_waste + gas_waste

    return {
        "efficiency_pct":       efficiency,
        "schedulable_per_day":  round(schedulable_per_day, 1),
        "covered_per_day":      round(covered_per_day, 1),
        "missed_per_day":       round(missed_per_day, 1),
        "wasted_recirc_cycles": wasted_recirc_cycles,
        "missed_waste_usd":     missed_waste,
        "gas_waste_usd":        gas_waste,
        "total_waste_usd":      total_waste,
    }


def estimate_schedule_cost(week, raw_counts, weighted_scores,
                           historical_days=16, hot_window_min=15):
    """
    For each day of the coming week, compute reframed efficiency.
    Returns a list of 7 dicts (index 0 = Sunday).
    """
    from datetime import timedelta
    today = datetime.now(timezone.utc).date()
    coming_week = []
    for offset in range(7):
        d      = today + timedelta(days=offset)
        py_dow = d.weekday()
        dow    = (py_dow + 1) % 7
        coming_week.append((d, dow))

    results = []
    for (d, dow) in coming_week:
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})
        slots        = week[dow]["slots"]
        m = _score_day(day_raw, day_weighted, slots, historical_days, hot_window_min)
        results.append({"date": d, "dow": dow, **m})
    return results


def compare_slot_widths(raw_counts, weighted_scores, args,
                        widths=(25, 30), historical_days=16, hot_window_min=15):
    """
    Build schedules at each peak_half_width and print a side-by-side coverage
    comparison so the user can choose the best slot width.
    Returns a dict of {width: week} for both widths.
    """
    print("\n=== Slot Width Comparison (±25min vs ±30min windows) ===")
    col = "  {:>14}  {:>9}  {:>9}"
    header = f"  {'Day':<10}" + "".join(
        col.format(f"±{w}min Effic%", "Sched/day", "Covrd/day") for w in widths
    )
    print(header)
    print("  " + "-" * (len(header) - 2))

    weeks = {}
    for w in widths:
        weeks[w] = buckets_to_windows(
            raw_counts, weighted_scores,
            gap_minutes=args.gap_minutes,
            min_occurrences=args.min_occurrences,
            preheat_minutes=args.preheat_minutes,
            peak_half_width=w,
            min_peak_separation=args.min_peak_separation,
            min_weighted_score=args.min_weighted_score,
            min_score_floor=args.min_score_floor,
        )

    for dow in range(7):
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})
        row = f"  {DAY_NAMES[dow]:<10}"
        for w in widths:
            slots = weeks[w][dow]["slots"]
            m = _score_day(day_raw, day_weighted, slots, historical_days, hot_window_min)
            eff = f"{m['efficiency_pct']:.1f}%" if m["efficiency_pct"] is not None else "N/A"
            row += col.format(eff, f"{m['schedulable_per_day']:.1f}", f"{m['covered_per_day']:.1f}")
        print(row)
    print()
    return weeks


# ---------------------------------------------------------------------------
# Pretty-print the schedule for human review
# ---------------------------------------------------------------------------
def print_schedule(week, raw_counts=None, weighted_scores=None, verbose=False,
                   historical_days=16, hot_window_min=15, args=None):
    print("\n=== Learned Recirculation Schedule ===")
    for dow, day in enumerate(week):
        slots = day["slots"]
        if not slots:
            print(f"  {DAY_NAMES[dow]:10s}  (no scheduled windows)")
        else:
            slot_strs = [
                f"{s['startHour']:02d}:{s['startMinute']:02d}"
                f"–{s['endHour']:02d}:{s['endMinute']:02d}"
                for s in slots
            ]
            print(f"  {DAY_NAMES[dow]:10s}  {',  '.join(slot_strs)}")
    print()

    if verbose and raw_counts is not None and weighted_scores is not None:
        # Show slot width comparison first
        if args is not None:
            compare_slot_widths(raw_counts, weighted_scores, args,
                                widths=(25, 30),
                                historical_days=historical_days,
                                hot_window_min=hot_window_min)

        # Then show coming-week efficiency for the selected schedule
        cost_results = estimate_schedule_cost(
            week, raw_counts, weighted_scores, historical_days, hot_window_min)
        print(f"=== Expected Efficiency for Coming Week ===")
        print(f"  Efficiency = schedulable cold-starts covered / all schedulable")
        print(f"  Schedulable = cold-starts falling inside a slot or within "
              f"{hot_window_min}min after slot end")
        print()
        hdr = (f"  {'Date':<12} {'Day':<10} {'Effic%':>7} {'Sched/d':>8} "
               f"{'Covrd/d':>8} {'Misd/d':>7} {'MissWst$':>9} {'GasWst$':>8} "
               f"{'TotWst$':>8} {'WstCyc':>7}")
        print(hdr)
        print("  " + "-" * (len(hdr) - 2))
        total_waste = 0.0
        for r in cost_results:
            eff = f"{r['efficiency_pct']:.1f}%" if r["efficiency_pct"] is not None else "  N/A"
            print(f"  {r['date'].strftime('%Y-%m-%d'):<12} "
                  f"{DAY_NAMES[r['dow']]:<10} "
                  f"{eff:>7} "
                  f"{r['schedulable_per_day']:>8.1f} "
                  f"{r['covered_per_day']:>8.1f} "
                  f"{r['missed_per_day']:>7.1f} "
                  f"${r['missed_waste_usd']:>8.4f} "
                  f"${r['gas_waste_usd']:>7.4f} "
                  f"${r['total_waste_usd']:>7.4f} "
                  f"{r['wasted_recirc_cycles']:>7}")
            total_waste += r["total_waste_usd"]
        print(f"  {'':12} {'':10} {'':>7} {'':>8} {'':>8} {'':>7} "
              f"{'':>9} {'':>8} ${total_waste:>7.4f}")
        print()


# Push to ESP32
# ---------------------------------------------------------------------------
def push_schedule(week, args):
    import requests

    url = f"http://{args.esp32_host}:{args.esp32_port}/schedule"
    payload = {"schedule": week}
    body    = json.dumps(payload)

    if args.verbose:
        print(f"[push] POST {url}")
        print(f"[push] Body: {body}")

    try:
        resp = requests.post(
            url,
            data=body,
            headers={"Content-Type": "application/json"},
            timeout=10,
        )
        if resp.status_code == 200:
            print(f"[push] Schedule accepted by ESP32: {resp.text.strip()}")
        else:
            print(f"[push] ESP32 returned HTTP {resp.status_code}: {resp.text.strip()}")
            sys.exit(1)
    except requests.exceptions.ConnectionError as e:
        print(f"[push] Could not reach ESP32 at {url}: {e}")
        sys.exit(1)
    except requests.exceptions.Timeout:
        print(f"[push] Timed out connecting to ESP32 at {url}")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Learn a recirculation schedule from InfluxDB and push to Navien ESP32",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    # InfluxDB
    parser.add_argument("--influxdb_host",  default=DEFAULT_INFLUXDB_HOST)
    parser.add_argument("--influxdb_port",  default=DEFAULT_INFLUXDB_PORT, type=int)
    parser.add_argument("--influxdb_user",  default=None)
    parser.add_argument("--influxdb_pass",  default=None)
    parser.add_argument("--influxdb_db",    default=DEFAULT_INFLUXDB_DB)

    # Learning parameters
    parser.add_argument("--window_weeks",    default=DEFAULT_WINDOW_WEEKS,    type=int,
                        help="Half-width of rolling seasonal window in weeks "
                             "(queries ±window_weeks around today's date in each year)")
    parser.add_argument("--recency_weights", default=DEFAULT_RECENCY_WEIGHTS, type=int,
                        nargs="+", metavar="W",
                        help="Per-year recency multipliers, most-recent first "
                             "(e.g. --recency_weights 3 2 1 for 3 years of data)")
    parser.add_argument("--cold_gap_minutes", default=DEFAULT_COLD_GAP, type=int,
                        help="Inactivity gap in minutes after which pipes are considered "
                             "cold (default: 10). Only the first tap-on after this gap "
                             "is counted as a cold-start event worth scheduling for.")
    parser.add_argument("--min_duration_genuine", default=DEFAULT_MIN_DURATION_GENUINE,
                        type=int,
                        help="Minimum tap-on duration in 10-second buckets (cold pipes) "
                             "to count as genuine demand with full weight (default: 6 = 1min). "
                             "Shorter taps get half weight (0.5).")
    parser.add_argument("--min_duration_recirc", default=DEFAULT_MIN_DURATION_RECIRC,
                        type=int,
                        help="Minimum tap-on duration in 10-second buckets when recirculation "
                             "is running to count the event at all (default: 3 = 30sec). "
                             "Shorter taps during recirculation are discarded entirely.")
    parser.add_argument("--preheat_minutes", default=DEFAULT_PREHEAT_MINUTES, type=int,
                        help="Minutes before first predicted demand to start recirc")
    parser.add_argument("--gap_minutes",     default=DEFAULT_GAP_MINUTES,     type=int,
                        help="Merge demand events closer than this into one window")
    parser.add_argument("--min_occurrences", default=DEFAULT_MIN_OCCURRENCES, type=int,
                        help="Minimum times a window must appear to be scheduled")

    # ESP32
    parser.add_argument("--esp32_host",  default=DEFAULT_ESP32_HOST,
                        help="Hostname or IP of the ESP32 (mDNS or static)")
    parser.add_argument("--esp32_port",  default=DEFAULT_ESP32_PORT, type=int)

    # Actions
    parser.add_argument("--push",    action="store_true",
                        help="Push the schedule to the ESP32 (dry-run by default)")
    parser.add_argument("--dry_run", action="store_true",
                        help="Print schedule but do not push (default behaviour)")
    parser.add_argument("--debug_day", default=None,
                        help="Show detailed bucket breakdown for one day before merging "
                             "(e.g. --debug_day Monday). Implies dry-run.")
    parser.add_argument("--min_weighted_score", default=DEFAULT_MIN_WEIGHTED_SCORE,
                        type=float,
                        help="Minimum weighted score for a bucket to qualify — "
                             "filters buckets that only appeared in older/less-weighted "
                             "years (default: 6.0)")
    parser.add_argument("--min_score_floor",    default=DEFAULT_MIN_SCORE_FLOOR,
                        type=float,
                        help="Lowest score threshold the adaptive algorithm will "
                             "relax to when a day has fewer than 4 peaks (default: 3.0). "
                             "Raise to keep schedules tighter; lower to accept weaker patterns.")
    parser.add_argument("--peak_half_width",    default=30,  type=int,
                        help="Half-width in minutes of the window built around each "
                             "activity peak (default: 20 → ±20min = 40min total window)")
    parser.add_argument("--min_peak_separation", default=45, type=int,
                        help="Minimum minutes between two accepted peaks — prevents "
                             "finding two peaks inside the same activity cluster "
                             "(default: 45)")
    parser.add_argument("-v", "--verbose", action="store_true")

    args = parser.parse_args()

    # Step 1: Fetch cold-start events from InfluxDB (rolling window, all years)
    today = datetime.now(timezone.utc).date()
    print(f"Querying InfluxDB ({args.influxdb_host}:{args.influxdb_port}/{args.influxdb_db}) "
          f"— rolling ±{args.window_weeks}-week window around "
          f"{today.strftime('%b %d')}, years weighted {args.recency_weights}...")
    print(f"Extracting cold-start events (first tap-on after "
          f"≥{args.cold_gap_minutes}min inactivity, "
          f"genuine threshold: {args.min_duration_genuine} buckets cold / "
          f"{args.min_duration_recirc} buckets with recirc "
          f"[10s resolution: ×6=min, ×3=30s])...")
    events = fetch_consumption_events(args)

    if not events:
        print("No cold-start events found. Check your InfluxDB connection and database name.")
        sys.exit(1)

    dts = [e[0] for e in events]
    print(f"Found {len(events)} cold-start events across "
          f"{len(args.recency_weights)} year(s) of seasonal data.")
    if args.verbose:
        print(f"  First event (UTC): {dts[0].strftime('%Y-%m-%d %H:%M %Z')}")
        print(f"  Last event  (UTC): {dts[-1].strftime('%Y-%m-%d %H:%M %Z')}")

    # Step 2: Bin into per-day-of-week buckets (raw counts + weighted scores)
    raw_counts, weighted_scores = events_to_minutes(events, verbose=args.verbose)

    # Optional: detailed single-day breakdown to help tune parameters
    if args.debug_day:
        day_name = args.debug_day.strip().capitalize()
        if day_name not in DAY_NAMES:
            print(f"Unknown day '{args.debug_day}'. Choose from: {', '.join(DAY_NAMES)}")
            sys.exit(1)
        dow = DAY_NAMES.index(day_name)
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})
        total_years  = len(args.recency_weights)
        _today = datetime.now(timezone.utc).date()
        print(f"\n=== Debug: {day_name} (±{args.window_weeks}wk window around "
              f"{_today.strftime('%b %d')}, {total_years} years, "
              f"weights={args.recency_weights}) ===")
        print(f"  min_occurrences={args.min_occurrences}  "
              f"gap_minutes={args.gap_minutes}  "
              f"preheat_minutes={args.preheat_minutes}")
        # Pre-compute the effective threshold/occurrences so it can be shown in the header
        _eff_t   = args.min_weighted_score
        _eff_occ = args.min_occurrences
        for _occ in [args.min_occurrences, max(1, args.min_occurrences - 1)]:
            _t = args.min_weighted_score
            while _t >= args.min_score_floor:
                _hw = {b: weighted_scores.get(dow, {}).get(b, 0)
                       for b in raw_counts.get(dow, {})
                       if raw_counts.get(dow, {}).get(b, 0) >= _occ
                       and weighted_scores.get(dow, {}).get(b, 0) >= _t}
                if len(_find_peaks(_hw, args.min_peak_separation)) >= MAX_SLOTS_PER_DAY:
                    _eff_t = _t; _eff_occ = _occ; break
                if _t <= args.min_score_floor:
                    _eff_t = _t; _eff_occ = _occ; break
                _t = max(args.min_score_floor, _t - 1.0)
            if _eff_occ < args.min_occurrences or len(_find_peaks(
                    {b: weighted_scores.get(dow, {}).get(b, 0)
                     for b in raw_counts.get(dow, {})
                     if raw_counts.get(dow, {}).get(b, 0) >= _eff_occ
                     and weighted_scores.get(dow, {}).get(b, 0) >= _eff_t},
                    args.min_peak_separation)) >= MAX_SLOTS_PER_DAY:
                break
        effective_thresh_display = _eff_t
        _relaxed_parts = []
        if _eff_t   < args.min_weighted_score:  _relaxed_parts.append(f"score→{_eff_t:.1f}")
        if _eff_occ < args.min_occurrences:     _relaxed_parts.append(f"occ→{_eff_occ}")
        relaxed_tag = f" [relaxed: {', '.join(_relaxed_parts)}]" if _relaxed_parts else ""
        print(f"\n  All buckets (time × raw_count [weighted_score]) "
              f"— threshold: {args.min_occurrences} raw hits AND "
              f"{effective_thresh_display:.1f} wtd score{relaxed_tag}:")
        all_buckets = sorted(set(day_raw) | set(day_weighted))
        if not all_buckets:
            print("    (no events at all on this day)")
        for b in all_buckets:
            c  = day_raw.get(b, 0)
            ws = day_weighted.get(b, 0)
            bar    = "█" * min(c, 20)
            marker = (" ← INCLUDED"
                      if c >= _eff_occ and ws >= effective_thresh_display
                      else " ← filtered out")
            print(f"    {b//60:02d}:{b%60:02d}  {bar:20s} ({c} raw, {ws:.1f} wtd){marker}")
        # Show peak-finding results using same logic as full run
        passing = sorted(b for b in day_raw
                         if day_raw[b] >= _eff_occ
                         and day_weighted.get(b, 0) >= effective_thresh_display)
        print(f"\n  {len(passing)} buckets pass the threshold:")
        if passing:
            # Use the pre-computed effective threshold/occurrences
            effective_thresh = effective_thresh_display
            hot_weighted = {b: day_weighted.get(b, 0) for b in day_raw
                            if day_raw[b] >= _eff_occ
                            and day_weighted.get(b, 0) >= effective_thresh_display}
            peaks = _find_peaks(hot_weighted,
                                min_separation=args.min_peak_separation)
            ranked_peaks = sorted(peaks, key=lambda p: p[1], reverse=True)
            kept_debug   = ranked_peaks[:MAX_SLOTS_PER_DAY]

            print(f"  (peak-finding: sep={args.min_peak_separation}min, "
                  f"±{args.peak_half_width}min window, "
                  f"preheat={args.preheat_minutes}min → {len(peaks)} peaks)")
            print(f"  {'Rank':<5} {'Peak':<8} {'Window (with preheat)':<24} "
                  f"{'Score':>8}  {'Selected'}")
            print(f"  {'-'*4} {'-'*7} {'-'*23} {'-'*8}  {'-'*8}")
            for i, (b, sc) in enumerate(ranked_peaks):
                win_start = max(0,    b - args.peak_half_width)
                win_end   = min(1439, b + args.peak_half_width)
                start_pre = max(0,    win_start - args.preheat_minutes)
                in_kept = any(k[0]==b for k in kept_debug)
                tag = "✓ keep" if in_kept else "  drop"
                print(f"  {i+1:<5} "
                      f"{b//60:02d}:{b%60:02d}   "
                      f"{start_pre//60:02d}:{start_pre%60:02d}–"
                      f"{win_end//60:02d}:{win_end%60:02d}            "
                      f"{sc:>8.1f}   {tag}")
        print("  (Re-run without --debug_day to see the full merged schedule)\n")
        sys.exit(0)

    # Step 3: Find activity peaks and build windows around them
    week = buckets_to_windows(
        raw_counts,
        weighted_scores,
        gap_minutes=args.gap_minutes,
        min_occurrences=args.min_occurrences,
        preheat_minutes=args.preheat_minutes,
        peak_half_width=args.peak_half_width,
        min_peak_separation=args.min_peak_separation,
        min_weighted_score=args.min_weighted_score,
        min_score_floor=args.min_score_floor,
        verbose=args.verbose,
    )

    # Step 4: Print for review (--verbose adds slot comparison + efficiency table)
    print_schedule(week, raw_counts=raw_counts, weighted_scores=weighted_scores,
                   verbose=args.verbose, args=args)

    # Step 5: Push (unless dry run)
    if args.push and not args.dry_run:
        push_schedule(week, args)
    else:
        print("[dry-run] Not pushing. Pass --push to send to ESP32.")
        print("[dry-run] JSON that would be sent:")
        print(json.dumps({"schedule": week}, indent=2))


if __name__ == "__main__":
    main()