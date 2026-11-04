# fmt: off

import os
import socket
import subprocess
import time

import pytest
from conftest import ShellTest, assert_loaded


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Server:
    """A quack server in its own process, kept alive by an open stdin"""

    def __init__(self, shell, tmp_path, name, port, extra_statements=()):
        init_file = tmp_path / f"init_{name}.sql"
        statements = [
            "LOAD httpfs;",
            "LOAD quack;",
            f"CALL quack_serve('quack:localhost:{port}', token='{name}_token');",
        ]
        statements.extend(extra_statements)
        init_file.write_text("\n".join(statements) + "\n")
        self.port = port
        self.process = subprocess.Popen(
            [shell, "--batch", str(tmp_path / f"{name}.db"), "-init", str(init_file)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def wait_until_listening(self, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"server on port {self.port} exited: {self.process.communicate()[1]}")
            # the server may listen on either stack
            for family, host in ((socket.AF_INET6, "::1"), (socket.AF_INET, "127.0.0.1")):
                with socket.socket(family, socket.SOCK_STREAM) as sock:
                    sock.settimeout(0.5)
                    if sock.connect_ex((host, self.port)) == 0:
                        return
            time.sleep(0.1)
        raise RuntimeError(f"server on port {self.port} did not start listening")

    def stop(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()


@pytest.mark.skipif(os.name == 'nt', reason="spawns background server processes")
def test_connect_chains_over_two_hops(shell, tmp_path):
    """A client reaches hop B through hop A, without knowing anything about B.

    This is what forwarding CONNECT buys: a client that cannot speak - or even load - the backend
    it ends up talking to can still get there, because every statement it sends is passed along
    verbatim until it reaches something that understands it.
    """
    assert_loaded(shell, "quack")
    assert_loaded(shell, "httpfs")

    port_b = free_port()
    hop_b = Server(shell, tmp_path, "b", port_b, ["CREATE TABLE marker AS SELECT 'i am hop B' AS who;"])
    try:
        hop_b.wait_until_listening()
        port_a = free_port()
        hop_a = Server(
            shell,
            tmp_path,
            "a",
            port_a,
            [f"ATTACH 'quack:localhost:{port_b}' AS b (TOKEN 'b_token');"],
        )
        try:
            hop_a.wait_until_listening()
            test = (
                ShellTest(shell)
                .statement("LOAD httpfs")
                .statement("LOAD quack")
                .statement(f"ATTACH 'quack:localhost:{port_a}' AS a (TOKEN 'a_token')")
                .statement("CONNECT a")
                # the client never interprets this - hop A does, and connects onwards to hop B
                .statement("CONNECT b")
                .statement("SELECT current_database() AS hop")
                .statement("SELECT * FROM marker")
                # DISCONNECT stays local, so the client can always get back
                .statement("DISCONNECT")
                .statement("SELECT current_database() AS hop")
            )
            result = test.run()
            # only hop B has this table, so the query travelled the whole chain
            result.check_stdout("i am hop B")
            # and the client is local again after DISCONNECT
            result.check_stdout("memory")
        finally:
            hop_a.stop()
    finally:
        hop_b.stop()
