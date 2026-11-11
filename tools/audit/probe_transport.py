#!/usr/bin/env python3
"""Bounded direct-LAN TCP echo probe; no input capture/injection. Requires ssh mac.
Measures Python/socket RTT, not Deskflow or application latency.
"""
import json, secrets, shlex, socket, subprocess, time

def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise EOFError('peer disconnected')
        data.extend(part)
    return bytes(data)

server = '''import socket,sys
s=socket.socket();s.settimeout(20);s.bind(('0.0.0.0',0));s.listen(1)
print(s.getsockname()[1],flush=True)
c,_=s.accept();c.settimeout(10);c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
def read(n):
 b=b''
 while len(b)<n:
  p=c.recv(n-len(b))
  if not p: raise EOFError()
  b+=p
 return b
assert read(64).decode()==sys.argv[1]
try:
 while True:
  n=int.from_bytes(read(4),'big')
  if not 0<n<=4096: break
  c.sendall(read(n))
except EOFError: pass
finally: c.close();s.close()
'''
config = subprocess.check_output(['ssh','-G','mac'], text=True)
host = next(line.split()[1] for line in config.splitlines() if line.startswith('hostname '))
token = secrets.token_hex(32)
command = shlex.join(['python3','-u','-c',server,token])
proc = subprocess.Popen(['ssh','-o','BatchMode=yes','mac',command], stdout=subprocess.PIPE, text=True)
try:
    port = int(proc.stdout.readline())
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        sock.sendall(token.encode())
        results = []
        for size in (16,256,4096):
            payload=bytes((i%251 for i in range(size)))
            packet=size.to_bytes(4,'big')+payload
            timings=[]
            for i in range(1100):
                start=time.perf_counter_ns()
                sock.sendall(packet)
                assert receive(sock,size)==payload
                elapsed=(time.perf_counter_ns()-start)/1e6
                if i>=100: timings.append(elapsed)
            timings.sort()
            results.append(dict(bytes=size,samples=len(timings),p50_ms=timings[499],p95_ms=timings[949],p99_ms=timings[989],max_ms=timings[-1]))
    proc.wait(timeout=15)
    assert proc.returncode==0
    print(json.dumps(dict(kind='direct LAN TCP_NODELAY Python echo RTT; no TLS or Deskflow',results=results),indent=2))
finally:
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=15)
