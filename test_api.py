#!/usr/bin/env python3
"""BXC VideoAnalyzer API test runner.

Usage:
  ./test_api.py -ihp                          # test index, health, probertsp
  ./test_api.py --all                         # test all APIs
  ./test_api.py -c -U http://192.168.1.5:9002 # cleanupfiles against custom server
  ./test_api.py -ihpAmArscuvlt                # every test
  ./test_api.py -d --all                      # create upload_dir, then run all tests

Flags (combine freely after a single dash):
  i  index          GET /
  h  health         GET /api/health
  A  allmonitors    GET /api/controls
  m  monitor        POST /api/control
  a  addmonitor     POST /api/control/add
  r  removemonitor  POST /api/control/cancel
  p  probertsp      POST /api/probe_rtsp
  s  snapshot       POST /api/snapshot
  c  cleanupfiles   POST /api/cleanup_files  (creates fake files first)
  u  storageusage   GET /api/storage_usage
  v  clip           POST /api/clip
  l  alarmsound     GET /alarm-sound
  t  loadmodels     GET /api/test/load_models

Options:
  -U <url>  Override server base URL (default: from test_params.json)
  -P <path> Path to params JSON     (default: ./test_params.json)
  -d        Create upload_dir if it doesn't exist
  --all     Run every test
  --help    Show this help and exit
"""

import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# ── paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR = Path(__file__).parent
DEFAULT_PARAMS = SCRIPT_DIR / "test_params.json"

# ── ANSI ──────────────────────────────────────────────────────────────────────
GREEN = "\033[92m"
RED = "\033[91m"
RESET = "\033[0m"
BOLD = "\033[1m"


def _ok(name: str) -> None:
    print(f"  {GREEN}PASS{RESET}  {name}")


def _fail(name: str, reason: str) -> None:
    print(f"  {RED}FAIL{RESET}  {name}: {reason}")


# ── HTTP helpers ───────────────────────────────────────────────────────────────
def _http(url: str, body=None, method: str | None = None) -> tuple[int | None, str]:
    """GET when body is None, POST with JSON body otherwise."""
    if body is not None:
        data = json.dumps(body).encode()
        req = urllib.request.Request(
            url,
            data=data,
            headers={"Content-Type": "application/json"},
            method=method or "POST",
        )
    else:
        req = urllib.request.Request(url, method=method or "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, str(e)


def _json(text: str):
    try:
        return json.loads(text)
    except Exception:
        return None


# ── Test functions ─────────────────────────────────────────────────────────────
def test_index(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/")
    if status is None:
        _fail("index", body)
        return False
    j = _json(body)
    if status == 200 and j and "urls" in j:
        _ok("index")
        return True
    _fail("index", f"HTTP {status}  {body[:200]}")
    return False


def test_health(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/health")
    if status is None:
        _fail("health", body)
        return False
    j = _json(body)
    if status == 200 and j and j.get("code") == 1000:
        _ok("health")
        return True
    _fail("health", f"HTTP {status}  {body[:200]}")
    return False


def test_allmonitors(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/controls")
    if status is None:
        _fail("allmonitors", body)
        return False
    j = _json(body)
    if status == 200 and j and "code" in j:
        _ok("allmonitors")
        return True
    _fail("allmonitors", f"HTTP {status}  {body[:200]}")
    return False


def test_monitor(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/control", p["api_monitor"])
    if status is None:
        _fail("monitor", body)
        return False
    j = _json(body)
    if status == 200 and j and "code" in j:
        _ok("monitor")
        return True
    _fail("monitor", f"HTTP {status}  {body[:200]}")
    return False


def test_addmonitor(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/control/add", p["api_add_monitor"])
    if status is None:
        _fail("addmonitor", body)
        return False
    j = _json(body)
    # Server returns either a JSON array (sourceType=stream) or an object
    if status == 200 and j is not None:
        _ok("addmonitor")
        return True
    _fail("addmonitor", f"HTTP {status}  {body[:200]}")
    return False


def test_removemonitor(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/control/cancel", p["api_remove_monitor"])
    if status is None:
        _fail("removemonitor", body)
        return False
    j = _json(body)
    if status == 200 and j is not None:
        _ok("removemonitor")
        return True
    _fail("removemonitor", f"HTTP {status}  {body[:200]}")
    return False


def test_probertsp(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/probe_rtsp", p["api_probe_rtsp"])
    if status is None:
        _fail("probertsp", body)
        return False
    j = _json(body)
    if status == 200 and isinstance(j, list):
        _ok("probertsp")
        return True
    _fail("probertsp", f"HTTP {status}  {body[:200]}")
    return False


def test_snapshot(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/snapshot", p["api_snapshot"])
    if status is None:
        _fail("snapshot", body)
        return False
    j = _json(body)
    # PASS if the server replied with a meaningful JSON (data= or error= both fine)
    if status == 200 and j and ("data" in j or "error" in j):
        _ok("snapshot")
        return True
    _fail("snapshot", f"HTTP {status}  {body[:200]}")
    return False


def test_cleanupfiles(base: str, p: dict) -> bool:
    upload_dir = p.get("upload_dir", "")
    if not upload_dir:
        _fail("cleanupfiles", "upload_dir not set in test_params.json")
        return False

    fake_rels: list[str] = p["api_cleanup_files"]["fake_files"]
    api_paths: list[str] = p["api_cleanup_files"]["paths"]

    # 1. Create fake files inside upload_dir
    created: list[Path] = []
    for rel in fake_rels:
        full = Path(upload_dir) / rel
        try:
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text("fake content for cleanup test\n")
            created.append(full)
        except Exception as e:
            _fail("cleanupfiles", f"could not create {full}: {e}")
            return False

    print(f"         created {len(created)} fake file(s) in {upload_dir}")

    # 2. Call the API
    status, body = _http(f"{base}/api/cleanup_files", api_paths)
    if status is None:
        _fail("cleanupfiles", body)
        return False

    j = _json(body)
    if status != 200 or not j:
        _fail("cleanupfiles", f"HTTP {status}  {body[:200]}")
        return False

    removed = j.get("filesRemoved", 0)
    missing = j.get("filesMissingOk", 0)
    accounted = removed + missing

    if accounted >= len(fake_rels):
        _ok("cleanupfiles")
        return True

    still_there = [str(f) for f in created if f.exists()]
    _fail(
        "cleanupfiles",
        f"expected {len(fake_rels)} files removed/missing-ok, got {accounted}. "
        f"Still on disk: {still_there}. Body: {body[:200]}",
    )
    return False


def test_storageusage(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/storage_usage")
    if status is None:
        _fail("storageusage", body)
        return False
    j = _json(body)
    if status == 200 and j and "total_bytes" in j:
        _ok("storageusage")
        return True
    _fail("storageusage", f"HTTP {status}  {body[:200]}")
    return False


def test_clip(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/clip", p["api_clip"])
    if status is None:
        _fail("clip", body)
        return False
    j = _json(body)
    if status == 200 and j and "clip_path" in j:
        _ok("clip")
        return True
    _fail("clip", f"HTTP {status}  {body[:200]}")
    return False


def test_alarmsound(base: str, p: dict) -> bool:
    method = p["alarm_sound"]["method"]
    url = f"{base}/alarm-sound?method={urllib.parse.quote(method)}"
    status, body = _http(url)
    if status is None:
        _fail("alarmsound", body)
        return False
    if status == 200:
        _ok("alarmsound")
        return True
    _fail("alarmsound", f"HTTP {status}  {body[:200]}")
    return False


def test_loadmodels(base: str, p: dict) -> bool:
    status, body = _http(f"{base}/api/test/load_models")
    if status is None:
        _fail("loadmodels", body)
        return False
    j = _json(body)
    if status == 200 and j and j.get("code") == 1000:
        _ok("loadmodels")
        return True
    _fail("loadmodels", f"HTTP {status}  {body[:200]}")
    return False


# ── Registry: flag char → (label, function) ───────────────────────────────────
TESTS: dict[str, tuple[str, object]] = {
    "i": ("index", test_index),
    "h": ("health", test_health),
    "A": ("allmonitors", test_allmonitors),
    "m": ("monitor", test_monitor),
    "a": ("addmonitor", test_addmonitor),
    "r": ("removemonitor", test_removemonitor),
    "p": ("probertsp", test_probertsp),
    "s": ("snapshot", test_snapshot),
    "c": ("cleanupfiles", test_cleanupfiles),
    "u": ("storageusage", test_storageusage),
    "v": ("clip", test_clip),
    "l": ("alarmsound", test_alarmsound),
    "t": ("loadmodels", test_loadmodels),
}
ALL_FLAGS = "".join(TESTS)  # canonical order


# ── CLI parsing ────────────────────────────────────────────────────────────────
def _parse_cli(argv: list[str]) -> tuple[str | None, str | None, bool, bool, str]:
    """Returns (base_url, params_path, run_all, create_dirs, flag_string)."""
    base_url: str | None = None
    params_path: str | None = None
    run_all = False
    create_dirs = False
    flags = ""

    i = 0
    while i < len(argv):
        arg = argv[i]

        if arg in ("--help", "-?"):
            print(__doc__)
            sys.exit(0)

        elif arg == "--all":
            run_all = True

        elif arg in ("-U", "--url"):
            i += 1
            if i >= len(argv):
                sys.exit("ERROR: -U requires a URL argument")
            base_url = argv[i]
        elif arg.startswith("-U") and len(arg) > 2:
            base_url = arg[2:]

        elif arg in ("-P", "--params"):
            i += 1
            if i >= len(argv):
                sys.exit("ERROR: -P requires a path argument")
            params_path = argv[i]
        elif arg.startswith("-P") and len(arg) > 2:
            params_path = arg[2:]

        elif arg in ("-d", "--create-dirs"):
            create_dirs = True

        elif arg.startswith("-") and not arg.startswith("--"):
            # e.g. -ihp  or  -dih  (collect the chars after the dash, excluding 'd')
            for c in arg[1:]:
                if c == 'd':
                    create_dirs = True
                else:
                    flags += c

        else:
            # bare word treated as a flag string too
            flags += arg

        i += 1

    return base_url, params_path, run_all, create_dirs, flags


# ── Entry point ────────────────────────────────────────────────────────────────
def main() -> None:
    base_url, params_path, run_all, create_dirs, flags = _parse_cli(sys.argv[1:])

    if not run_all and not flags:
        print(__doc__)
        sys.exit(0)

    # Load parameters
    p_file = Path(params_path) if params_path else DEFAULT_PARAMS
    if not p_file.exists():
        sys.exit(f"ERROR: params file not found: {p_file}")
    with open(p_file) as f:
        params = json.load(f)

    # Create upload_dir if requested
    if create_dirs:
        upload_dir = params.get("upload_dir", "")
        if upload_dir:
            try:
                Path(upload_dir).mkdir(parents=True, exist_ok=True)
                print(f"Created directory: {upload_dir}")
            except Exception as e:
                sys.exit(f"ERROR: failed to create upload_dir: {e}")
        else:
            print("WARNING: upload_dir not set in params, skipping creation")

    # Resolve server URL
    if base_url is None:
        srv = params.get("server", {})
        base_url = f"http://{srv.get('host', '127.0.0.1')}:{srv.get('port', 9002)}"
    base_url = base_url.rstrip("/")

    # Determine ordered list of tests to run
    if run_all:
        selected = ALL_FLAGS
    else:
        unknown = [c for c in flags if c not in TESTS]
        if unknown:
            sys.exit(
                f"ERROR: unknown flag(s): {''.join(unknown)}\n"
                f"Valid flags: {ALL_FLAGS}\nRun with --help for details."
            )
        # Preserve canonical order, deduplicate
        seen: set[str] = set()
        selected = ""
        for c in ALL_FLAGS:
            if c in flags and c not in seen:
                selected += c
                seen.add(c)

    names = "  ".join(TESTS[c][0] for c in selected)
    print(f"{BOLD}Server:{RESET} {base_url}")
    print(f"{BOLD}Tests: {RESET} {names}")
    print()

    passed = 0
    failed = 0
    for flag in selected:
        label, fn = TESTS[flag]
        result = fn(base_url, params)
        if result:
            passed += 1
        else:
            failed += 1

    print()
    color = GREEN if failed == 0 else RED
    print(f"{color}{BOLD}Results: {passed}/{passed + failed} passed{RESET}")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
