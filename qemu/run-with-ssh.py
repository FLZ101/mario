import json
import re
import signal
import socket
import subprocess
import sys


def configure_forward(monitor, requested_port):
    def read_message():
        line = stream.readline()
        if not line:
            raise RuntimeError("QEMU closed the startup monitor")
        return json.loads(line)

    def request(command, **arguments):
        monitor.sendall(
            json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n"
        )
        while True:
            response = read_message()
            if "error" in response:
                raise RuntimeError(response["error"]["desc"])
            if "return" in response:
                return response["return"]

    def hmp(command):
        return request("human-monitor-command", **{"command-line": command})

    with monitor.makefile("rb") as stream:
        read_message()
        request("qmp_capabilities")
        ports = ("2222", "0") if requested_port == "auto" else (requested_port,)
        for port in ports:
            error = hmp(f"hostfwd_add tcp:127.0.0.1:{port}-:22").strip()
            if not error:
                break
        else:
            raise RuntimeError(error)

        forward = re.search(
            r"^\s*TCP\[HOST_FORWARD\]\s+\d+\s+127\.0\.0\.1\s+(\d+)\s+\S+\s+22\s",
            hmp("info usernet"),
            re.MULTILINE,
        )
        if not forward:
            raise RuntimeError("QEMU did not report the SSH forwarding port")
        return int(forward[1])


def stop_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


def run(requested_port, command):
    signal.signal(signal.SIGTERM, stop_on_signal)
    signal.signal(signal.SIGHUP, stop_on_signal)
    monitor, endpoint = socket.socketpair()
    with monitor, endpoint:
        monitor.settimeout(30)
        fd = endpoint.fileno()
        command += [
            "-chardev",
            f"socket,id=mario-ssh-monitor,fd={fd}",
            "-mon",
            "chardev=mario-ssh-monitor,mode=control",
        ]
        with subprocess.Popen(command, pass_fds=(fd,)) as qemu:
            endpoint.close()
            try:
                port = configure_forward(monitor, requested_port)
                monitor.close()
                print(f"SSH forward: 127.0.0.1:{port} -> guest TCP port 22", flush=True)
                status = qemu.wait()
                return status if status >= 0 else 128 - status
            finally:
                if qemu.poll() is None:
                    qemu.terminate()
                    try:
                        qemu.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        qemu.kill()


if __name__ == "__main__":
    try:
        sys.exit(run(sys.argv[1], sys.argv[2:]))
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(f"QEMU SSH forward: {error}")
    except KeyboardInterrupt:
        sys.exit(130)
