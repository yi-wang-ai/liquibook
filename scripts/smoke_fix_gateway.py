import pathlib
import signal
import subprocess
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="liquibook-fix-smoke-") as directory:
    root = pathlib.Path(directory)
    (root / "sessions").mkdir()
    settings = pathlib.Path("/workspace/examples/fix/acceptor.cfg").read_text()
    (root / "server.cfg").write_text(settings.replace("run/fix-sessions", str(root / "sessions")))
    process = subprocess.Popen(
        ["/workspace/build/fix-Release/fix_gateway", str(root / "server.cfg"), str(root / "gateway")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    try:
        time.sleep(1)
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        output, _ = process.communicate(timeout=10)
        print(output, end="")
        assert process.returncode == 0, process.returncode
        assert "FIX 4.4 gateway running" in output
        assert (root / "gateway/adapter.journal").is_file()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
