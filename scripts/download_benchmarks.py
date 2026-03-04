#!/usr/bin/env python3
"""Self-contained parallel downloader for SAT Competition benchmarks.

Downloads from benchmark-database.de. Each year in benchmarks/sc2023/, benchmarks/sc2024/
with instances.csv. Only .cnf is kept; .xz is deleted after decompression.

Features:
  - Auto-verify CNF integrity (last line ends with ' 0')
  - Delete .xz after successful decompress (save ~50% disk)
  - Generate track_main_{year}.uri for each year

Examples:
    python scripts/download_benchmarks.py                    # SC2023 + SC2024
    python scripts/download_benchmarks.py --year 2023 -j 16
    python scripts/download_benchmarks.py --year all --bg
    python scripts/download_benchmarks.py --verify           # Re-check existing, re-dl corrupt
    python scripts/download_benchmarks.py --cleanup          # Remove .cache, download.log
"""

import argparse
import csv
import gzip
import json
import logging
import lzma
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from html.parser import HTMLParser
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = PROJECT_ROOT / "benchmarks"
CACHE_DIR = BENCH_DIR / ".cache"

# ── Embedded source URLs ────────────────────────────────────────────────────
BENCHMARK_DB = "https://benchmark-database.de"

TRACKS = {
    "2023": f"{BENCHMARK_DB}/?track=main_2023",
    "2024": f"{BENCHMARK_DB}/?track=main_2024",
}

FILE_URL = f"{BENCHMARK_DB}/file/"      # + {hash}
USER_AGENT = "SAT-Parallel-Downloader/2.0"
DOWNLOAD_TIMEOUT = 300
MAX_RETRIES = 2

log = logging.getLogger("download_benchmarks")


# ── HTML table parser ───────────────────────────────────────────────────────
# benchmark-database.de returns an HTML table with columns:
#   hash | isohash | family | author | result | proceedings | minisat1m | filename | track

class _TableParser(HTMLParser):
    """Extract instance metadata from benchmark-database.de HTML table."""

    def __init__(self):
        super().__init__()
        self.instances: list[dict] = []
        self._in_row = False
        self._in_cell = False
        self._cells: list[str] = []
        self._cell_buf = ""
        self._cell_link = ""

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self._in_row = True
            self._cells = []
            self._cell_link = ""
        elif tag in ("td", "th") and self._in_row:
            self._in_cell = True
            self._cell_buf = ""
        elif tag == "a" and self._in_cell:
            href = dict(attrs).get("href", "")
            if "/file/" in href:
                self._cell_link = href

    def handle_data(self, data):
        if self._in_cell:
            self._cell_buf += data

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self._in_cell:
            self._in_cell = False
            self._cells.append(self._cell_buf.strip())
        elif tag == "tr" and self._in_row:
            self._in_row = False
            self._emit_row()
            self._cell_link = ""

    def _emit_row(self):
        if not self._cell_link or len(self._cells) < 8:
            return
        m = re.search(r"/file/([a-f0-9]{32})", self._cell_link)
        if not m:
            return
        result = self._cells[4].strip().lower() if len(self._cells) > 4 else ""
        if result not in ("sat", "unsat"):
            result = "unknown"
        self.instances.append({
            "hash": m.group(1),
            "filename": self._cells[7],
            "family": self._cells[2],
            "author": self._cells[3],
            "result": result,
        })


# ── Metadata fetching ───────────────────────────────────────────────────────

def _fetch_from_web(year: str) -> list[dict]:
    url = TRACKS[year]
    log.info("Fetching SC%s instance list from %s ...", year, url)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=120) as resp:
        html = resp.read().decode("utf-8", errors="replace")
    parser = _TableParser()
    parser.feed(html)
    log.info("Parsed %d instances from SC%s", len(parser.instances), year)
    return parser.instances


def _fetch_from_local(year: str) -> list[dict]:
    """Fallback: load from local metadata files left by setup_env.sh."""
    meta_csv = BENCH_DIR / f"sc{year}-results" / "bench_meta.csv"
    uri_file = BENCH_DIR / f"track_main_{year}.uri"
    results_csv = BENCH_DIR / f"sc{year}-results" / "results_main_detailed.csv"

    if not meta_csv.exists() or not uri_file.exists():
        return []

    meta = {}
    with open(meta_csv) as f:
        for row in csv.DictReader(f):
            meta[row["hash"]] = row

    expected = {}
    if results_csv.exists():
        with open(results_csv) as f:
            for row in csv.DictReader(f):
                v = row.get("vresult", "").strip().lower()
                expected[row["hash"]] = v if v in ("sat", "unsat") else "unknown"

    hashes = []
    with open(uri_file) as f:
        for line in f:
            u = line.strip()
            if u:
                hashes.append(u.rsplit("/", 1)[-1])

    instances = []
    for h in hashes:
        if h in meta:
            instances.append({
                "hash": h,
                "filename": meta[h]["filename"],
                "family": meta[h].get("family", ""),
                "author": meta[h].get("author", ""),
                "result": expected.get(h, "unknown"),
            })
    log.info("Loaded %d instances from local SC%s metadata", len(instances), year)
    return instances


def fetch_track(year: str) -> list[dict]:
    cache_file = CACHE_DIR / f"main_{year}.json"

    if cache_file.exists():
        age_h = (time.time() - cache_file.stat().st_mtime) / 3600
        if age_h < 7 * 24:
            with open(cache_file) as f:
                data = json.load(f)
            log.info("Using cached SC%s metadata (%d instances, %.0fh old)", year, len(data), age_h)
            return data

    try:
        instances = _fetch_from_web(year)
    except Exception as e:
        log.warning("Web fetch failed for SC%s: %s — trying local fallback", year, e)
        instances = _fetch_from_local(year)

    if not instances:
        log.error("No instances found for SC%s", year)
        return []

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    with open(cache_file, "w") as f:
        json.dump(instances, f)

    return instances


# ── Integrity verification ──────────────────────────────────────────────────

def _verify_cnf_tail(path: Path) -> tuple[bool, str]:
    """Verify last line of CNF ends with ' 0'. Return (ok, msg)."""
    try:
        with open(path, "rb") as f:
            f.seek(max(0, path.stat().st_size - 256))
            tail = f.read().decode("utf-8", errors="replace")
        lines = [l.strip() for l in tail.rsplit("\n", 2) if l.strip()]
        if not lines:
            return False, "empty or no complete line"
        last = lines[-1]
        if last.endswith(" 0") or (last.endswith("0") and " " in last):
            return True, ""
        return False, f"last line does not end with ' 0': ...{last[-40:]}"
    except Exception as e:
        return False, str(e)[:80]


# ── Download + decompress ───────────────────────────────────────────────────

def _decompress(src: Path, dst: Path):
    if src.suffix == ".xz":
        with lzma.open(src, "rb") as fi, open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo, length=1 << 20)
    elif src.suffix == ".gz":
        with gzip.open(src, "rb") as fi, open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo, length=1 << 20)


def _cnf_name(filename: str) -> str:
    name = filename
    for ext in (".xz", ".gz"):
        if name.endswith(ext):
            name = name[: -len(ext)]
    return name


def _verify_and_cleanup(compressed: Path, cnf_path: Path, delete_compressed: bool) -> tuple[bool, str]:
    """Verify CNF, optionally delete compressed. Return (ok, msg)."""
    ok, msg = _verify_cnf_tail(cnf_path)
    if not ok:
        cnf_path.unlink(missing_ok=True)
        compressed.unlink(missing_ok=True)
        return False, msg
    if delete_compressed and compressed.exists():
        compressed.unlink(missing_ok=True)
    return True, ""


def download_one(
    inst: dict,
    dest_dir: Path,
    *,
    verify_existing: bool = False,
    delete_xz: bool = True,
) -> tuple[dict, str | None, str, str | None]:
    """Returns (inst, cnf_path, status, error).  status ∈ {skipped, downloaded, failed, re-downloaded}."""
    filename = inst["filename"]
    compressed = dest_dir / filename
    cnf_path = dest_dir / _cnf_name(filename)

    if cnf_path.exists():
        if verify_existing:
            ok, msg = _verify_cnf_tail(cnf_path)
            if not ok:
                cnf_path.unlink(missing_ok=True)
                compressed.unlink(missing_ok=True)
                # Fall through to re-download
            else:
                if compressed.exists() and delete_xz:
                    compressed.unlink(missing_ok=True)
                return (inst, str(cnf_path), "skipped", None)
        else:
            if compressed.exists() and delete_xz:
                compressed.unlink(missing_ok=True)
            return (inst, str(cnf_path), "skipped", None)

    if compressed.exists():
        try:
            _decompress(compressed, cnf_path)
            ok, msg = _verify_and_cleanup(compressed, cnf_path, delete_xz)
            if ok:
                return (inst, str(cnf_path), "skipped", None)
            # Verify failed, fall through to re-download
            last_err = msg
        except Exception as e:
            compressed.unlink(missing_ok=True)
            last_err = str(e)
    else:
        last_err = ""

    url = FILE_URL + inst["hash"]
    for attempt in range(MAX_RETRIES + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
                with open(compressed, "wb") as f:
                    shutil.copyfileobj(resp, f, length=1 << 20)

            if compressed.suffix in (".xz", ".gz"):
                _decompress(compressed, cnf_path)
                ok, msg = _verify_and_cleanup(compressed, cnf_path, delete_xz)
            else:
                if compressed != cnf_path:
                    shutil.move(str(compressed), str(cnf_path))
                ok, msg = _verify_cnf_tail(cnf_path)

            if ok:
                return (inst, str(cnf_path), "downloaded", None)
            last_err = msg
            cnf_path.unlink(missing_ok=True)
        except Exception as e:
            last_err = str(e)
            compressed.unlink(missing_ok=True)
        if attempt < MAX_RETRIES:
            time.sleep(2 ** attempt)

    compressed.unlink(missing_ok=True)
    return (inst, None, "failed", last_err)


# ── Integrity check (check-only mode) ────────────────────────────────────────

def _run_integrity_check():
    """Verify all CNF files, print report."""
    years = ["2023", "2024"]
    stats = {"ok": 0, "missing": 0, "corrupt": 0}
    issues = []

    for year in years:
        csv_path = BENCH_DIR / f"sc{year}" / "instances.csv"
        if not csv_path.exists():
            log.warning("SKIP: %s not found", csv_path)
            continue
        dest_dir = BENCH_DIR / f"sc{year}"
        with open(csv_path) as f:
            for row in csv.DictReader(f):
                cnf_name = _cnf_name(row["filename"])
                cnf_path = dest_dir / cnf_name
                if not cnf_path.exists():
                    stats["missing"] += 1
                    issues.append((year, cnf_name, "MISSING"))
                    continue
                ok, msg = _verify_cnf_tail(cnf_path)
                if ok:
                    stats["ok"] += 1
                else:
                    stats["corrupt"] += 1
                    issues.append((year, cnf_name, msg))

    log.info("=" * 60)
    log.info("  Integrity Report: OK=%d  Missing=%d  Corrupt=%d",
             stats["ok"], stats["missing"], stats["corrupt"])
    log.info("=" * 60)
    if issues:
        for year, name, msg in issues[:20]:
            log.warning("  [%s] %s: %s", year, name, msg[:60] if msg != "MISSING" else msg)
        if len(issues) > 20:
            log.warning("  ... and %d more", len(issues) - 20)
    r = subprocess.run(["du", "-sh", str(BENCH_DIR)], capture_output=True, text=True)
    if r.returncode == 0:
        log.info("  Disk: %s", r.stdout.strip())
    sys.exit(1 if issues else 0)


# ── Progress tracker ────────────────────────────────────────────────────────

class _Progress:
    def __init__(self, total: int):
        self.total = total
        self.done = 0
        self.downloaded = 0
        self.skipped = 0
        self.failed = 0
        self._lock = threading.Lock()

    def update(self, status: str) -> tuple[int, int, int, int]:
        with self._lock:
            self.done += 1
            if status == "downloaded":
                self.downloaded += 1
            elif status == "skipped":
                self.skipped += 1
            else:
                self.failed += 1
            return self.done, self.downloaded, self.skipped, self.failed


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Parallel SAT Competition benchmark downloader (SC2023/SC2024)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--year", default="all", choices=["2023", "2024", "all"],
                        help="Competition year (default: all)")
    parser.add_argument("-j", "--workers", type=int, default=8,
                        help="Parallel download threads (default: 8)")
    parser.add_argument("--limit", type=int, default=None,
                        help="Max instances per year")
    parser.add_argument("--bg", action="store_true",
                        help="Run in background, log to benchmarks/download.log")
    parser.add_argument("--force-refresh", action="store_true",
                        help="Ignore cached metadata, re-fetch from web")
    parser.add_argument("--verify", action="store_true",
                        help="Verify existing CNF files, re-download if corrupt")
    parser.add_argument("--no-delete-xz", action="store_true",
                        help="Keep .xz after decompress (default: delete to save space)")
    parser.add_argument("--cleanup", action="store_true",
                        help="Remove .cache and download.log at end")
    parser.add_argument("--check-only", action="store_true",
                        help="Only verify existing CNF integrity, no download")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    # ── Check-only mode: verify integrity, print report, exit ──
    if args.check_only:
        _run_integrity_check()
        return

    # ── Background mode: relaunch self as detached subprocess ──
    if args.bg:
        BENCH_DIR.mkdir(parents=True, exist_ok=True)
        log_path = BENCH_DIR / "download.log"
        cmd = [sys.executable] + [a for a in sys.argv if a != "--bg"]
        with open(log_path, "w") as lf:
            proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        print(f"Background download started  PID={proc.pid}")
        print(f"Log : {log_path}")
        print(f"Monitor : tail -f {log_path}")
        sys.exit(0)

    if args.force_refresh:
        for f in CACHE_DIR.glob("*.json"):
            f.unlink()

    years = ["2023", "2024"] if args.year == "all" else [args.year]

    fieldnames = ["hash", "filename", "cnf_path", "expected", "family", "author"]

    for year in years:
        year_dir = BENCH_DIR / f"sc{year}"
        year_dir.mkdir(parents=True, exist_ok=True)
        instances_csv = year_dir / "instances.csv"

        insts = fetch_track(year)
        if args.limit:
            insts = insts[: args.limit]
        log.info("SC%s: %d instances → %s", year, len(insts), year_dir)
        delete_xz = not args.no_delete_xz

        progress = _Progress(len(insts))
        results: list[dict] = []
        results_lock = threading.Lock()
        t0 = time.monotonic()

        def _task(inst):
            return download_one(
                inst, year_dir,
                verify_existing=args.verify,
                delete_xz=delete_xz,
            )

        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = {pool.submit(_task, inst): inst for inst in insts}

            for future in as_completed(futures):
                inst, cnf_path, status, error = future.result()
                done, dl, sk, fl = progress.update(status)

                if status == "failed":
                    log.warning("[%d/%d] FAIL %-45s %s",
                                done, progress.total, inst["filename"][:45], error)
                elif status == "downloaded":
                    log.info("[%d/%d]  DL  %-45s",
                             done, progress.total, inst["filename"][:45])

                if cnf_path:
                    with results_lock:
                        results.append({
                            "hash": inst["hash"],
                            "filename": inst["filename"],
                            "cnf_path": str(Path(cnf_path).relative_to(PROJECT_ROOT)),
                            "expected": inst["result"],
                            "family": inst["family"],
                            "author": inst["author"],
                        })

                if done % 50 == 0:
                    elapsed = time.monotonic() - t0
                    log.info("--- progress: %d/%d  (DL=%d SKIP=%d FAIL=%d)  %.0fs ---",
                             done, progress.total, dl, sk, fl, elapsed)

        elapsed = time.monotonic() - t0

        results.sort(key=lambda r: r["filename"])
        with open(instances_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)

        track_uri = BENCH_DIR / f"track_main_{year}.uri"
        uris = [f"{FILE_URL}{r['hash']}" for r in results]
        track_uri.write_text("\n".join(uris) + "\n")
        log.info("  Track URI  : %s", track_uri)

        sat_n = sum(1 for r in results if r["expected"] == "sat")
        unsat_n = sum(1 for r in results if r["expected"] == "unsat")

        log.info("=" * 60)
        log.info("SC%s completed in %.0fs", year, elapsed)
        log.info("  Downloaded : %d", progress.downloaded)
        log.info("  Skipped    : %d  (already existed)", progress.skipped)
        log.info("  Failed     : %d", progress.failed)
        log.info("  Indexed    : %d  (SAT=%d  UNSAT=%d  unknown=%d)",
                 len(results), sat_n, unsat_n, len(results) - sat_n - unsat_n)
        log.info("  Output     : %s", instances_csv)

    if args.cleanup:
        for path in [CACHE_DIR, BENCH_DIR / "download.log"]:
            if path.exists():
                if path.is_dir():
                    shutil.rmtree(path)
                    log.info("Removed: %s", path)
                else:
                    path.unlink()
                    log.info("Removed: %s", path)


if __name__ == "__main__":
    main()
