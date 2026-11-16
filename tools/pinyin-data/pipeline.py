#!/usr/bin/env python3
"""Reproduce and evaluate the frozen Simple Voice data outside production paths.

Python standard library only. Run --help for explicit source/work/output roots.
All subprocesses use argument lists; no shell, user-profile discovery or TSF input.
"""

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sqlite3
import stat
import subprocess
import sys
import urllib.request
import zipfile

from prepare_data import generate, write_crlf

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
LOCK = json.loads((TOOLS / "source-lock.json").read_text(encoding="utf-8"))
ESDB_FOLDER = "wordlist-" + LOCK["esdb_commit"]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_info(path: Path) -> dict:
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": digest(data)}


def json_file(path: Path, value) -> None:
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


def announce(event: str, **values) -> None:
    print(json.dumps({"event": event, **values}, ensure_ascii=False), flush=True)


def write_root(path: Path) -> Path:
    path = path.resolve()
    protected = [REPO / name for name in ("rime-data", "src", "include", "cmake", "tools", "docs", ".git", "tests", "third_party")]
    if any(path.is_relative_to(item) or item.is_relative_to(path) for item in protected):
        raise ValueError(f"Output must be an isolated directory outside production/source paths: {path}")
    return path


def fresh_root(path: Path) -> Path:
    path = write_root(path)
    if path.exists():
        raise ValueError(f"Refusing to reuse an existing work/output directory: {path}")
    return path


def safe_child(root: Path, relative: str) -> Path:
    target = (root / relative).resolve()
    if not target.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes its root: {relative}")
    return target


def verify_source_bytes(entry: dict, data: bytes) -> None:
    if len(data) != entry["bytes"] or digest(data) != entry["sha256"]:
        raise ValueError(f"Pinned source mismatch: {entry['path']}")
    if entry.get("git_blob_sha1"):
        blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        if hashlib.sha1(blob).hexdigest() != entry["git_blob_sha1"]:
            raise ValueError(f"Git blob mismatch: {entry['path']}")


def verify_sources(source_dir: Path) -> list:
    entries = []
    for entry in LOCK["sources"]:
        path = safe_child(source_dir, entry["path"])
        verify_source_bytes(entry, path.read_bytes())
        entries.append({"path": entry["path"], "url": entry["url"], **file_info(path)})
    return entries


def fetch(args) -> None:
    source_dir = write_root(args.source_dir)
    cache = args.cache_dir.resolve() if args.cache_dir else None
    source_dir.mkdir(parents=True, exist_ok=True)
    for entry in LOCK["sources"]:
        target = safe_child(source_dir, entry["path"])
        if target.exists():
            verify_source_bytes(entry, target.read_bytes())
            announce("source_verified", path=entry["path"], origin="existing")
            continue
        data, origin = None, None
        if cache:
            for name in [entry["path"], entry.get("cache_alias")]:
                if not name:
                    continue
                cached = safe_child(cache, name)
                if cached.is_file():
                    data = cached.read_bytes()
                    if entry.get("cache_normalize_lf"):
                        data = data.replace(b"\r\n", b"\n")
                    verify_source_bytes(entry, data)
                    origin = "verified cache"
                    break
        if data is None:
            if args.offline:
                raise ValueError(f"Offline source missing from destination/cache: {entry['path']}")
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "Simple-Voice-data-reproduction/1"})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
            verify_source_bytes(entry, data)
            origin = "pinned download"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        announce("source_verified", path=entry["path"], origin=origin)
    json_file(source_dir / "verified-sources.json", verify_sources(source_dir))


def extract_esdb(archive_path: Path, work: Path) -> Path:
    root = work / "esdb"
    root.mkdir()
    with zipfile.ZipFile(archive_path) as archive:
        regular, links = [], []
        for entry in archive.infolist():
            destination = safe_child(root, entry.filename)
            if Path(entry.filename).parts[0] != ESDB_FOLDER:
                raise ValueError(f"Unexpected ESDB archive root: {entry.filename}")
            if stat.S_ISLNK(entry.external_attr >> 16):
                # The pinned archive has two PostgreSQL links to libscowl SQL.
                # Materialize only an in-archive regular target, never an OS link.
                target_name = archive.read(entry).decode("utf-8")
                target = safe_child(root, str(Path(entry.filename).parent / target_name))
                target_entry = archive.getinfo(target.relative_to(root).as_posix())
                if target_entry.is_dir() or stat.S_ISLNK(target_entry.external_attr >> 16):
                    raise ValueError(f"Unsupported archive link target: {entry.filename}")
                links.append((destination, target_entry))
            else:
                regular.append(entry)
        archive.extractall(root, members=regular)
        for destination, target_entry in links:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.read(target_entry))
    return root / ESDB_FOLDER


def build_database(source_dir: Path, work: Path) -> Path:
    source = extract_esdb(source_dir / "esdb-source.zip", work)
    database = source / "scowl.db"
    announce("esdb_build_started", log=str(work / "esdb-build.log"))
    with (work / "esdb-build.log").open("wb") as log:
        subprocess.run(
            [sys.executable, "-X", "utf8", "combine.py", "create-db", str(database)],
            cwd=source, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300,
        )
    announce("esdb_build_finished", **file_info(database))
    return database


def baseline_assets(source_dir: Path, work: Path) -> None:
    baseline = work / "baseline-rime-data"
    baseline.mkdir()
    for fixture in sorted((TOOLS / "fixtures/baseline").iterdir()):
        write_crlf(baseline / fixture.name, fixture.read_text(encoding="utf-8"))
    for name in ("pinyin_simp.dict.yaml", "pinyin_simp.AUTHORS", "pinyin_simp.LICENSE"):
        write_crlf(baseline / name, (source_dir / "pinyin-simp" / name).read_text(encoding="utf-8"))


def expected_hashes() -> dict:
    return dict(line.split("  ", 1)[::-1] for line in (TOOLS / "expected-sha256.txt").read_text(encoding="utf-8").splitlines() if line)


def verify_output(output: Path, *, require_manifest: bool = True) -> dict:
    expected = expected_hashes()
    actual_names = {path.name for path in output.iterdir() if path.name != "SHA256SUMS"}
    if actual_names != set(expected):
        raise ValueError(f"Output file set mismatch; missing={set(expected)-actual_names}, extra={actual_names-set(expected)}")
    for name, sha in expected.items():
        if file_info(output / name)["sha256"] != sha:
            raise ValueError(f"Frozen output SHA-256 mismatch: {name}")
    if require_manifest:
        manifest = "".join(f"{sha}  {name}\r\n" for name, sha in sorted(expected.items())).encode("utf-8")
        if (output / "SHA256SUMS").read_bytes() != manifest:
            raise ValueError("SHA256SUMS does not match the frozen CRLF manifest")
    row_counts = {}
    for path in sorted(output.glob("*.dict.yaml")):
        seen = set()
        for line in path.read_text(encoding="utf-8").splitlines():
            row = line.split("\t")
            if len(row) != 3:
                continue
            key = tuple(row[:2])
            if not all(key) or int(row[2]) <= 0 or key in seen:
                raise ValueError(f"Invalid/duplicate dictionary row in {path.name}: {row}")
            seen.add(key)
        row_counts[path.name] = len(seen)
    return {"files_hash_verified": len(expected), "dictionary_rows": row_counts}


def freeze_output(output: Path) -> dict:
    verify_output(output, require_manifest=False)
    lines = [f"{file_info(path)['sha256']}  {path.name}" for path in sorted(output.iterdir()) if path.name != "SHA256SUMS"]
    write_crlf(output / "SHA256SUMS", "\n".join(lines) + "\n")
    return verify_output(output)


def tool_manifest() -> dict:
    return {
        path.relative_to(TOOLS).as_posix(): file_info(path)
        for path in sorted(TOOLS.rglob("*"))
        if path.is_file() and "__pycache__" not in path.parts
    }


def build(args) -> None:
    source_dir = args.source_dir.resolve()
    work, output = fresh_root(args.work_dir), fresh_root(args.output_dir)
    if work == output or work.is_relative_to(output):
        raise ValueError("Output must not contain the work directory")
    if any(root.is_relative_to(source_dir) or source_dir.is_relative_to(root) for root in (work, output)):
        raise ValueError("Source cache and build/output roots must be separate")
    sources = verify_sources(source_dir)
    implementation = tool_manifest()
    work.mkdir(parents=True)
    database = build_database(source_dir, work)
    output.mkdir(parents=True)
    stats = generate(source_dir, database, output)
    for folder in ("schemas", "notices"):
        for path in sorted((TOOLS / "fixtures" / folder).iterdir()):
            write_crlf(output / path.name, path.read_text(encoding="utf-8"))
    for name in ("LICENSE", "LICENSE-SCOPE.md", "NOTICE.md"):
        shutil.copyfile(source_dir / "wanxiang" / name, output / ("wanxiang." + name))
    shutil.copyfile(database.parent / "Copyright", output / "esdb.Copyright")
    for name in ("pinyin_simp.AUTHORS", "pinyin_simp.LICENSE"):
        write_crlf(output / name, (source_dir / "pinyin-simp" / name).read_text(encoding="utf-8"))
    write_crlf(output / "pinyin_simp.SOURCE.txt", (TOOLS / "fixtures/baseline/SOURCE.txt").read_text(encoding="utf-8"))
    baseline_assets(source_dir, work)
    verified = freeze_output(output)
    json_file(work / "generation.json", {
        "python_version": platform.python_version(), "sqlite_version": sqlite3.sqlite_version,
        "source_dir": str(source_dir), "output_dir": str(output),
        "source_lock_sha256": file_info(TOOLS / "source-lock.json")["sha256"],
        "sources": sources, "derived_database": file_info(database),
        "selection": stats, "verification": verified, "tools": implementation,
    })
    announce("generation_verified", output_dir=str(output), **stats, **verified)


def reproduce(args) -> None:
    fresh_root(args.work_dir)
    fresh_root(args.output_dir)
    fetch(args)
    build(args)


def compile_probes(args) -> None:
    if os.name != "nt":
        raise ValueError("Probe compilation is validated for Windows MinGW; data generation is platform-independent")
    output = fresh_root(args.output_dir)
    prefix = args.rime_prefix.resolve()
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise ValueError(f"C++ compiler was not found: {args.cxx}")
    libraries = [prefix / "lib" / name for name in ("librime.a", "libyaml-cpp.a", "libleveldb.a", "libopencc.a", "libmarisa.a")]
    for library in libraries:
        if not library.is_file():
            raise ValueError(f"Required prebuilt library not found: {library}")
    output.mkdir(parents=True)
    commands = []
    for name in ("evaluate", "personal_probe"):
        command = [compiler, "-std=c++17", "-O2", "-static", "-DRIME_STATIC", "-finput-charset=UTF-8", "-fexec-charset=UTF-8",
                   "-I" + str(prefix / "include"), str(TOOLS / "probes" / (name + ".cpp")),
                   "-Wl,--whole-archive", str(libraries[0]), "-Wl,--no-whole-archive",
                   *map(str, libraries[1:]), "-lbcrypt", "-lws2_32", "-lwsock32", "-lwinpthread", "-o", str(output / (name + ".exe"))]
        with (output / (name + "-compile.log")).open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        commands.append(command)
    json_file(output / "build-info.json", {
        "compiler": compiler,
        "compiler_version": subprocess.check_output([compiler, "--version"]).decode("utf-8", errors="replace").splitlines()[0],
        "commands": commands,
        "libraries": {path.name: file_info(path) for path in libraries},
        "probes": {name: file_info(output / name) for name in ("evaluate.exe", "personal_probe.exe")},
    })
    announce("probes_compiled", output_dir=str(output))


def validate_corpus(path: Path) -> int:
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    if not lines or lines[0] != "category\tinput\texpected":
        raise ValueError("Corpus header must be category<TAB>input<TAB>expected")
    for index, line in enumerate(lines[1:], 2):
        row = line.split("\t")
        if len(row) != 3 or not all(row) or not all(0x20 <= ord(c) < 0x7f for c in row[1]):
            raise ValueError(f"Malformed corpus row {index}")
    return len(lines) - 1


def probe_run(args, personal=False) -> None:
    work = fresh_root(args.work_dir)
    probe, shared = args.probe.resolve(), args.shared_dir.resolve()
    if not probe.is_file() or not shared.is_dir():
        raise ValueError("Probe executable and shared-data directory must exist")
    if personal:
        command = [str(probe), str(shared), str(work / "user"), args.mode]
        output_name = "results.tsv"
    else:
        corpus = args.corpus.resolve()
        count = validate_corpus(corpus)
        command = [str(probe), str(shared), str(work / "user"), args.schema, str(corpus)]
        output_name = "results.json"
    work.mkdir(parents=True)
    with (work / output_name).open("wb") as out, (work / "probe.log").open("wb") as err:
        subprocess.run(command, stdout=out, stderr=err, check=True, timeout=300)
    metadata = {"command": command, "probe": file_info(probe), "shared_files": {
        path.name: file_info(path) for path in sorted(shared.iterdir()) if path.is_file()
    }}
    if not personal:
        rows = json.loads((work / output_name).read_text(encoding="utf-8"))
        if len(rows) != count:
            raise ValueError(f"Probe evaluated {len(rows)} rows, expected {count}")
        summary = defaultdict(lambda: {"cases": 0, "top1": 0, "top5": 0, "top1_misses": [], "top5_misses": []})
        for row in rows:
            section = summary[row["category"]]
            section["cases"] += 1
            accepted, candidates = row["expected"].split("|"), row["candidates"]
            top1 = bool(candidates and candidates[0] in accepted)
            top5 = any(candidate in accepted for candidate in candidates)
            section["top1"] += int(top1)
            section["top5"] += int(top5)
            if not top1:
                section["top1_misses"].append(row)
            if not top5:
                section["top5_misses"].append(row)
        json_file(work / "summary.json", summary)
        metadata["corpus"] = {"path": str(corpus), **file_info(corpus)}
        announce("evaluation_finished", summary={key: {k: v for k, v in value.items() if not k.endswith("misses")} for key, value in summary.items()})
    else:
        announce("personal_probe_finished", mode=args.mode, results=str(work / output_name))
    json_file(work / "run-info.json", metadata)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    for name, handler in (("fetch", fetch), ("build", build), ("reproduce", reproduce)):
        command = commands.add_parser(name)
        command.add_argument("--source-dir", type=Path, required=True)
        if name in ("fetch", "reproduce"):
            command.add_argument("--cache-dir", type=Path)
            command.add_argument("--offline", action="store_true")
        if name in ("build", "reproduce"):
            command.add_argument("--work-dir", type=Path, required=True)
            command.add_argument("--output-dir", type=Path, required=True)
        command.set_defaults(handler=handler)
    command = commands.add_parser("verify")
    command.add_argument("--output-dir", type=Path, required=True)
    command.set_defaults(handler=lambda args: announce("output_verified", **verify_output(args.output_dir.resolve())))
    command = commands.add_parser("freeze")
    command.add_argument("--output-dir", type=Path, required=True)
    command.set_defaults(handler=lambda args: announce("output_frozen", **freeze_output(write_root(args.output_dir))))
    command = commands.add_parser("compile")
    command.add_argument("--cxx", required=True, help="GNU/MinGW g++ name or executable path")
    command.add_argument("--rime-prefix", type=Path, required=True)
    command.add_argument("--output-dir", type=Path, required=True)
    command.set_defaults(handler=compile_probes)
    for name in ("evaluate", "personal"):
        command = commands.add_parser(name)
        command.add_argument("--probe", type=Path, required=True)
        command.add_argument("--shared-dir", type=Path, required=True)
        command.add_argument("--work-dir", type=Path, required=True)
        if name == "evaluate":
            command.add_argument("--schema", default="voiceime")
            command.add_argument("--corpus", type=Path, required=True)
            command.set_defaults(handler=probe_run)
        else:
            command.add_argument("--mode", choices=("normal", "private", "refresh", "generation"), required=True)
            command.set_defaults(handler=lambda args: probe_run(args, personal=True))
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        args.handler(args)
    except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
