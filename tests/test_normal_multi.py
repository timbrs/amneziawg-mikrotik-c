"""One normal port, three independent keys and deliberately colliding WG indices.
WG-shaped packets with real MAC1; actual Noise/AEAD tested separately by netstack.
"""
import base64
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

BIN = os.environ.get('AWG_REAL_PROXY', str(Path(__file__).resolve().parents[2] / 'builds/awg-proxy'))
PUBS = [bytes([i]) * 32 for i in (1, 2, 3)]
SERVER = bytes([9]) * 32

def key(pub):
    return hashlib.blake2s(b'mac1----' + pub).digest()

def mac(packet, pub):
    off = 116 if len(packet) == 148 else 60
    return packet[:off] + hashlib.blake2s(packet[:off], key=key(pub), digest_size=16).digest() + packet[off + 16:]

def pkt(typ, idx, receiver=0):
    n = {1:148, 2:92, 3:64, 4:128}[typ]
    return struct.pack('<III', typ, idx, receiver) + bytes(n-12)

def sock(family=socket.AF_INET):
    s = socket.socket(family, socket.SOCK_DGRAM)
    s.bind(('::1' if family == socket.AF_INET6 else '127.0.0.1', 0))
    s.settimeout(2)
    return s

def run(gro, ipv6):
    family = socket.AF_INET6 if ipv6 else socket.AF_INET
    host = '::1' if ipv6 else '127.0.0.1'
    fmt = lambda p: f'[{host}]:{p}' if ipv6 else f'{host}:{p}'
    remote = sock(family)
    reserve = sock(family)
    front = reserve.getsockname()[:2]
    reserve.close()
    clients = [sock(family) for _ in PUBS]
    env = {k:v for k,v in os.environ.items() if not k.startswith('AWG_')}
    env.update(AWG_LISTEN=fmt(front[1]), AWG_REMOTE=fmt(remote.getsockname()[1]), AWG_MODE='normal',
        AWG_JC='0', AWG_JMIN='0', AWG_JMAX='0', AWG_S1='0', AWG_S2='0', AWG_S4='0',
        AWG_H1='101', AWG_H2='102', AWG_H3='103', AWG_H4='104', AWG_SPIN='0',
        AWG_NO_GRO=str(1-gro), AWG_NO_GSO=str(1-gro), AWG_SOCKET_BUF='1048576',
        AWG_CLIENT_PUBS=','.join(base64.b64encode(x).decode() for x in PUBS),
        AWG_SERVER_PUB=base64.b64encode(SERVER).decode(), AWG_LOG_LEVEL='error')
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([BIN], env=env, stdout=log, stderr=log)
        try:
            time.sleep(.2)
            assert process.poll() is None
            # All native clients pick the SAME sender index, including retransmits.
            wires = []
            for i, client in enumerate(clients):
                init = mac(pkt(1, 77), SERVER)
                client.sendto(init, front)
                incoming, upstream = remote.recvfrom(4096)
                assert incoming == mac(incoming, SERVER)
                wire = struct.unpack_from('<I', incoming, 4)[0]
                wires.append(wire)
                assert len(set(wires)) == len(wires)
                client.sendto(init, front)
                retry, _ = remote.recvfrom(4096)
                assert retry == incoming
                response = mac(pkt(2, 900+i, wire), PUBS[i])
                awg = mac(struct.pack('<I', 102) + response[4:], PUBS[i])
                remote.sendto(awg, upstream)
                assert client.recv(4096) == mac(pkt(2, 900+i, 77), PUBS[i])
            # Interleaved packets, same IP and different source ports, batch boundary.
            for batch in range(5):
                for j in range(32):
                    i = j % 3
                    data = pkt(4, 900+i, batch*32+j)
                    clients[i].sendto(data, front)
                for j in range(32):
                    incoming, upstream = remote.recvfrom(4096)
                    server_index = struct.unpack_from('<I', incoming, 4)[0]
                    i = server_index - 900
                    remote.sendto(struct.pack('<II', 104, wires[i]) + incoming[8:], upstream)
                for i, client in enumerate(clients):
                    for _ in range(sum(j % 3 == i for j in range(32))):
                        answer = client.recv(4096)
                        assert struct.unpack_from('<II', answer) == (4, 77)
                        seq = struct.unpack_from('<I', answer, 8)[0]
                        assert seq // 32 == batch and (seq % 32) % 3 == i
            # Force real UDP segmentation (including >32-segment receive batches).
            if gro:
                packets = [struct.pack('<II', 104, wires[j % 3]) + bytes([j]) * 120 for j in range(64)]
                remote.sendmsg([b''.join(packets)], [(socket.IPPROTO_UDP, 103, struct.pack('H', 128))], 0, upstream)
                for i, client in enumerate(clients):
                    got = []
                    for _ in range(sum(j % 3 == i for j in range(64))):
                        answer = client.recv(4096)
                        assert struct.unpack_from('<II', answer) == (4, 77)
                        got.append(answer[8])
                    assert got == [j for j in range(64) if j % 3 == i]
            # Unknown index and wrong recipient MAC1 must never be delivered.
            remote.sendto(struct.pack("<II", 104, 0xfefefefe) + bytes(120), upstream)
            remote.sendto(mac(struct.pack('<I', 102) + pkt(2, 900, wires[0])[4:], PUBS[1]), upstream)
            # Server initiates a rekey: MAC1 selects a previously learned client.
            for i, client in enumerate(clients):
                init = mac(pkt(1, 1000+i), PUBS[i])
                remote.sendto(mac(struct.pack('<I', 101)+init[4:], PUBS[i]), upstream)
                assert client.recv(4096) == init
                response = mac(pkt(2, 88, 1000+i), SERVER)
                client.sendto(response, front)
                got, upstream = remote.recvfrom(4096)
                assert got == mac(got, SERVER)
                wire = struct.unpack_from('<I', got, 4)[0]
                remote.sendto(struct.pack('<II', 104, wire) + bytes(120), upstream)
                assert client.recv(4096) == struct.pack('<II', 4, 88) + bytes(120)
            for client in clients:
                client.settimeout(.05)
                try:
                    unexpected = client.recv(4096)
                    raise AssertionError(('misdelivered packet', unexpected.hex()))
                except socket.timeout:
                    pass
            print(f'PASS normal multi: IPv{6 if ipv6 else 4}, GRO/GSO={gro}, 3 keys, colliding indices, 160 interleaved data packets, server rekey')
        except Exception:
            log.seek(0)
            print(log.read().decode(errors='replace'))
            raise
        finally:
            process.terminate()
            process.wait(timeout=8)
            for s in [remote, *clients]: s.close()

if __name__ == '__main__':
    for ipv6 in (False, True):
        for gro in (0, 1): run(gro, ipv6)
