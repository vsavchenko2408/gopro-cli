#!/usr/bin/env python3
"""End-to-end test: run the real `gopro` binary against the camera emulator.

    python3 tests/smoke_test.py build/gopro

Works on Linux, macOS and Windows. Exits non-zero on the first failure.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: smoke_test.py <path to gopro binary>")
    binary = sys.argv[1]
    port = free_port()
    work = Path(tempfile.mkdtemp(prefix="gopro-test-"))

    env = dict(os.environ, GOPRO_HOST="127.0.0.1", GOPRO_PORT=str(port),
               GOPRO_MEDIA_PORT=str(port),
               # keep the schema cache out of the user's real cache folder
               XDG_CACHE_HOME=str(work), LOCALAPPDATA=str(work), HOME=str(work))

    mock = subprocess.Popen([sys.executable, str(ROOT / "tools" / "mock_camera.py"), str(port)],
                            stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):  # wait until the emulator accepts connections
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            sys.exit("the camera emulator did not start")

        def run(*args, expect=0):
            result = subprocess.run([binary, *args], env=env, capture_output=True, text=True,
                                    timeout=60)
            output = result.stdout + result.stderr
            if result.returncode != expect:
                sys.exit(f"FAIL: gopro {' '.join(args)} -> exit {result.returncode}, "
                         f"expected {expect}\n{output}")
            print(f"ok: gopro {' '.join(args)}")
            return output

        def check(condition, what, output):
            if not condition:
                sys.exit(f"FAIL: {what}\n{output}")

        out = run("info")
        check("HERO7 Black" in out, "info shows the model", out)

        out = run("status")
        check("Battery:    84%" in out and "Video / Video" in out, "status summary", out)

        out = run("settings", "video")
        check("video.resolution" in out and "1080" in out, "settings list", out)

        out = run("set", "video.fov", "superview")
        check("video.fov = Superview" in out, "set by option name", out)
        out = run("get", "video.fov")
        check("* Superview" in out, "get marks the current option", out)
        run("set", "video.fov", "no-such-option", expect=2)

        out = run("mode", "photo", "night")
        check("Photo / Night" in out, "mode with sub-mode", out)

        out = run("record", "start")
        check("Recording started" in out, "record start", out)
        out = run("status")
        check("Recording:  yes" in out, "status while recording", out)
        run("tag")
        out = run("record", "stop")
        check("Recording stopped" in out, "record stop", out)
        out = run("record", "stop")
        check("Not recording" in out, "stop when idle", out)

        # The emulator is "busy" for a moment after a recording, like the camera.
        out = run("photo")
        check("Photo taken" in out, "photo right after a recording", out)

        out = run("media", "ls")
        check("GH010001.MP4" in out and "2 files" in out, "media list", out)
        target = work / "downloads"
        run("media", "get", "all", "-o", str(target))
        check((target / "GH010001.MP4").stat().st_size == 3_000_000, "downloaded file size", "")
        out = run("media", "get", "last", "-o", str(target))
        check("already downloaded" in out, "second download is skipped", out)
        run("media", "rm", "GOPR0002.JPG", "--yes")
        out = run("media", "ls")
        check("GOPR0002.JPG" not in out, "file deleted", out)

        run("time")
        run("zoom", "50")
        run("no-such-command", expect=2)
        print("all smoke tests passed")
    finally:
        mock.terminate()
        mock.wait(timeout=10)


if __name__ == "__main__":
    main()
