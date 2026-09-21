#!/usr/bin/env python3
"""Exercise the installed native host directly and through LaunchServices."""

import html.parser
import os
from pathlib import Path
import select
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


class Assets(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.urls = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "script" and attrs.get("src"):
            self.urls.append(attrs["src"])
        if tag == "link" and attrs.get("rel") == "stylesheet" and attrs.get("href"):
            self.urls.append(attrs["href"])


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def matching_pids(config):
    result = []
    for line in command("/bin/ps", "-ww", "-axo", "pid=,stat=,command=").splitlines():
        fields = line.split(None, 2)
        if len(fields) != 3:
            continue
        pid, status, argv = fields
        if "Z" not in status and str(config) in argv and "Contents/MacOS/Lumen Fusion" in argv:
            result.append(int(pid))
    return result


def listening(port):
    with socket.socket() as connection:
        connection.settimeout(0.3)
        return connection.connect_ex(("127.0.0.1", port)) == 0


def wait_until(predicate, seconds, description):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise RuntimeError(f"Timed out: {description}")


def validate(app, diagnostics, mode):
    state = Path(tempfile.mkdtemp(prefix=f"{mode} isolated state ", dir=diagnostics))
    home = state / "home"
    home.mkdir()
    cwd = state / "unrelated working directory"
    cwd.mkdir()
    config = state / "native-validation.conf"
    log = state / "host.log"
    # A fresh port range per launch avoids collisions with other local hosts.
    for base in range(49000, 59000, 37):
        sockets = []
        try:
            for offset in (-5, 0, 1, 9, 10, 11, 13, 21):
                sock = socket.socket()
                sockets.append(sock)
                sock.bind(("127.0.0.1", base + offset))
            break
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()
    else:
        raise RuntimeError("No free host port range")
    paths = {
        "file_state": state / "state.json",
        "file_apps": state / "apps.json",
        "credentials_file": state / "credentials.json",
        "log_path": log,
        "pkey": state / "private-key.pem",
        "cert": state / "certificate.pem",
    }
    config.write_text("".join(f"{key} = {value}\n" for key, value in paths.items()) +
                      f"port = {base}\nsystem_tray = enabled\nupnp = disabled\n")
    stdout = state / "stdout.log"
    stderr = state / "stderr.log"
    env = dict(os.environ, HOME=str(home))
    args = [str(app / "Contents/MacOS/Lumen Fusion"), str(config)]
    if mode == "launchservices":
        args = ["/usr/bin/open", "-n", "-W", "--stdout", str(stdout),
                "--stderr", str(stderr), "--env", f"HOME={home}",
                str(app), "--args", str(config)]
    process = None
    pid = None
    exit_events = None
    port = base + 1
    context = ssl._create_unverified_context()  # The fresh host generates a local certificate.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                        urllib.request.HTTPSHandler(context=context))

    def fetch(path):
        with opener.open(f"https://127.0.0.1:{port}{path}", timeout=2) as response:
            if response.status != 200 or urllib.parse.urlsplit(response.url).path != urllib.parse.urlsplit(path).path:
                raise RuntimeError(f"Unexpected response for {path}: {response.url}")
            return response.read(), response.headers.get_content_type()

    def logs():
        return log.read_text(errors="replace") if log.exists() else ""

    try:
        with stdout.open("wb") as out, stderr.open("wb") as err:
            process = subprocess.Popen(args, cwd=cwd, env=env, stdout=out, stderr=err)
        if mode == "direct":
            pid = process.pid
        else:
            def launched():
                nonlocal pid
                if process.poll() is not None:
                    raise RuntimeError("LaunchServices command exited before native PID discovery")
                candidates = matching_pids(config)
                if len(candidates) == 1:
                    pid = candidates[0]
                    return True
                return False

            wait_until(launched, 30, "LaunchServices native PID")
        # Darwin NOTE_EXITSTATUS (not exposed by Python) requests wait status in data.
        note_exitstatus = 0x04000000
        exit_events = select.kqueue()
        exit_events.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                                          flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                                          fflags=select.KQ_NOTE_EXIT | note_exitstatus)], 0, 0)
        print(f"{mode}: native PID {pid}, config {config}", flush=True)

        def ready():
            if process.poll() is not None or pid not in matching_pids(config):
                raise RuntimeError("Host exited before readiness")
            try:
                body, content_type = fetch("/welcome")
            except (OSError, urllib.error.URLError):
                return False
            if content_type != "text/html" or b"<html" not in body.lower():
                raise RuntimeError("Welcome response is not HTML")
            parser = Assets()
            parser.feed(body.decode())
            for url in parser.urls:
                parsed = urllib.parse.urlsplit(url)
                if parsed.scheme or parsed.netloc:
                    continue
                asset = urllib.parse.urljoin("/welcome", url)
                payload, kind = fetch(asset)
                if not payload or kind == "text/html":
                    raise RuntimeError(f"Referenced asset did not load: {asset}")
                if "System tray created" not in logs():
                    return False
                print(f"{mode}: HTTPS /welcome and {asset} loaded; tray created", flush=True)
                return True
            raise RuntimeError("Welcome page has no local script or stylesheet")

        wait_until(ready, 60, "native HTTPS and tray readiness")
        owners = command("/usr/sbin/lsof", "-t", f"-iTCP:{port}", "-sTCP:LISTEN").splitlines()
        if str(pid) not in owners or set(owners) != {str(pid)}:
            raise RuntimeError(f"HTTPS listener is not owned solely by native PID {pid}: {owners}")
        if pid not in matching_pids(config) or process.poll() is not None:
            raise RuntimeError("Native process did not remain alive")
        os.kill(pid, signal.SIGTERM)
        events = exit_events.control(None, 1, 8)
        if (not events or events[0].ident != pid
                or events[0].flags & select.KQ_EV_ERROR
                or not events[0].fflags & select.KQ_NOTE_EXIT
                or not events[0].fflags & note_exitstatus or events[0].data != 0):
            raise RuntimeError(f"Native process did not exit cleanly within 8 seconds: {events}")
        wait_until(lambda: pid not in matching_pids(config), 1, "native SIGTERM exit")
        if process.wait(timeout=3) != 0:
            raise RuntimeError("Launch command did not exit cleanly")
        wait_until(lambda: not listening(port), 3, "HTTPS listener close")
        if "Terminate handler called" not in logs() or "Forcing shutdown" in logs():
            raise RuntimeError("Missing graceful termination evidence")
        print(f"{mode}: native SIGTERM exit and listener close passed", flush=True)
    finally:
        if exit_events is not None:
            exit_events.close()
        # Forced cleanup never turns a failed validation into a successful result.
        try:
            survivors = matching_pids(config)
            for survivor in survivors:
                try:
                    os.kill(survivor, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            deadline = time.monotonic() + 5
            while matching_pids(config) and time.monotonic() < deadline:
                time.sleep(0.2)
            for survivor in matching_pids(config):
                print(f"Failure cleanup requires SIGKILL for PID {survivor}", file=sys.stderr)
                try:
                    os.kill(survivor, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


if __name__ == "__main__":
    bundle = Path(sys.argv[1]).resolve(strict=True)
    output = Path(sys.argv[2]).resolve(strict=True)
    for launch_mode in ("direct", "launchservices"):
        validate(bundle, output, launch_mode)
