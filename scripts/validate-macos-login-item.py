#!/usr/bin/env python3
"""Exercise real SMAppService registration in the disposable macOS CI account."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def validate(source: Path, diagnostics: Path) -> None:
    if sys.platform != "darwin" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("Run this mutating login-item check only in the disposable macOS Actions account")
    diagnostics.mkdir(parents=True, exist_ok=True)
    applications = Path.home() / "Applications"
    applications.mkdir(exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="Lumen Fusion login check ", dir=applications))
    app = staging / "Lumen Fusion.app"
    host = app / "Contents/MacOS/Lumen Fusion"
    transcript = []
    may_have_registered = False

    def command(action: str):
        result = subprocess.run([str(host), "--login-item", action], capture_output=True,
                                text=True, timeout=30)
        transcript.append(f"ACTION {action}; exit={result.returncode}\n{result.stdout}\n{result.stderr}\n")
        states = re.findall(r"^login-item status: ([A-Za-z_]+)\s*$", result.stdout, re.MULTILINE)
        if len(states) != 1:
            raise RuntimeError(f"Missing or ambiguous status for {action}: {result.stdout}\n{result.stderr}")
        state = {"notRegistered": "not_registered", "requiresApproval": "requires_approval", "notFound": "not_found"}.get(states[0], states[0])
        return result.returncode, state

    try:
        subprocess.run(["/usr/bin/ditto", str(source), str(app)], check=True)
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        code, initial = command("status")
        if code or initial != "not_registered":
            raise RuntimeError(f"Refusing to modify a pre-existing or unknown login item: {initial} (exit {code})")
        # Explicit registration in a disposable account, never an approval bypass.
        may_have_registered = True
        code, registered = command("enable")
        if registered not in {"enabled", "requires_approval"} or (code and registered != "requires_approval"):
            raise RuntimeError(f"Native registration failed: {registered} (exit {code})")
        print(f"Real login item registered: {registered}; command exit={code}", flush=True)
        code, repeated = command("enable")
        if code or repeated not in {"enabled", "requires_approval"}:
            raise RuntimeError(f"Idempotent enable failed: {repeated} (exit {code})")
        code, removed = command("disable")
        if code or removed != "not_registered":
            raise RuntimeError(f"Native unregistration failed: {removed} (exit {code})")
        code, repeated = command("disable")
        if code or repeated != "not_registered":
            raise RuntimeError(f"Idempotent disable failed: {repeated} (exit {code})")
        code, final = command("status")
        if code or final != initial:
            raise RuntimeError(f"Original unregistered state was not restored: {final} (exit {code})")
        may_have_registered = False
        print("Real SMAppService enable/disable roundtrip passed; original state restored.", flush=True)
        print("No logout/login or human approval was simulated; pending approval remains pending.", flush=True)
    finally:
        if may_have_registered and host.exists():
            try:
                code, state = command("disable")
                if code or state != "not_registered":
                    raise RuntimeError(f"Cleanup could not unregister the test item: {state} (exit {code})")
            finally:
                (diagnostics / "login-item.log").write_text("\n".join(transcript))
        else:
            (diagnostics / "login-item.log").write_text("\n".join(transcript))
        shutil.rmtree(staging)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: validate-macos-login-item.py APP DIAGNOSTICS")
    validate(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve())
