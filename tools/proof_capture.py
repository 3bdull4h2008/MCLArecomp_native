#!/usr/bin/env python3
"""Local-only, draft-capable proof capture package tool for MCLA PC.

This tool never uploads, invokes Git, synthesizes game input, edits source/config/
executable/guest files, or kills by image name. It writes only below
build/proof/<session-id>/ and, after a future human approval, an explicitly
supplied empty public staging directory.

Draft mode is deliberate: PROOF-SESSION, PROOF-CONFIG, PROOF-INPUT and
PROOF-FRAME do not exist in the runtime yet. Draft packages preserve evidence
and produce an annotated derivative, but package/verify report state=DRAFT and
approve/stage-public remain hard-disabled.
"""

from __future__ import annotations

import argparse
import atexit
import csv
import ctypes
import datetime as dt
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from ctypes import wintypes
from pathlib import Path
from typing import Iterable

TOOL_VERSION = "1.0"
SCHEMA = "mcla-proof-package/v1"
ROOT = Path(__file__).resolve().parents[1]
PROOF_ROOT = ROOT / "build" / "proof"
STATE_ORDER = ["PREPARED", "RECORDING", "RECORDED", "PACKAGED", "VERIFIED", "APPROVED", "STAGED"]
REQUIRED_MARKERS = ("PROOF-SESSION", "PROOF-CONFIG", "PROOF-INPUT", "PROOF-FRAME")
MARKER_FIELDS = {
    "PROOF-SESSION": ("session_id", "build", "route"),
    "PROOF-CONFIG": ("key", "value"),
    "PROOF-INPUT": ("port", "source", "instance", "generation", "packet", "buttons",
                    "triggers", "sticks", "event", "qpc", "utc"),
    "PROOF-FRAME": ("outcome", "attempt", "presented", "displayed", "owner", "hresult",
                    "drawn", "qpc", "utc"),
}
FRAME_COLUMNS = ("outcome", "attempt", "presented", "displayed", "owner", "hresult",
                 "drawn", "qpc", "utc")
INPUT_COLUMNS = ("port", "source", "instance", "generation", "packet", "buttons",
                 "triggers", "sticks", "event", "qpc", "utc")
EVENT_COLUMNS = ("utc", "qpc", "event", "detail")
PUBLIC_TEXT_SUFFIXES = {".json", ".csv", ".txt", ".ass", ".md"}
ALLOWED_CHILD_ENV = {
    "SystemRoot", "SystemDrive", "WINDIR", "COMSPEC", "PATH", "PATHEXT", "TEMP", "TMP",
    "USERPROFILE", "APPDATA", "LOCALAPPDATA", "HOMEDRIVE", "HOMEPATH",
    "MCLA_LOG_LEVEL", "MCLA_FIXED_SEED", "MCLA_RENDER_SCALE", "MCLA_PERF_STAGES",
    "MCLA_PERF_FRAMES", "MCLA_PSO_CACHE", "MCLA_PSO_SEED", "MCLA_NO_PSO_PREWARM",
    "MCLA_NO_PSO_SEED", "MCLA_NO_PSO_PERSIST", "MCLA_BMP_DUMP",
}
ALLOWED_CHILD_MCLA_ENV = ALLOWED_CHILD_ENV - {
    "SystemRoot", "SystemDrive", "WINDIR", "COMSPEC", "PATH", "PATHEXT", "TEMP", "TMP",
    "USERPROFILE", "APPDATA", "LOCALAPPDATA", "HOMEDRIVE", "HOMEPATH",
}
P0_MARKERS = (
    "SAMPLER-HEAP-OWNER-MISMATCH", "ALLOC-LIFETIME-FENCE",
    "RESOLVED-BIND-DIM-MISMATCH", "D3D12-VALIDATION", "DEVICE-REMOVED",
)


class ProofError(Exception):
    pass


def kernel32_api():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    api.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                               ctypes.POINTER(wintypes.DWORD)]
    api.QueryFullProcessImageNameW.restype = wintypes.BOOL
    api.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
                                    ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                    ctypes.POINTER(wintypes.FILETIME)]
    api.GetProcessTimes.restype = wintypes.BOOL
    api.QueryPerformanceFrequency.argtypes = [ctypes.POINTER(wintypes.LARGE_INTEGER)]
    api.QueryPerformanceFrequency.restype = wintypes.BOOL
    api.QueryPerformanceCounter.argtypes = [ctypes.POINTER(wintypes.LARGE_INTEGER)]
    api.QueryPerformanceCounter.restype = wintypes.BOOL
    return api


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def parse_utc(value: str) -> float:
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ProofError(f"invalid UTC timestamp: {value}") from exc
    if parsed.tzinfo is None:
        raise ProofError("UTC timestamp must include a timezone")
    return parsed.timestamp()


def qpc_now() -> int:
    if os.name != "nt":
        return time.time_ns()
    frequency = wintypes.LARGE_INTEGER()
    counter = wintypes.LARGE_INTEGER()
    kernel32 = kernel32_api()
    if not kernel32.QueryPerformanceFrequency(ctypes.byref(frequency)):
        raise ProofError("QueryPerformanceFrequency failed")
    if not kernel32.QueryPerformanceCounter(ctypes.byref(counter)):
        raise ProofError("QueryPerformanceCounter failed")
    return counter.value


def qpc_frequency() -> int:
    if os.name != "nt":
        return 1_000_000_000
    frequency = wintypes.LARGE_INTEGER()
    if not kernel32_api().QueryPerformanceFrequency(ctypes.byref(frequency)):
        raise ProofError("QueryPerformanceFrequency failed")
    return frequency.value


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def write_json(path: Path, value) -> None:
    atomic_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"))


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ProofError(f"cannot read JSON {path}: {exc}") from exc


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_streamed(source: Path, destination: Path) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        for chunk in iter(lambda: incoming.read(1024 * 1024), b""):
            digest.update(chunk)
            outgoing.write(chunk)
        outgoing.flush()
        os.fsync(outgoing.fileno())
    return digest.hexdigest()


def validate_session_id(session_id: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", session_id) or ".." in session_id:
        raise ProofError("session id must be a safe single path component")
    return session_id


def session_dir(session_id: str) -> Path:
    validate_session_id(session_id)
    path = PROOF_ROOT / session_id
    if path.resolve().parent != PROOF_ROOT.resolve():
        raise ProofError("session path escaped build/proof")
    return path


def load_state(session_id: str) -> tuple[Path, dict]:
    directory = session_dir(session_id)
    state = read_json(directory / "state.json")
    if state.get("session_id") != session_id:
        raise ProofError("state/session id mismatch")
    return directory, state


def save_state(directory: Path, state: dict) -> None:
    state["updated_utc"] = utc_now()
    write_json(directory / "state.json", state)


def transition(state: dict, expected: str, target: str) -> None:
    if state.get("lifecycle_state") != expected:
        raise ProofError(f"expected state {expected}, found {state.get('lifecycle_state')}")
    if STATE_ORDER.index(target) != STATE_ORDER.index(expected) + 1:
        raise ProofError(f"invalid transition {expected} -> {target}")
    state["lifecycle_state"] = target


def redact_value(value):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            lowered = key.lower()
            if any(token in lowered for token in ("serial", "deviceid", "device_id", "uuid", "macaddress", "smbios")):
                result[key] = "<redacted>"
            else:
                result[key] = redact_value(item)
        return result
    if isinstance(value, list):
        return [redact_value(item) for item in value]
    return value


def redact_text(text: str) -> str:
    text = re.sub(r"(?i)\b[A-Z]:\\[^\r\n\"']+", "<absolute-path>", text)
    text = re.sub(r"(?<![A-Za-z0-9])/(?:home|Users|root|mnt)/[^\s\"']+", "<absolute-path>", text)
    text = re.sub(r"(?i)PCI\\VEN_[0-9A-F]{4}&DEV_[0-9A-F]{4}[^\s,;]*", "<device-id>", text)
    text = re.sub(r"(?i)\bGPU-[0-9a-f-]{8,}\b", "<device-id>", text)
    return text


def collect_hardware() -> dict:
    evidence = {
        "collected_utc": utc_now(),
        "collection": "local CIM/WMI read-only query",
        "redactions": ["serial numbers", "device IDs", "UUIDs", "MAC addresses", "absolute paths"],
    }
    if os.name != "nt":
        evidence["status"] = "unavailable"
        evidence["reason"] = "non-Windows host"
        return evidence
    command = (
        "$os=Get-CimInstance Win32_OperatingSystem; "
        "$cpu=Get-CimInstance Win32_Processor | Select-Object -First 1; "
        "$gpu=Get-CimInstance Win32_VideoController; "
        "$cs=Get-CimInstance Win32_ComputerSystem; "
        "[pscustomobject]@{os_caption=$os.Caption; os_version=$os.Version; "
        "cpu_name=$cpu.Name; cpu_cores=$cpu.NumberOfCores; cpu_threads=$cpu.NumberOfLogicalProcessors; "
        "memory_gib=[math]::Round($cs.TotalPhysicalMemory/1GB,2); "
        "gpu=@($gpu | Select-Object Name,DriverVersion,AdapterRAM)} | ConvertTo-Json -Compress -Depth 4"
    )
    try:
        completed = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                                   capture_output=True, text=True, timeout=30, check=False)
        if completed.returncode != 0:
            raise ProofError(completed.stderr.strip() or "hardware query failed")
        evidence["status"] = "collected"
        evidence["hardware"] = redact_value(json.loads(completed.stdout or "{}"))
    except (OSError, subprocess.SubprocessError, ValueError, ProofError) as exc:
        evidence["status"] = "unavailable"
        evidence["reason"] = redact_text(str(exc))
    return evidence


def public_hardware(hardware: dict) -> dict:
    raw = hardware.get("hardware", {}) if isinstance(hardware, dict) else {}
    gpu = raw.get("gpu", []) if isinstance(raw, dict) else []
    if isinstance(gpu, dict):
        gpu = [gpu]
    names = [redact_text(str(item.get("Name", "")))[:120] for item in gpu if isinstance(item, dict)]
    return {
        "status": hardware.get("status", "unavailable"),
        "os_caption": redact_text(str(raw.get("os_caption", "")))[:120],
        "cpu_name": redact_text(str(raw.get("cpu_name", "")))[:120],
        "cpu_cores": raw.get("cpu_cores"),
        "cpu_threads": raw.get("cpu_threads"),
        "memory_gib": raw.get("memory_gib"),
        "gpu_names": names,
        "redactions": hardware.get("redactions", []),
    }


def hash_artifact(role: str, path: Path, required: bool = False) -> dict:
    resolved = path.resolve()
    item = {"role": role, "path": str(resolved), "exists": resolved.is_file()}
    if not item["exists"]:
        if required:
            raise ProofError(f"required {role} artifact not found: {resolved}")
        return item
    item["bytes"] = resolved.stat().st_size
    item["sha256"] = sha256_file(resolved)
    return item


def parse_role_path(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise ProofError("--artifact/--config/--cache requires role=path")
    role, path = value.split("=", 1)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", role) or ".." in role or not path:
        raise ProofError("artifact role must be a safe single path component and path is required")
    return role.lower(), Path(path)


def prepare_session(args) -> tuple[Path, dict]:
    directory = session_dir(args.session_id)
    if directory.exists():
        raise ProofError(f"session already exists: {directory}")
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", args.commit):
        raise ProofError("--commit must be a full Git object id supplied by the orchestrator")
    if args.clean_attestation not in ("clean", "dirty"):
        raise ProofError("--clean-attestation must be clean or dirty")
    if not args.attested_by.strip() or not args.route.strip():
        raise ProofError("--attested-by and --route are required")
    if re.search(r"(?i)[A-Z]:\\|/(?:home|Users|root)/", args.route):
        raise ProofError("route must not contain an absolute path")
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        raise ProofError(f"executable not found: {exe}")
    for subdir in ("raw/config", "raw/ffprobe", "evidence", "timelines", "derivative", "public/timelines"):
        (directory / subdir).mkdir(parents=True, exist_ok=False)

    artifacts = []
    defaults = (
        ("map", Path(args.map), False), ("pdb", Path(args.pdb), False),
        ("cmake-cache", Path(args.cmake_cache), False),
        ("guest-xex", Path(args.guest_xex), False), ("guest-pe", Path(args.guest_pe), False),
    )
    artifacts.append(hash_artifact("exe", exe, True))
    for role, path, required in defaults:
        artifacts.append(hash_artifact(role, path, required))
    for value in args.artifact:
        role, path = parse_role_path(value)
        artifacts.append(hash_artifact(role, path, role in ("ffmpeg", "obs")))
    for value in args.config:
        role, path = parse_role_path(value)
        artifacts.append(hash_artifact(f"config/{role}", path, True))
    for value in args.cache:
        role, path = parse_role_path(value)
        artifacts.append(hash_artifact(f"cache/{role}", path, True))
    artifacts.append(hash_artifact("tool", Path(__file__), True))
    for role, executable in (("ffmpeg", "ffmpeg"), ("ffprobe", "ffprobe")):
        found = shutil.which(executable)
        if found:
            artifacts.append(hash_artifact(role, Path(found), True))
        else:
            artifacts.append({"role": role, "path": executable, "exists": False})

    hardware = collect_hardware()
    scope = {
        "route": args.route,
        "claims": list(args.claim),
        "exclusions": ["no claim of verified frame/input identity in draft mode"],
        "operator_attestation": {
            "commit": args.commit,
            "clean": args.clean_attestation,
            "attested_by": args.attested_by,
            "attested_utc": utc_now(),
        },
    }
    session = {
        "schema": SCHEMA,
        "session_id": args.session_id,
        "mode": "draft",
        "proof_state": "DRAFT",
        "created_utc": utc_now(),
        "exe": str(exe),
        "route": args.route,
        "commit": args.commit,
        "clean_attestation": args.clean_attestation,
        "attested_by": args.attested_by,
        "tool_version": TOOL_VERSION,
    }
    write_json(directory / "session.json", session)
    write_json(directory / "scope.json", scope)
    write_json(directory / "evidence/build.json", {
        "commit": args.commit, "clean_attestation": args.clean_attestation,
        "attested_by": args.attested_by, "attested_utc": scope["operator_attestation"]["attested_utc"],
        "tool_version": TOOL_VERSION, "artifacts": artifacts,
    })
    write_json(directory / "evidence/guest.json", {
        "artifacts": [item for item in artifacts if item["role"] in ("guest-xex", "guest-pe", "rpf") or item["role"].startswith("rpf/")],
    })
    write_json(directory / "evidence/config.json", {
        "artifacts": [item for item in artifacts if item["role"].startswith("config/")],
        "effective_cvars": "pending ingest",
    })
    write_json(directory / "evidence/cache.json", {
        "artifacts": [item for item in artifacts if item["role"].startswith("cache/")],
    })
    write_json(directory / "evidence/hardware.json", hardware)
    write_json(directory / "evidence/process.json", {"status": "not-started"})
    write_json(directory / "evidence/recorder.json", {
        "preferred": "OBS window capture, MKV", "ffmpeg": next((a for a in artifacts if a["role"] == "ffmpeg"), None),
        "ffprobe": next((a for a in artifacts if a["role"] == "ffprobe"), None),
        "obs": [a for a in artifacts if a["role"] == "obs"],
    })
    write_json(directory / "evidence/clock.json", {"status": "pending-ingest", "method": None})
    write_json(directory / "evidence/p0.json", {"status": "pending-ingest"})
    write_json(directory / "evidence/soak.json", {"status": "pending-ingest"})
    state = {
        "schema": SCHEMA, "session_id": args.session_id, "mode": "draft", "proof_state": "DRAFT",
        "lifecycle_state": "PREPARED", "created_utc": utc_now(), "route": args.route,
        "commit": args.commit, "clean_attestation": args.clean_attestation,
    }
    save_state(directory, state)
    return directory, state


def list_processes() -> list[dict]:
    if os.name != "nt":
        rows = []
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                image = os.readlink(entry / "exe")
            except OSError:
                continue
            rows.append({"PID": int(entry.name), "ImageName": Path(image).name, "ExecutablePath": image})
        return rows
    try:
        completed = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True,
                                   text=True, timeout=20, check=True)
    except (OSError, subprocess.SubprocessError) as exc:
        raise ProofError(f"process enumeration failed closed: {exc}") from exc
    rows = []
    for row in csv.reader(io.StringIO(completed.stdout)):
        if len(row) >= 2 and row[1].isdigit():
            rows.append({"PID": int(row[1]), "ImageName": row[0]})
    return rows


def assert_no_conflicting_processes(rows: Iterable[dict]) -> None:
    conflicts = [row for row in rows if str(row.get("ImageName", "")).lower() in ("mcla.exe", "ninja.exe")]
    if conflicts:
        raise ProofError("refusing capture while another mcla.exe/ninja.exe is running: " +
                         ", ".join(f"{row.get('ImageName')} PID={row.get('PID')}" for row in conflicts))


def process_image_path(pid: int) -> str | None:
    if os.name != "nt":
        try:
            return os.readlink(f"/proc/{pid}/exe")
        except OSError:
            return None
    kernel32 = kernel32_api()
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_ulong(len(buffer))
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return None
        return buffer.value
    finally:
        kernel32.CloseHandle(handle)


def process_creation_time(pid: int) -> int | None:
    if os.name != "nt":
        try:
            return int((Path("/proc") / str(pid) / "stat").read_text().split()[21])
        except (OSError, IndexError, ValueError):
            return None
    kernel32 = kernel32_api()
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel_time = wintypes.FILETIME()
        user_time = wintypes.FILETIME()
        if not kernel32.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                        ctypes.byref(kernel_time), ctypes.byref(user_time)):
            return None
        return (creation.dwHighDateTime << 32) | creation.dwLowDateTime
    finally:
        kernel32.CloseHandle(handle)


def pid_exists(pid: int) -> bool:
    if os.name != "nt":
        return Path(f"/proc/{pid}").exists()
    kernel32 = kernel32_api()
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    kernel32.CloseHandle(handle)
    return True


def stop_child_pid(pid: int, expected_image: str, expected_creation: int | None) -> dict:
    observed = process_image_path(pid)
    if observed is None:
        return {"pid": pid, "running_before_stop": False, "stopped_by_tool": False}
    creation = process_creation_time(pid)
    if os.path.normcase(os.path.abspath(observed)) != os.path.normcase(os.path.abspath(expected_image)):
        raise ProofError(f"PID {pid} rebound to a different image; refusing to stop it")
    if expected_creation is not None and creation != expected_creation:
        raise ProofError(f"PID {pid} creation time changed; refusing a possible PID-reuse stop")
    completed = subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True,
                               text=True, timeout=20, check=False)
    if completed.returncode != 0 and pid_exists(pid):
        raise ProofError(f"could not stop child PID {pid}: {completed.stderr.strip()}")
    deadline = time.monotonic() + 10
    while pid_exists(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    if pid_exists(pid):
        raise ProofError(f"child PID {pid} did not exit after PID-only stop")
    return {"pid": pid, "running_before_stop": True, "stopped_by_tool": True,
            "observed_image": observed, "stop_utc": utc_now()}


def parse_child_env(values: Iterable[str]) -> dict:
    environment = {key: value for key, value in os.environ.items() if key in ALLOWED_CHILD_ENV}
    environment["MCLA_LOG_LEVEL"] = "2"
    for value in values:
        if "=" not in value:
            raise ProofError("--env requires KEY=VALUE")
        key, item = value.split("=", 1)
        if key not in ALLOWED_CHILD_MCLA_ENV:
            raise ProofError(f"child environment key is not allowlisted: {key}")
        environment[key] = item
    if environment.get("MCLA_LOG_LEVEL") != "2":
        raise ProofError("proof capture requires MCLA_LOG_LEVEL=2")
    return environment


def capture_session(args) -> tuple[Path, dict]:
    directory, state = load_state(args.session_id)
    if args.start == args.stop:
        raise ProofError("capture requires exactly one of --start or --stop")
    if args.start:
        transition(state, "PREPARED", "RECORDING")
        session = read_json(directory / "session.json")
        exe = Path(session["exe"])
        environment = parse_child_env(args.env)
        stdout_path = directory / "raw" / "stdout.log"
        stderr_path = directory / "raw" / "stderr.log"
        if stdout_path.exists() or stderr_path.exists():
            raise ProofError("capture logs already exist; refusing overwrite")
        try:
            with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                process = subprocess.Popen([str(exe)], cwd=exe.parent, env=environment,
                                           stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                           shell=False)
        except OSError:
            for path in (stdout_path, stderr_path):
                if path.exists() and path.stat().st_size == 0:
                    path.unlink()
            raise
        child = {
            "pid": process.pid, "expected_image": str(exe), "observed_image": process_image_path(process.pid),
            "creation_time_100ns": process_creation_time(process.pid),
            "command_line": subprocess.list2cmdline([str(exe)]), "cwd": str(exe.parent),
            "started_utc": utc_now(), "qpc_start": qpc_now(), "qpc_frequency_hz": qpc_frequency(),
            "environment": environment, "stdout": "raw/stdout.log", "stderr": "raw/stderr.log",
        }
        write_json(directory / "raw" / "child.json", child)
        write_json(directory / "evidence/process.json", {
            "status": "recording", "pid": process.pid, "expected_image": str(exe),
            "observed_image": child["observed_image"], "creation_time_100ns": child["creation_time_100ns"],
            "started_utc": child["started_utc"],
            "qpc_start": child["qpc_start"], "qpc_frequency_hz": child["qpc_frequency_hz"],
            "environment_keys": sorted(environment),
        })
        save_state(directory, state)
        return directory, state
    if args.stop:
        if not args.operator_approved_stop or not args.operator:
            raise ProofError("--stop requires --operator-approved-stop and --operator")
        if state.get("lifecycle_state") != "RECORDING":
            raise ProofError("capture is not RECORDING")
        child = read_json(directory / "raw" / "child.json")
        result = stop_child_pid(int(child["pid"]), child["expected_image"], child.get("creation_time_100ns"))
        result.update({"operator": args.operator, "approval": "explicit --operator-approved-stop"})
        write_json(directory / "evidence/process.json", {
            "status": "stopped", **child, **result,
            "environment_keys": sorted(child.get("environment", {})),
        })
        transition(state, "RECORDING", "RECORDED")
        save_state(directory, state)
        return directory, state
    raise ProofError("capture requires --start or --stop")


def run_ffprobe(executable: str, media: Path, frames: bool = False) -> dict:
    command = [executable, "-v", "error"]
    if frames:
        command += ["-select_streams", "v:0", "-show_frames", "-show_entries",
                    "frame=best_effort_timestamp_time", "-of", "json", str(media)]
    else:
        command += ["-show_format", "-show_streams", "-of", "json", str(media)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=300, check=False)
    if completed.returncode != 0:
        raise ProofError(f"ffprobe failed: {completed.stderr.strip()}")
    try:
        return json.loads(completed.stdout)
    except ValueError as exc:
        raise ProofError("ffprobe returned invalid JSON") from exc


def full_decode(executable: str, media: Path) -> tuple[int, str, str]:
    completed = subprocess.run([executable, "-hide_banner", "-v", "error", "-xerror", "-i", str(media),
                                "-map", "0:v:0", "-f", "null", "-"],
                               capture_output=True, text=True, timeout=600, check=False)
    return completed.returncode, completed.stdout, completed.stderr


def media_duration(probe: dict, frame_times: list[float]) -> float:
    duration = probe.get("format", {}).get("duration")
    if duration not in (None, "N/A"):
        return float(duration)
    if len(frame_times) >= 2:
        return frame_times[-1] - frame_times[0]
    raise ProofError("media duration unavailable")


def frame_timestamps(probe: dict) -> list[float]:
    values = []
    for frame in probe.get("frames", []):
        raw = frame.get("best_effort_timestamp_time")
        if raw in (None, "N/A"):
            raise ProofError("frame timestamp missing")
        values.append(float(raw))
    if len(values) < 2:
        raise ProofError("fewer than two video frames")
    if any(current <= previous for previous, current in zip(values, values[1:])):
        raise ProofError("video frame timestamps are not strictly monotonic")
    return values


def align_clock(directory: Path, obs_clock: Path | None, marks_csv: Path | None) -> dict:
    if obs_clock:
        supplied = read_json(obs_clock)
        frequency = int(supplied.get("qpc_frequency_hz", 0))
        origin = int(supplied.get("qpc_origin", 0))
        utc_origin = supplied.get("utc_origin")
        if frequency <= 0 or utc_origin is None:
            raise ProofError("OBS clock needs qpc_frequency_hz, qpc_origin and utc_origin")
        parse_utc(utc_origin)
        result = {
            "status": "aligned", "method": "obs_qpc", "qpc_frequency_hz": frequency,
            "qpc_origin": origin, "utc_origin": utc_origin,
            "source": str(obs_clock.resolve()), "assumption": "none; explicit OBS QPC mapping",
        }
    elif marks_csv:
        marks = []
        with marks_csv.open("r", encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source)
            if not {"label", "utc", "qpc"}.issubset(reader.fieldnames or []):
                raise ProofError("marks CSV requires label,utc,qpc")
            for row in reader:
                marks.append({"label": row["label"], "utc": row["utc"],
                              "utc_epoch": parse_utc(row["utc"]), "qpc": int(row["qpc"])})
        if len(marks) < 2 or marks[0]["qpc"] == marks[1]["qpc"]:
            raise ProofError("clock alignment requires at least two distinct human QPC marks")
        slope = (marks[1]["utc_epoch"] - marks[0]["utc_epoch"]) / (marks[1]["qpc"] - marks[0]["qpc"])
        if slope <= 0:
            raise ProofError("human clock marks have non-positive UTC/QPC slope")
        intercept = marks[0]["utc_epoch"] - slope * marks[0]["qpc"]
        residuals = [abs((slope * mark["qpc"] + intercept) - mark["utc_epoch"]) for mark in marks]
        result = {
            "status": "aligned", "method": "human-marks", "marks": marks,
            "utc_seconds_per_qpc_tick": slope, "utc_epoch_intercept": intercept,
            "max_residual_seconds": max(residuals), "source": str(marks_csv.resolve()),
            "assumption": "none; at least two explicit human sync marks",
        }
    else:
        result = {"status": "unavailable", "method": None,
                  "reason": "no OBS QPC mapping or two human sync marks; wall clock not assumed"}
    write_json(directory / "evidence/clock.json", result)
    write_json(directory / "timelines/clock_alignment.json", result)
    return result


def marker_message(line: str) -> str | None:
    match = re.match(r"^\[[^\]]+\]\s+\[[^\]]+\]\s+(.*)$", line)
    return match.group(1) if match else None


def parse_key_values(message: str) -> dict:
    values = {}
    for token in shlex.split(message, posix=True):
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def parse_runtime_markers(text: str) -> dict:
    rows = {name: [] for name in REQUIRED_MARKERS}
    malformed = []
    capped = False
    collisions = []
    for line_number, line in enumerate(text.splitlines(), 1):
        message = marker_message(line)
        if not message:
            continue
        matched = next((name for name in REQUIRED_MARKERS if message == name or message.startswith(name + " ")), None)
        if not matched:
            if any(name in message for name in REQUIRED_MARKERS):
                collisions.append({"line": line_number, "message": redact_text(message)[:300]})
            continue
        fields = parse_key_values(message[len(matched):])
        if any(fields.get(key) == "1" for key in ("capped", "truncated", "markersCapped")):
            capped = True
        missing = [key for key in MARKER_FIELDS[matched] if key not in fields]
        if missing:
            malformed.append({"line": line_number, "marker": matched, "missing": missing})
            continue
        invalid = []
        if matched in ("PROOF-INPUT", "PROOF-FRAME"):
            try:
                int(fields["qpc"])
            except ValueError:
                invalid.append("qpc")
            try:
                parse_utc(fields["utc"])
            except ProofError:
                invalid.append("utc")
        if matched == "PROOF-FRAME":
            invalid.extend(key for key in ("presented", "displayed", "drawn") if fields[key] not in ("0", "1"))
        if invalid:
            malformed.append({"line": line_number, "marker": matched, "invalid": invalid})
            continue
        rows[matched].append({"line": line_number, **fields})
    return {"rows": rows, "counts": {name: len(items) for name, items in rows.items()},
            "malformed": malformed, "capped": capped, "substring_collisions": collisions}


def write_timeline_csv(path: Path, columns: tuple[str, ...], rows: Iterable[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def refresh_timelines(directory: Path, write: bool = True) -> dict:
    stdout_path = directory / "raw" / "stdout.log"
    stderr_path = directory / "raw" / "stderr.log"
    text = ""
    for path in (stdout_path, stderr_path):
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace") + "\n"
    parsed = parse_runtime_markers(text)
    if write:
        write_timeline_csv(directory / "timelines/frames.csv", FRAME_COLUMNS, parsed["rows"]["PROOF-FRAME"])
        write_timeline_csv(directory / "timelines/inputs.csv", INPUT_COLUMNS, parsed["rows"]["PROOF-INPUT"])
        clock = read_json(directory / "evidence/clock.json")
        marks = clock.get("marks", [])
        write_timeline_csv(directory / "timelines/marks.csv", ("label", "utc", "qpc"), marks)
        if not (directory / "timelines/events.csv").exists():
            write_timeline_csv(directory / "timelines/events.csv", EVENT_COLUMNS, [])
    state = read_json(directory / "state.json")
    for row in parsed["rows"]["PROOF-SESSION"]:
        expected = {"session_id": state.get("session_id"), "build": state.get("commit"), "route": state.get("route")}
        mismatches = [key for key, value in expected.items() if row.get(key) != value]
        if mismatches:
            parsed["malformed"].append({"line": row["line"], "marker": "PROOF-SESSION",
                                        "identity_mismatch": mismatches})
    parsed["required_markers_missing"] = [name for name in REQUIRED_MARKERS if parsed["counts"][name] == 0]
    if write:
        write_json(directory / "evidence/runtime_markers.json", parsed)
    return parsed


def soak_evidence(directory: Path, duration: float) -> dict:
    text = ""
    line_count = 0
    for name in ("stdout.log", "stderr.log"):
        path = directory / "raw" / name
        if path.exists():
            content = path.read_bytes()
            line_count += content.count(b"\n")
            text += content.decode("utf-8", errors="replace") + "\n"
    lowered = text.lower()
    p0 = {}
    for marker in P0_MARKERS:
        p0[marker] = sum(1 for line in text.splitlines()
                         if (marker_message(line) or "").startswith(marker))
    fatal = {token: lowered.count(token) for token in ("fatal error", "c0000005", "device removed", "assert")}
    write_json(directory / "evidence/p0.json", {"status": "draft-census", "marker_rows": p0,
                                                "note": "counts are evidence, not a compatibility claim"})
    evidence = {"status": "draft-census", "raw_duration_seconds": duration, "log_lines": line_count,
                "fatal_token_counts": fatal, "gate": "not-claimed-in-draft"}
    write_json(directory / "evidence/soak.json", evidence)
    return evidence


def ingest_session(args) -> tuple[Path, dict]:
    directory, state = load_state(args.session_id)
    if state.get("lifecycle_state") not in ("PREPARED", "RECORDED"):
        raise ProofError("ingest requires PREPARED or RECORDED state")
    if args.min_duration <= 0:
        raise ProofError("--min-duration must be positive")
    if not args.raw_finalized or not args.raw_uncut:
        raise ProofError("--raw-finalized and --raw-uncut are required for the raw contract")
    source = Path(args.raw).resolve()
    if not source.is_file() or source.suffix.lower() != ".mkv":
        raise ProofError("raw recording must be a finalized .mkv file (OBS window capture preferred)")
    destination = directory / "raw/capture.mkv"
    if destination.exists():
        raise ProofError("raw capture already exists; refusing overwrite")
    source_hash = sha256_file(source)
    source_size = source.stat().st_size
    time.sleep(0.25)
    if sha256_file(source) != source_hash or source.stat().st_size != source_size:
        raise ProofError("raw recording changed during finalization check")
    copied_hash = copy_streamed(source, destination)
    if copied_hash != source_hash or sha256_file(destination) != source_hash:
        raise ProofError("raw copy hash mismatch")

    if state.get("lifecycle_state") == "PREPARED":
        if not args.stdout or not args.stderr:
            raise ProofError("external ingest requires --stdout and --stderr")
        copy_streamed(Path(args.stdout).resolve(), directory / "raw/stdout.log")
        copy_streamed(Path(args.stderr).resolve(), directory / "raw/stderr.log")
    for index, value in enumerate(args.config):
        role, path = parse_role_path(value)
        target = directory / "raw/config" / f"{index:03d}_{role}_{path.name}"
        copy_streamed(path.resolve(), target)
    if args.effective_cvars:
        target = directory / "raw/effective_cvars.json"
        if target.exists():
            raise ProofError("effective cvars already captured; refusing overwrite")
        copy_streamed(Path(args.effective_cvars).resolve(), target)
    elif not (directory / "raw/effective_cvars.json").exists():
        write_json(directory / "raw/effective_cvars.json", {
            "status": "unavailable", "reason": "PROOF-CONFIG runtime marker is not implemented",
        })

    build = read_json(directory / "evidence/build.json")
    ffprobe = next((item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffprobe" and item.get("exists")), None)
    ffmpeg = next((item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffmpeg" and item.get("exists")), None)
    if not ffprobe or not ffmpeg:
        raise ProofError("ffprobe and ffmpeg are required")
    probe = run_ffprobe(ffprobe, destination)
    frame_probe = run_ffprobe(ffprobe, destination, True)
    write_json(directory / "raw/ffprobe/container.json", probe)
    write_json(directory / "raw/ffprobe/frames.json", frame_probe)
    timestamps = frame_timestamps(frame_probe)
    duration = media_duration(probe, timestamps)
    if duration < args.min_duration:
        raise ProofError(f"raw duration {duration:.3f}s is below required {args.min_duration:.3f}s")
    with (directory / "raw/frame_times.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("frame", "timestamp_seconds"))
        for index, timestamp in enumerate(timestamps):
            writer.writerow((index, f"{timestamp:.9f}"))
    decode_rc, decode_stdout, decode_stderr = full_decode(ffmpeg, destination)
    atomic_write(directory / "raw/decode_stdout.txt", decode_stdout.encode("utf-8"))
    atomic_write(directory / "raw/decode_stderr.txt", decode_stderr.encode("utf-8"))
    if decode_rc != 0:
        raise ProofError(f"full raw decode failed: {decode_stderr.strip()}")
    raw_contract = {
        "status": "passed", "recorder": args.recorder, "operator_finalized": True,
        "operator_uncut_attestation": True, "source_path": str(source),
        "source_sha256": source_hash, "raw_sha256": sha256_file(destination),
        "source_bytes": source_size, "raw_bytes": destination.stat().st_size,
        "duration_seconds": duration, "minimum_duration_seconds": args.min_duration,
        "frame_count": len(timestamps), "timestamps_strictly_monotonic": True,
        "full_decode_returncode": decode_rc, "trims": False, "speed_changes": False,
        "interpolation": False, "overlays_in_raw": "operator-attested absent; not machine-verifiable",
        "raw_contract_basis": "operator finalization/uncut attestation plus hash/decode/timestamp checks",
    }
    write_json(directory / "evidence/raw_contract.json", raw_contract)
    write_json(directory / "evidence/recorder.json", {
        **read_json(directory / "evidence/recorder.json"), "raw_container": "MKV",
        "recorder": args.recorder, "raw_sha256": source_hash, "duration_seconds": duration,
        "ffprobe_returncode": 0, "full_decode_returncode": decode_rc,
    })
    align_clock(directory, Path(args.obs_clock).resolve() if args.obs_clock else None,
                Path(args.marks_csv).resolve() if args.marks_csv else None)
    if args.events_csv:
        target = directory / "timelines/events.csv"
        if target.exists():
            raise ProofError("events timeline already exists; refusing overwrite")
        copy_streamed(Path(args.events_csv).resolve(), target)
    refresh_timelines(directory)
    soak_evidence(directory, duration)
    if state.get("lifecycle_state") == "PREPARED":
        transition(state, "PREPARED", "RECORDING")
        transition(state, "RECORDING", "RECORDED")
    state["proof_state"] = "DRAFT"
    save_state(directory, state)
    return directory, state


def ass_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}").replace("\n", "\\N")


def build_ass(directory: Path, duration: float, parsed: dict) -> str:
    session = read_json(directory / "session.json")
    hardware = public_hardware(read_json(directory / "evidence/hardware.json"))
    gpu = hardware.get("gpu_names", [""])[0] if hardware.get("gpu_names") else "UNAVAILABLE"
    frames = parsed["rows"]["PROOF-FRAME"]
    inputs = parsed["rows"]["PROOF-INPUT"]
    header = (
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1280\nPlayResY: 720\nWrapStyle: 2\n\n"
        "[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Proof,Arial,22,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
        "-1,0,0,0,100,100,0,0,1,2,1,1,24,24,24,1\n\n[Events]\n"
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
    )
    events = []
    step = 5.0
    count = max(1, int(duration // step) + 1)
    for index in range(count):
        start = min(index * step, max(0.0, duration - 0.1))
        end = min(start + step, duration)
        frame = frames[index] if index < len(frames) else None
        input_row = inputs[index] if index < len(inputs) else None
        frame_text = "UNAVAILABLE(draft)" if not frame else (
            f"frame outcome={frame['outcome']} attempt={frame['attempt']} presented={frame['presented']} "
            f"displayed={frame['displayed']} owner={frame['owner']} hr={frame['hresult']} drawn={frame['drawn']}")
        input_text = "UNAVAILABLE(draft)" if not input_row else (
            f"input port={input_row['port']} source={input_row['source']} packet={input_row['packet']} "
            f"buttons={input_row['buttons']} triggers={input_row['triggers']} sticks={input_row['sticks']}")
        text = (f"time={start:.1f}s  build={session['commit'][:12]}  route={session['route']}\n"
                f"GPU={gpu}\n{frame_text}\n{input_text}\nhealth=DRAFT (runtime proof markers pending)")
        events.append(f"Dialogue: 0,{start:.3f},{end:.3f},Proof,,0,0,0,,{ass_escape(text)}")
    return header + "\n".join(events) + "\n"


def make_derivative(directory: Path, parsed: dict) -> dict:
    contract = read_json(directory / "evidence/raw_contract.json")
    build = read_json(directory / "evidence/build.json")
    ffmpeg = next(item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffmpeg" and item.get("exists"))
    ffprobe = next(item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffprobe" and item.get("exists"))
    raw = directory / "raw/capture.mkv"
    derivative = directory / "derivative/annotated.mp4"
    ass = directory / "derivative/annotations.ass"
    duration = float(contract["duration_seconds"])
    atomic_write(ass, build_ass(directory, duration, parsed).encode("utf-8"))
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", "../raw/capture.mkv",
               "-map_metadata", "-1", "-map", "0:v:0", "-map", "0:a?", "-vf", "ass=annotations.ass",
               "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-c:a", "aac",
               "-movflags", "+faststart", "annotated.mp4"]
    completed = subprocess.run(command, cwd=directory / "derivative", capture_output=True,
                               text=True, timeout=900, check=False)
    if completed.returncode != 0 or not derivative.is_file():
        raise ProofError(f"annotated derivative failed: {completed.stderr.strip()}")
    decode_rc, _, decode_stderr = full_decode(ffmpeg, derivative)
    if decode_rc != 0:
        raise ProofError(f"derivative decode failed: {decode_stderr.strip()}")
    probe = run_ffprobe(ffprobe, derivative)
    derivative_duration = media_duration(probe, frame_timestamps(run_ffprobe(ffprobe, derivative, True)))
    if abs(derivative_duration - duration) > max(1.0, duration * 0.01):
        raise ProofError("annotated derivative duration differs from raw; full-length contract failed")
    raw_hash_after = sha256_file(raw)
    if raw_hash_after != contract["raw_sha256"]:
        raise ProofError("raw hash changed while producing derivative")
    result = {
        "path": "derivative/annotated.mp4", "sha256": sha256_file(derivative),
        "bytes": derivative.stat().st_size, "duration_seconds": derivative_duration,
        "raw_duration_seconds": duration, "raw_sha256_after": raw_hash_after,
        "full_decode_returncode": decode_rc, "trims": False, "speed_changes": False,
        "interpolation": False, "overlay": "ASS annotations; raw untouched",
    }
    write_json(directory / "evidence/derivative.json", result)
    return result


def infer_role(path: str) -> str:
    if path.startswith("raw/"):
        return "raw-evidence"
    if path.startswith("evidence/"):
        return "evidence"
    if path.startswith("timelines/"):
        return "timeline"
    if path.startswith("derivative/"):
        return "derivative"
    if path.startswith("public/"):
        return "public-redacted"
    return "manifest"


def package_files(directory: Path) -> list[str]:
    excluded = {"state.json", "SHA256SUMS"}
    return sorted(path.relative_to(directory).as_posix() for path in directory.rglob("*")
                  if path.is_file() and path.relative_to(directory).as_posix() not in excluded)


def artifact_entry(directory: Path, relative: str, public: bool) -> dict:
    path = directory / relative
    return {"path": relative, "role": infer_role(relative), "public": public,
            "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def build_manifests(directory: Path, state: dict, parsed: dict) -> str:
    scope = read_json(directory / "scope.json")
    hardware = public_hardware(read_json(directory / "evidence/hardware.json"))
    limitations = [
        "PROOF-SESSION/PROOF-CONFIG/PROOF-INPUT/PROOF-FRAME runtime markers are not implemented",
        "draft approval and public staging are hard-disabled",
        "empty frame/input timelines are not evidence of no frames/no input",
    ]
    if parsed["required_markers_missing"]:
        limitations.append("missing runtime markers: " + ", ".join(parsed["required_markers_missing"]))
    if parsed["capped"]:
        limitations.append("runtime marker stream reported capped/truncated")
    if read_json(directory / "evidence/clock.json").get("status") != "aligned":
        limitations.append("QPC/UTC alignment unavailable; wall clock not assumed")
    write_json(directory / "derivative/redactions.json", {
        "redactions": ["public copies redact absolute paths and device identifiers",
                       "private hardware collection omits/redacts serials, UUIDs, MACs and device IDs"],
        "draft_placeholders": ["frame identity unavailable when PROOF-FRAME is absent",
                               "input identity unavailable when PROOF-INPUT is absent"],
        "limitations": limitations,
    })
    write_json(directory / "public/scope.json", {
        "schema": SCHEMA, "proof_state": "DRAFT", "route": scope["route"],
        "claims": scope["claims"], "exclusions": scope["exclusions"],
        "commit": state["commit"], "clean_attestation": state["clean_attestation"],
        "limitations": limitations,
    })
    write_json(directory / "public/build.json", {
        "schema": SCHEMA, "proof_state": "DRAFT", "commit": state["commit"],
        "clean_attestation": state["clean_attestation"], "tool_version": TOOL_VERSION,
    })
    write_json(directory / "public/hardware.json", {"schema": SCHEMA, "proof_state": "DRAFT", **hardware})
    for name in ("frames.csv", "inputs.csv", "marks.csv", "events.csv"):
        source = directory / "timelines" / name
        target = directory / "public/timelines" / name
        atomic_write(target, redact_text(source.read_text(encoding="utf-8")).encode("utf-8"))
    write_json(directory / "public/limitations.json", {"schema": SCHEMA, "proof_state": "DRAFT",
                                                       "limitations": limitations})

    private_payload = [path for path in package_files(directory)
                       if path not in ("private_manifest.json", "public_manifest.json")]
    public_payload = [path for path in private_payload if path.startswith("public/")]
    common = {
        "schema": SCHEMA, "session_id": state["session_id"], "mode": "draft", "proof_state": "DRAFT",
        "lifecycle_state": "PACKAGED", "tool_version": TOOL_VERSION,
        "commit": state["commit"], "clean_attestation": state["clean_attestation"],
        "route": scope["route"], "scope": {"claims": scope["claims"], "exclusions": scope["exclusions"]},
        "limitations": limitations,
    }
    public_manifest = {
        **common,
        "artifacts": [artifact_entry(directory, path, True) for path in public_payload],
        "raw_included": False, "private_evidence_included": False,
        "redactions": read_json(directory / "derivative/redactions.json"),
    }
    write_json(directory / "public_manifest.json", public_manifest)
    private_files = package_files(directory)
    private_manifest = {
        **common,
        "attestation": scope["operator_attestation"],
        "artifacts": [artifact_entry(directory, path, path.startswith("public/") or path == "public_manifest.json")
                      for path in private_files],
        "runtime_marker_counts": parsed["counts"],
        "runtime_markers_capped": parsed["capped"],
        "clock_alignment": read_json(directory / "evidence/clock.json").get("status"),
    }
    write_json(directory / "private_manifest.json", private_manifest)
    sums = package_files(directory)
    atomic_write(directory / "SHA256SUMS",
                 "".join(f"{sha256_file(directory / path)}  {path}\n" for path in sums).encode("utf-8"))
    state["package_fingerprint"] = sha256_file(directory / "private_manifest.json")
    state["proof_state"] = "DRAFT"
    transition(state, "RECORDED", "PACKAGED")
    save_state(directory, state)
    return state["package_fingerprint"]


def package_session(args) -> tuple[Path, dict]:
    directory, state = load_state(args.session_id)
    if state.get("lifecycle_state") != "RECORDED":
        raise ProofError("package requires RECORDED state")
    parsed = refresh_timelines(directory)
    make_derivative(directory, parsed)
    build_manifests(directory, state, parsed)
    return directory, state


def read_sums(directory: Path) -> dict[str, str]:
    path = directory / "SHA256SUMS"
    if not path.is_file():
        raise ProofError("SHA256SUMS missing")
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise ProofError("malformed SHA256SUMS line")
        digest, relative = match.groups()
        if relative in result:
            raise ProofError("duplicate SHA256SUMS path")
        result[relative] = digest
    return result


def verify_sums(directory: Path) -> None:
    expected = read_sums(directory)
    actual = {path for path in package_files(directory)}
    if set(expected) != actual:
        missing = sorted(set(expected) - actual)
        extra = sorted(actual - set(expected))
        raise ProofError(f"package file set mismatch: missing={missing} extra={extra}")
    for relative, digest in expected.items():
        if sha256_file(directory / relative) != digest:
            raise ProofError(f"hash mismatch: {relative}")


def validate_approval(directory: Path, state: dict, approval: dict) -> list[str]:
    reasons = []
    if state.get("mode") != "verified" or state.get("proof_state") != "VERIFIED":
        reasons.append("draft mode is hard-disabled for approval")
    if approval.get("package_fingerprint") != state.get("package_fingerprint"):
        reasons.append("stale approval: package fingerprint mismatch")
    if not approval.get("reviewer"):
        reasons.append("reviewer missing")
    checklist = approval.get("checklist", {})
    for key in ("privacy", "rights", "claims", "scope"):
        if checklist.get(key) is not True:
            reasons.append(f"approval checklist failed: {key}")
    if approval.get("scope_digest") != sha256_file(directory / "public/scope.json"):
        reasons.append("stale approval: scope digest mismatch")
    try:
        if parse_utc(approval.get("approved_utc", "")) < parse_utc(state.get("updated_utc", "")):
            reasons.append("stale approval: approval predates package state")
    except ProofError:
        reasons.append("approval timestamp invalid")
    return reasons


def proof_blockers(directory: Path, parsed: dict) -> list[str]:
    blockers = ["draft mode: runtime proof markers are not implemented"]
    blockers.extend("missing " + marker for marker in parsed["required_markers_missing"])
    if parsed["capped"]:
        blockers.append("runtime marker stream capped/truncated")
    if parsed["malformed"]:
        blockers.append("malformed runtime marker records")
    if read_json(directory / "evidence/clock.json").get("status") != "aligned":
        blockers.append("QPC/UTC clock alignment unavailable")
    if state_clean_attestation(directory) != "clean":
        blockers.append("orchestrator did not attest a clean tree")
    return blockers


def state_clean_attestation(directory: Path) -> str:
    state = read_json(directory / "state.json")
    return state.get("clean_attestation", "unknown")


def verify_session(args) -> tuple[Path, dict, list[str]]:
    directory, state = load_state(args.session_id)
    if state.get("lifecycle_state") not in ("PACKAGED", "VERIFIED", "APPROVED", "STAGED"):
        raise ProofError("verify requires PACKAGED or later state")
    verify_sums(directory)
    private_manifest = read_json(directory / "private_manifest.json")
    public_manifest = read_json(directory / "public_manifest.json")
    if private_manifest.get("proof_state") != "DRAFT" or public_manifest.get("proof_state") != "DRAFT":
        raise ProofError("draft package manifests must state proof_state=DRAFT")
    if sha256_file(directory / "private_manifest.json") != state.get("package_fingerprint"):
        raise ProofError("private manifest fingerprint mismatch")
    if not any(item["path"] == "public_manifest.json" and item["sha256"] == sha256_file(directory / "public_manifest.json")
               for item in private_manifest.get("artifacts", [])):
        raise ProofError("private manifest does not bind the public manifest")
    for item in public_manifest.get("artifacts", []):
        reasons = public_denylist_reasons(item["path"], (directory / item["path"]).read_bytes()
                                          if (directory / item["path"]).suffix.lower() in PUBLIC_TEXT_SUFFIXES else None)
        if reasons:
            raise ProofError(f"public manifest denylist violation: {item['path']}: {reasons}")
    contract = read_json(directory / "evidence/raw_contract.json")
    if sha256_file(directory / "raw/capture.mkv") != contract["raw_sha256"]:
        raise ProofError("raw hash changed after derivative/package")
    build = read_json(directory / "evidence/build.json")
    ffmpeg = next(item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffmpeg" and item.get("exists"))
    ffprobe = next(item["path"] for item in build.get("artifacts", []) if item.get("role") == "ffprobe" and item.get("exists"))
    decode_rc, _, decode_stderr = full_decode(ffmpeg, directory / "raw/capture.mkv")
    if decode_rc != 0:
        raise ProofError(f"raw full decode failed during verify: {decode_stderr.strip()}")
    parsed = refresh_timelines(directory, write=False)
    if parsed != read_json(directory / "evidence/runtime_markers.json"):
        raise ProofError("runtime marker evidence changed during verify")
    for filename, marker in (("frames.csv", "PROOF-FRAME"), ("inputs.csv", "PROOF-INPUT")):
        with (directory / "timelines" / filename).open("r", encoding="utf-8", newline="") as source:
            actual_rows = list(csv.DictReader(source))
        expected_rows = [{key: str(row.get(key, "")) for key in (FRAME_COLUMNS if marker == "PROOF-FRAME" else INPUT_COLUMNS)}
                         for row in parsed["rows"][marker]]
        if actual_rows != expected_rows:
            raise ProofError(f"timeline does not match runtime markers: {filename}")
    approval_path = directory / "approval.json"
    if approval_path.exists():
        reasons = validate_approval(directory, state, read_json(approval_path))
        if reasons:
            raise ProofError("invalid/stale approval: " + "; ".join(reasons))
    blockers = proof_blockers(directory, parsed)
    if state.get("lifecycle_state") == "PACKAGED":
        transition(state, "PACKAGED", "VERIFIED")
        state["proof_state"] = "DRAFT"
        save_state(directory, state)
    return directory, state, blockers


def public_denylist_reasons(relative: str, content: bytes | None = None) -> list[str]:
    lowered = relative.replace("\\", "/").lower()
    reasons = []
    suffix = Path(lowered).suffix
    if suffix in (".exe", ".xex", ".rpf", ".pdb"):
        reasons.append("forbidden binary/guest artifact")
    if "mcla_pe.bin" in lowered:
        reasons.append("forbidden raw guest image")
    if "generated/" in lowered or lowered.startswith("generated"):
        reasons.append("forbidden generated tree")
    if ".secrets" in lowered or "/.env" in lowered or lowered.startswith(".env"):
        reasons.append("forbidden secret/environment file")
    if "account" in lowered and suffix == ".json":
        reasons.append("forbidden account JSON")
    if "raw/config" in lowered or lowered.endswith("config.toml") or lowered.endswith("opencode.json"):
        reasons.append("forbidden raw config")
    if "/saves/" in lowered or lowered.startswith("saves/") or "mc4.sav" in lowered:
        reasons.append("forbidden save data")
    if re.search(r"(?i)[A-Z]:\\|/(?:home|Users|root)/", relative):
        reasons.append("absolute path")
    if content is not None:
        text = content.decode("utf-8", errors="replace")
        if re.search(r"(?i)\b[A-Z]:\\|/(?:home|Users|root)/", text):
            reasons.append("absolute path in content")
        if re.search(r"(?i)PCI\\VEN_[0-9A-F]{4}&DEV_[0-9A-F]{4}|\bGPU-[0-9a-f-]{8,}\b", text):
            reasons.append("device identifier in content")
    return sorted(set(reasons))


def approve_session(args) -> None:
    directory, state = load_state(args.session_id)
    if state.get("lifecycle_state") != "VERIFIED":
        raise ProofError("approve requires VERIFIED state")
    raise ProofError("approve is hard-disabled in draft mode; missing runtime markers: " +
                     ", ".join(REQUIRED_MARKERS))


def stage_public_session(args) -> tuple[Path, dict]:
    directory, state = load_state(args.session_id)
    if state.get("lifecycle_state") != "APPROVED" or state.get("proof_state") != "VERIFIED":
        raise ProofError("stage-public requires a future VERIFIED/APPROVED package; draft staging is disabled")
    if not args.public_dir:
        raise ProofError("--public-dir is required")
    staging = Path(args.public_dir).resolve()
    if staging == directory or directory in staging.parents:
        raise ProofError("public staging directory must be outside the private session")
    if staging.exists():
        if not staging.is_dir() or any(staging.iterdir()):
            raise ProofError("public staging directory must be explicitly supplied and empty")
    else:
        staging.mkdir(parents=True)
    approval = read_json(directory / "approval.json")
    reasons = validate_approval(directory, state, approval)
    if reasons:
        raise ProofError("approval invalid: " + "; ".join(reasons))
    manifest = read_json(directory / "public_manifest.json")
    allowed = {item["path"] for item in manifest.get("artifacts", [])}
    for relative in sorted(allowed):
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise ProofError(f"public manifest path is not a safe relative path: {relative}")
        source = directory / relative
        reasons = public_denylist_reasons(relative, source.read_bytes() if source.suffix.lower() in PUBLIC_TEXT_SUFFIXES else None)
        if reasons:
            raise ProofError(f"public denylist: {relative}: {reasons}")
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    shutil.copyfile(directory / "public_manifest.json", staging / "public_manifest.json")
    staged = sorted(path.relative_to(staging).as_posix() for path in staging.rglob("*") if path.is_file())
    atomic_write(staging / "SHA256SUMS",
                 "".join(f"{sha256_file(staging / path)}  {path}\n" for path in staged).encode("utf-8"))
    transition(state, "APPROVED", "STAGED")
    save_state(directory, state)
    return staging, state


def output_state(label: str, state: dict, **extra) -> None:
    fields = {"state": state.get("proof_state", "DRAFT"), "lifecycle": state.get("lifecycle_state"), **extra}
    print(label + " " + " ".join(f"{key}={value}" for key, value in fields.items()))


def run_selftest() -> None:
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        raise ProofError("selftest requires local ffmpeg and ffprobe")
    PROOF_ROOT.mkdir(parents=True, exist_ok=True)
    session_id = "selftest-" + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix="selftest-fixtures-", dir=PROOF_ROOT) as temporary:
        root = Path(temporary)
        fixture = root / "fixtures with spaces"
        fixture.mkdir()
        exe = fixture / "mcla fixture.exe"
        exe.write_bytes(b"synthetic-executable-not-run")
        config = fixture / "config with spaces.toml"
        config.write_text("synthetic=true\n", encoding="utf-8")
        stdout = fixture / "stdout with spaces.log"
        stdout.write_text(
            f"[00:00:00.000] [info] PROOF-SESSION session_id={session_id} build=" + "a" * 40 + " route=title-selftest\n"
            "[00:00:00.100] [info] PROOF-CONFIG key=mcla_fixed_seed value=0x2\n"
            "[00:00:00.200] [info] PROOF-INPUT port=0 source=fixture instance=0 generation=1 packet=1 "
            "buttons=1 triggers=0 sticks=0 event=press qpc=1000 utc=2026-01-01T00:00:00Z\n"
            "[00:00:00.300] [info] PROOF-FRAME outcome=presented attempt=1 presented=1 displayed=1 "
            "owner=fixture hresult=0 drawn=1 qpc=1100 utc=2026-01-01T00:00:00.100Z\n",
            encoding="utf-8")
        stderr = fixture / "stderr.log"
        stderr.write_bytes(b"")
        cvars = fixture / "effective cvars.json"
        write_json(cvars, {"mcla_fixed_seed": "0x2", "fixture": True})
        clock = fixture / "obs clock.json"
        write_json(clock, {"qpc_frequency_hz": 10_000_000, "qpc_origin": 0,
                           "utc_origin": "2026-01-01T00:00:00Z"})
        events = fixture / "events.csv"
        events.write_text("utc,qpc,event,detail\n2026-01-01T00:00:00Z,0,mark,synthetic\n", encoding="utf-8")
        raw = fixture / "raw with spaces.mkv"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        "color=c=black:s=320x240:d=2", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(raw)],
                       capture_output=True, text=True, timeout=120, check=True)

        args = argparse.Namespace(session_id=session_id, route="title-selftest", commit="a" * 40,
                                  clean_attestation="clean", attested_by="selftest", exe=str(exe),
                                  map=str(fixture / "missing.map"), pdb=str(fixture / "missing.pdb"),
                                  cmake_cache=str(fixture / "missing.cache"), guest_xex=str(fixture / "missing.xex"),
                                  guest_pe=str(fixture / "missing.bin"), artifact=[], config=[f"fixture={config}"], cache=[],
                                  claim=["synthetic selftest only"])
        directory, state = prepare_session(args)
        atexit.register(shutil.rmtree, directory, ignore_errors=True)
        assert state["lifecycle_state"] == "PREPARED"
        ingest = argparse.Namespace(session_id=session_id, raw=str(raw), stdout=str(stdout), stderr=str(stderr),
                                    config=[], effective_cvars=str(cvars), min_duration=1.0, raw_finalized=True,
                                    raw_uncut=True, recorder="synthetic-ffmpeg-fixture", obs_clock=str(clock), marks_csv=None,
                                    events_csv=str(events))
        directory, state = ingest_session(ingest)
        assert state["lifecycle_state"] == "RECORDED"
        directory, state = package_session(argparse.Namespace(session_id=session_id))
        assert state["lifecycle_state"] == "PACKAGED" and state["proof_state"] == "DRAFT"
        directory, state, blockers = verify_session(argparse.Namespace(session_id=session_id))
        assert state["lifecycle_state"] == "VERIFIED" and state["proof_state"] == "DRAFT" and blockers
        try:
            approve_session(argparse.Namespace(session_id=session_id))
            raise AssertionError("draft approve was not disabled")
        except ProofError:
            pass
        try:
            stage_public_session(argparse.Namespace(session_id=session_id, public_dir=str(root / "must not exist")))
            raise AssertionError("draft staging was not disabled")
        except ProofError:
            pass
        assert not (root / "must not exist").exists()

        derivative = directory / "derivative/annotated.mp4"
        original = derivative.read_bytes()
        derivative.write_bytes(original + b"mutation")
        try:
            verify_session(argparse.Namespace(session_id=session_id))
            raise AssertionError("hash mutation was accepted")
        except ProofError:
            pass
        derivative.write_bytes(original)

        stale = directory / "approval.json"
        stale_approval = {"package_fingerprint": "0" * 64, "reviewer": "fixture",
                          "checklist": {"privacy": True, "rights": True, "claims": True, "scope": True},
                          "scope_digest": "0" * 64, "approved_utc": utc_now()}
        write_json(stale, stale_approval)
        stale_reasons = validate_approval(directory, state, stale_approval)
        assert any("stale approval" in reason for reason in stale_reasons)
        stale.unlink()

        capped = parse_runtime_markers(
            "[00:00:00.000] [info] PROOF-FRAME outcome=x attempt=1 presented=1 displayed=1 owner=x "
            "hresult=0 drawn=1 qpc=1 utc=2026-01-01T00:00:00Z capped=1\n")
        assert capped["capped"] and capped["counts"]["PROOF-FRAME"] == 1
        collision = parse_runtime_markers(
            "[00:00:00.000] [info] note mentions PROOF-FRAME outcome=x but is not a marker\n")
        assert collision["counts"]["PROOF-FRAME"] == 0 and collision["substring_collisions"]

        denied = root / "denylist fixtures"
        denied.mkdir()
        cases = {
            "build/mcla.exe": None, "guest/default.xex": None, "archive.rpf": None,
            "raw/mcla_pe.bin": None, "generated/a.cpp": None, ".secrets/key.txt": None,
            ".env": None, "account.json": None, "raw/config/settings.toml": None,
            "saves/mc4.sav": None, "public/leak.json": b'{"path":"C:\\\\secret\\\\file"}',
            "public/device.json": b'{"id":"PCI\\VEN_1234&DEV_5678"}',
        }
        for relative, content in cases.items():
            reasons = public_denylist_reasons(relative, content)
            assert reasons, relative
        try:
            assert_no_conflicting_processes([{"PID": 7, "ImageName": "mcla.exe"}])
            raise AssertionError("running mcla.exe was not refused")
        except ProofError:
            pass
        assert process_image_path(os.getpid())
        assert process_creation_time(os.getpid()) is not None
    print("SELFTEST OK")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--selftest", action="store_true")
    sub = parser.add_subparsers(dest="command")

    prepare = sub.add_parser("prepare")
    prepare.add_argument("--session-id", required=True)
    prepare.add_argument("--route", required=True)
    prepare.add_argument("--commit", required=True)
    prepare.add_argument("--clean-attestation", required=True, choices=("clean", "dirty"))
    prepare.add_argument("--attested-by", required=True)
    prepare.add_argument("--exe", required=True)
    prepare.add_argument("--map", default=str(ROOT / "build/mcla.map"))
    prepare.add_argument("--pdb", default=str(ROOT / "build/mcla.pdb"))
    prepare.add_argument("--cmake-cache", default=str(ROOT / "build/CMakeCache.txt"))
    prepare.add_argument("--guest-xex", default=str(ROOT / "build/game_data/default.xex"))
    prepare.add_argument("--guest-pe", default=str(ROOT / "build/cache/mcla_pe.bin"))
    prepare.add_argument("--artifact", action="append", default=[], metavar="ROLE=PATH")
    prepare.add_argument("--config", action="append", default=[], metavar="ROLE=PATH")
    prepare.add_argument("--cache", action="append", default=[], metavar="ROLE=PATH")
    prepare.add_argument("--claim", action="append", default=[])

    capture = sub.add_parser("capture")
    capture.add_argument("--session-id", required=True)
    capture.add_argument("--start", action="store_true")
    capture.add_argument("--stop", action="store_true")
    capture.add_argument("--operator-approved-stop", action="store_true")
    capture.add_argument("--operator")
    capture.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")

    ingest = sub.add_parser("ingest")
    ingest.add_argument("--session-id", required=True)
    ingest.add_argument("--raw", required=True)
    ingest.add_argument("--stdout")
    ingest.add_argument("--stderr")
    ingest.add_argument("--config", action="append", default=[], metavar="ROLE=PATH")
    ingest.add_argument("--effective-cvars")
    ingest.add_argument("--recorder", default="OBS")
    ingest.add_argument("--min-duration", type=float, default=180.0)
    ingest.add_argument("--raw-finalized", action="store_true")
    ingest.add_argument("--raw-uncut", action="store_true")
    ingest.add_argument("--obs-clock")
    ingest.add_argument("--marks-csv")
    ingest.add_argument("--events-csv")

    package = sub.add_parser("package")
    package.add_argument("--session-id", required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("--session-id", required=True)
    approve = sub.add_parser("approve")
    approve.add_argument("--session-id", required=True)
    approve.add_argument("--reviewer")
    approve.add_argument("--scope")
    approve.add_argument("--privacy-ok", action="store_true")
    approve.add_argument("--rights-ok", action="store_true")
    approve.add_argument("--claims-ok", action="store_true")
    stage = sub.add_parser("stage-public")
    stage.add_argument("--session-id", required=True)
    stage.add_argument("--public-dir", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        try:
            run_selftest()
            return 0
        except Exception as exc:
            print(f"SELFTEST FAILED: {exc}", file=sys.stderr)
            return 1
    if not args.command:
        parser.error("a command is required")
    try:
        if not (args.command == "capture" and args.stop):
            assert_no_conflicting_processes(list_processes())
        if args.command == "prepare":
            directory, state = prepare_session(args)
            output_state("PREPARE", state, session=state["session_id"], path=directory)
        elif args.command == "capture":
            directory, state = capture_session(args)
            output_state("CAPTURE", state, session=state["session_id"])
        elif args.command == "ingest":
            directory, state = ingest_session(args)
            output_state("INGEST", state, session=state["session_id"])
        elif args.command == "package":
            directory, state = package_session(args)
            output_state("PACKAGE", state, session=state["session_id"], fingerprint=state.get("package_fingerprint"))
        elif args.command == "verify":
            directory, state, blockers = verify_session(args)
            output_state("VERIFY", state, session=state["session_id"], blockers=len(blockers))
        elif args.command == "approve":
            approve_session(args)
        elif args.command == "stage-public":
            staging, state = stage_public_session(args)
            output_state("STAGE-PUBLIC", state, session=state["session_id"], path=staging)
        return 0
    except (ProofError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
