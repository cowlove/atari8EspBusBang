#!/usr/bin/env python3
"""A plain TCP forwarder, for fault injection in csim tests.

    tcp_proxy.py <listen_port> <host> <port>

Listens on 127.0.0.1:<listen_port> and forwards each connection to
<host>:<port>. Stop the process (SIGSTOP) to freeze every connection (the
peer's requests time out), continue it (SIGCONT) to resume, kill it to
drop them all (and refuse new ones until it is started again).
csim/a800/smb_outage_test.sh drives it.
"""
import asyncio
import sys


async def pipe(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, OSError):
        pass
    finally:
        writer.close()


async def handle(client_reader, client_writer, host, port):
    try:
        server_reader, server_writer = await asyncio.open_connection(host, port)
    except OSError:
        client_writer.close()
        return
    await asyncio.gather(pipe(client_reader, server_writer), pipe(server_reader, client_writer))


async def main():
    listen_port, host, port = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
    server = await asyncio.start_server(lambda r, w: handle(r, w, host, port), "127.0.0.1", listen_port,
                                        reuse_address=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
