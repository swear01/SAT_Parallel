#!/usr/bin/env python3
"""Self-contained parallel downloader for SAT Competition benchmarks.

Downloads from benchmark-database.de. No external metadata files needed.

Examples:
    python scripts/download_benchmarks.py                          # SC2023 + SC2024, 8 workers
    python scripts/download_benchmarks.py --year 2023              # SC2023 only
    python scripts/download_benchmarks.py --year 2024 -j 16       # SC2024, 16 workers
    python scripts/download_benchmarks.py --year all --bg          # Background mode
    python scripts/download_benchmarks.py --year 2023 --limit 50  # First 50 only
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
CNF_DIR = BENCH_DIR / "cnf"
CACHE_DIR = BENCH_DIR / ".cache"
INSTANCES_CSV = BENCH_DIR / "instances.csv"

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


def download_one(inst: dict, dest_dir: Path) -> tuple[dict, str | None, str, str | None]:
    """Returns (inst, cnf_path, status, error).  status ∈ {skipped, downloaded, failed}."""
    filename = inst["filename"]
    compressed = dest_dir / filename
    cnf_path = dest_dir / _cnf_name(filename)

    if cnf_path.exists():
        return (inst, str(cnf_path), "skipped", None)

    if compressed.exists():
        try:
            _decompress(compressed, cnf_path)
            return (inst, str(cnf_path), "skipped", None)
        except Exception:
            compressed.unlink(missing_ok=True)

    url = FILE_URL + inst["hash"]
    last_err = ""
    for attempt in range(MAX_RETRIES + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
                with open(compressed, "wb") as f:
                    shutil.copyfileobj(resp, f, length=1 << 20)

            if compressed.suffix in (".xz", ".gz"):
                _decompress(compressed, cnf_path)
            else:
                cnf_path = compressed

            return (inst, str(cnf_path), "downloaded", None)
        except Exception as e:
            last_err = str(e)
            if attempt < MAX_RETRIES:
                time.sleep(2 ** attempt)

    compressed.unlink(missing_ok=True)
    return (inst, None, "failed", last_err)


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
    args = parser.parse_args()

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

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    if args.force_refresh:
        for f in CACHE_DIR.glob("*.json"):
            f.unlink()

    years = ["2023", "2024"] if args.year == "all" else [args.year]

    CNF_DIR.mkdir(parents=True, exist_ok=True)

    # ── Gather instance lists ──
    all_instances: list[dict] = []
    for year in years:
        insts = fetch_track(year)
        for inst in insts:
            inst["year"] = year
        if args.limit:
            insts = insts[: args.limit]
        all_instances.extend(insts)
        log.info("SC%s: %d instances queued", year, len(insts))

    seen: set[str] = set()
    unique: list[dict] = []
    for inst in all_instances:
        if inst["hash"] not in seen:
            seen.add(inst["hash"])
            unique.append(inst)
    log.info("Total unique instances: %d  (workers=%d)", len(unique), args.workers)

    # ── Parallel download ──
    progress = _Progress(len(unique))
    results: list[dict] = []
    results_lock = threading.Lock()
    t0 = time.monotonic()

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(download_one, inst, CNF_DIR): inst for inst in unique}

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
                        "year": inst.get("year", ""),
                    })

            if done % 50 == 0:
                elapsed = time.monotonic() - t0
                log.info("--- progress: %d/%d  (DL=%d SKIP=%d FAIL=%d)  %.0fs ---",
                         done, progress.total, dl, sk, fl, elapsed)

    elapsed = time.monotonic() - t0

    # ── Write instances.csv ──
    results.sort(key=lambda r: r["filename"])
    with open(INSTANCES_CSV, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["hash", "filename", "cnf_path", "expected", "family", "author", "year"],
        )
        writer.writeheader()
        writer.writerows(results)

    sat_n = sum(1 for r in results if r["expected"] == "sat")
    unsat_n = sum(1 for r in results if r["expected"] == "unsat")

    log.info("=" * 60)
    log.info("Completed in %.0fs", elapsed)
    log.info("  Downloaded : %d", progress.downloaded)
    log.info("  Skipped    : %d  (already existed)", progress.skipped)
    log.info("  Failed     : %d", progress.failed)
    log.info("  Indexed    : %d  (SAT=%d  UNSAT=%d  unknown=%d)",
             len(results), sat_n, unsat_n, len(results) - sat_n - unsat_n)
    log.info("  Output     : %s", INSTANCES_CSV)


if __name__ == "__main__":
    main()
