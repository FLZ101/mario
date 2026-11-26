"""Run against the installed Mario interpreter, not the build interpreter."""

import asyncio
import errno
import http.client
import selectors
import socket
import sys
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import HTTPServer
from threading import Event

from dns_fixture import answer
from http_fixture import BODY, Handler

LOCAL = "127.0.0.1"
HTTP_NAME = "http.mario.test"
ECHO_NAME = "echo.mario.test"
HTTP_IP = "10.0.2.100"
TIMEOUT = 4


def recv_exact(peer, size):
    data = bytearray()
    while len(data) < size:
        received = peer.recv(size - len(data))
        assert received, data
        data.extend(received)
    return bytes(data)


def tcp_peer(server):
    peer, address = server.accept()
    with peer:
        peer.settimeout(TIMEOUT)
        assert address[0] == LOCAL
        assert recv_exact(peer, len(BODY)) == BODY
        assert peer.recv(1) == b""
        peer.sendall(BODY[::-1])
        peer.shutdown(socket.SHUT_WR)


def check_tcp():
    with socket.socket() as server, ThreadPoolExecutor(1) as pool:
        server.bind((LOCAL, 0))
        server.listen()
        server.settimeout(TIMEOUT)
        result = pool.submit(tcp_peer, server)
        with socket.create_connection(server.getsockname(), TIMEOUT) as client:
            client.sendall(BODY)
            client.settimeout(0.2)
            try:
                client.recv(1)
            except TimeoutError:
                pass
            else:
                raise AssertionError("read should time out before half-close")
            client.setblocking(False)
            client.shutdown(socket.SHUT_WR)
            with selectors.DefaultSelector() as selector:
                assert isinstance(
                    selector, (selectors.PollSelector, selectors.SelectSelector)
                )
                selector.register(client, selectors.EVENT_READ)
                received = bytearray()
                while len(received) < len(BODY):
                    ready = selector.select(TIMEOUT)
                    assert ready and ready[0][1] == selectors.EVENT_READ, ready
                    chunk = client.recv(len(BODY) - len(received))
                    assert chunk
                    received.extend(chunk)
                assert bytes(received) == BODY[::-1]
                assert selector.select(TIMEOUT) and client.recv(1) == b""
        result.result(timeout=TIMEOUT)


def check_udp():
    with socket.socket(type=socket.SOCK_DGRAM) as server, socket.socket(
        type=socket.SOCK_DGRAM
    ) as client:
        server.bind((LOCAL, 0))
        client.bind((LOCAL, 0))
        server.settimeout(TIMEOUT)
        client.settimeout(TIMEOUT)
        for payload in (BODY[:13], b"", BODY):
            assert client.sendto(payload, server.getsockname()) == len(payload)
        for payload in (BODY[:13], b"", BODY):
            data, peer = server.recvfrom(2048)
            assert (data, peer) == (payload, client.getsockname())
            assert server.sendto(data, peer) == len(data)
            assert client.recvfrom(2048) == (payload, server.getsockname())


@contextmanager
def local_dns(records=None):
    if records is None:
        records = {HTTP_NAME: HTTP_IP, ECHO_NAME: LOCAL}

    def serve():
        while not done.is_set():
            try:
                query, peer = server.recvfrom(512)
            except TimeoutError:
                continue
            name, response = answer(query, records)
            names.append(name)
            server.sendto(response, peer)

    names = []
    done = Event()
    with open("/etc/resolv.conf", "w") as config:
        config.write("nameserver 127.0.0.1\noptions timeout:1 attempts:1\n")
    with socket.socket(type=socket.SOCK_DGRAM) as server, ThreadPoolExecutor(1) as pool:
        server.bind((LOCAL, 53))
        server.settimeout(0.15)
        future = pool.submit(serve)
        try:
            yield names
        finally:
            done.set()
            future.result(timeout=TIMEOUT)


def check_dns():
    assert socket.getaddrinfo(HTTP_NAME, 80, socket.AF_INET, socket.SOCK_STREAM)[0][
        4
    ] == (HTTP_IP, 80)
    try:
        socket.getaddrinfo("missing.mario.test", 80, socket.AF_INET, socket.SOCK_STREAM)
    except socket.gaierror as error:
        assert error.errno == socket.EAI_NONAME, error
    else:
        raise AssertionError("DNS unexpectedly resolved a missing name")


def check_http(port):
    connection = http.client.HTTPConnection(HTTP_IP, port, timeout=TIMEOUT)
    try:
        connection.request("GET", "/payload")
        response = connection.getresponse()
        assert response.status == 200 and response.getheader("Content-Length") == str(
            len(BODY)
        )
        assert (
            response.getheader("X-Mario-Fixture") == "net" and response.read() == BODY
        )
    finally:
        connection.close()

    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    endpoint = f"http://{HTTP_NAME}:{port}"
    with opener.open(endpoint + "/payload", timeout=TIMEOUT) as response:
        assert response.status == 200 and response.headers["X-Mario-Fixture"] == "net"
        assert response.read() == BODY
    try:
        opener.open(endpoint + "/missing", timeout=TIMEOUT)
    except urllib.error.HTTPError as error:
        with error:
            assert error.code == 404, error
    else:
        raise AssertionError("missing URL unexpectedly succeeded")
    with socket.socket() as unused:
        unused.bind((LOCAL, 0))
        try:
            opener.open(
                f"http://{LOCAL}:{unused.getsockname()[1]}/payload", timeout=TIMEOUT
            )
        except urllib.error.URLError as error:
            assert getattr(error.reason, "errno", None) == errno.ECONNREFUSED, error
        else:
            raise AssertionError("closed port unexpectedly accepted a connection")
    try:
        opener.open(endpoint + "/slow", timeout=0.3)
    except TimeoutError:
        pass
    else:
        raise AssertionError("slow response did not time out")


async def check_async_tcp():
    async def echo(reader, writer):
        try:
            writer.write(await reader.readexactly(len(BODY)))
            await writer.drain()
            assert await reader.read() == b""
        finally:
            writer.close()
            await writer.wait_closed()

    tasks = []

    def accepted(reader, writer):
        tasks.append(asyncio.create_task(echo(reader, writer)))

    loop = asyncio.get_running_loop()
    server = await asyncio.start_server(accepted, LOCAL, 0)
    async with server:
        port = server.sockets[0].getsockname()[1]
        addresses = await loop.getaddrinfo(
            ECHO_NAME, port, family=socket.AF_INET, type=socket.SOCK_STREAM
        )
        assert addresses[0][4] == (LOCAL, port)
        for interrupted in (True, False):
            reader, writer = await asyncio.open_connection(*addresses[0][4])
            try:
                if interrupted:
                    try:
                        await asyncio.wait_for(reader.read(1), 0.15)
                    except TimeoutError:
                        pass
                    else:
                        raise AssertionError("async stream read did not time out")
                    pending = asyncio.create_task(reader.read(1))
                    await asyncio.sleep(0)
                    assert not pending.done()
                    pending.cancel()
                    try:
                        await pending
                    except asyncio.CancelledError:
                        pass
                    else:
                        raise AssertionError("pending read was not cancelled")
                writer.write(BODY)
                await writer.drain()
                assert await reader.readexactly(len(BODY)) == BODY
            finally:
                writer.close()
                await writer.wait_closed()
        await asyncio.gather(*tasks)
        assert len(tasks) == 2 and all(task.done() for task in tasks)


async def check_async_udp():
    loop = asyncio.get_running_loop()

    class Endpoint(asyncio.DatagramProtocol):
        def __init__(self, echo):
            self.echo = echo
            self.incoming = loop.create_future()
            self.closed = loop.create_future()

        def connection_made(self, transport):
            self.transport = transport

        def datagram_received(self, data, peer):
            self.incoming.set_result((data, peer))
            if self.echo:
                self.transport.sendto(data[::-1], peer)

        def connection_lost(self, error):
            assert error is None
            self.closed.set_result(None)

    server, echo = await loop.create_datagram_endpoint(
        lambda: Endpoint(True), local_addr=(LOCAL, 0)
    )
    try:
        client, peer = await loop.create_datagram_endpoint(
            lambda: Endpoint(False), local_addr=(LOCAL, 0)
        )
        try:
            client.sendto(BODY, server.get_extra_info("sockname"))
            assert (await echo.incoming) == (BODY, client.get_extra_info("sockname"))
            assert (await peer.incoming) == (
                BODY[::-1],
                server.get_extra_info("sockname"),
            )
        finally:
            client.close()
            await peer.closed
    finally:
        server.close()
        await echo.closed


async def check_asyncio():
    await asyncio.wait_for(check_async_tcp(), 6)
    await asyncio.wait_for(check_async_udp(), 4)
    assert asyncio.all_tasks() == {asyncio.current_task()}


def main():
    port = int(sys.argv[1])
    check_tcp()
    check_udp()
    with local_dns() as names:
        check_dns()
        check_http(port)
        asyncio.run(check_asyncio())
        assert names.count(HTTP_NAME) >= 2 and {"missing.mario.test", ECHO_NAME} <= set(
            names
        ), names
        with HTTPServer(("10.0.2.15", port), Handler) as server:
            server.timeout = TIMEOUT
            print("PY_NET_READY", flush=True)
            server.handle_request()
    print("PY_NET_OK")


if __name__ == "__main__":
    main()
