"""Black-box UDP server tests; uses only Python's standard library."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
HEADER=struct.Struct('<4sBBHQIIIQ')
def encode(kind,payload=b'',session=0,race=0,seq=0,token=0):
    return HEADER.pack(b'FZVS',1,kind,len(payload),session,race,seq,0,token)+payload
def decode(data):
    h=HEADER.unpack_from(data)
    assert h[0]==b'FZVS' and h[1]==1 and h[3]==len(data)-36
    return h,data[36:]
class Peer:
    def __init__(self,port,nonce):
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.sock.connect(('127.0.0.1',port)); self.sock.settimeout(.1)
        self.nonce=nonce; self.session=self.token=self.race=self.seq=0
        self.state=None; self.ack=0; self.id=-1; self.last_ping=0
    def send(self,kind,payload=b'',seq=0,race=None):
        self.sock.send(encode(kind,payload,self.session,self.race if race is None else race,seq,self.token))
    def hello(self): self.send(1,struct.pack('<QB',self.nonce,4)+b'test')
    def poll(self):
        if time.monotonic()-self.last_ping>.4:
            if self.id<0: self.hello()
            else: self.send(6,struct.pack('<Q',int(time.monotonic()*1e6)))
            self.last_ping=time.monotonic()
        try: data=self.sock.recv(257)
        except socket.timeout:return
        h,p=decode(data)
        if h[2]==2:
            self.id=p[0]; self.session=h[4]; self.race=h[5]; self.token=h[8]
        elif h[2]==5:
            self.race=h[5]; self.state=p; self.ack=h[7]
    def command(self,kind,data=b''):
        self.seq+=1; self.send(3,bytes([kind])+data,self.seq)
        return self.seq

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1',0)); self.port=sock.getsockname()[1]
        self.output=open(self.root/'stdout','w'); self.addCleanup(self.output.close)
        self.server=subprocess.Popen([str(ROOT/'build/server/fzvs-server'),'--port',str(self.port),
            '--players','4','--room-key','test','--ready-file',str(self.root/'ready'),
            '--log-level','debug','--json-log',str(self.root/'log.jsonl')],stdout=self.output,stderr=self.output)
        self.addCleanup(self.stop)
        deadline=time.monotonic()+3
        while not (self.root/'ready').exists():
            if self.server.poll() is not None or time.monotonic()>deadline:
                self.fail((self.root/'stdout').read_text())
            time.sleep(.02)
        self.peers=[]
        for i in range(4):
            peer=Peer(self.port,i+1); self.peers.append(peer); self.addCleanup(peer.sock.close)
            peer.hello(); self.wait(lambda:peer.id==i)
    def stop(self):
        if self.server.poll() is None:self.server.terminate()
        self.server.wait(timeout=3)
    def wait(self,predicate,timeout=4):
        deadline=time.monotonic()+timeout
        while not predicate():
            self.assertIsNone(self.server.poll())
            if time.monotonic()>deadline:self.fail('UDP condition timed out\n'+(self.root/'stdout').read_text())
            for p in self.peers:p.poll()
    def phase(self,n):self.wait(lambda:all(p.state and p.state[0]==n for p in self.peers))
    def start_race(self):
        for i,p in enumerate(self.peers):p.command(1,bytes([i%4]))
        self.phase(1)
        for i,p in enumerate(self.peers):p.command(2,struct.pack('<HHB',100+i,200+i,32+i))
        self.phase(2)
        for p in self.peers:p.command(3,p.state[16:24])
        self.phase(3)
    def test_race_relay_finish_restart_and_bad_packets(self):
        self.start_race(); p=self.peers[0]
        p.send(4,struct.pack('<HHB',0x1234,0x5678,45),seq=1)
        self.wait(lambda:all(q.state and struct.unpack_from('<HH',q.state,26)==(0x1234,0x5678) for q in self.peers))
        # An older update must not rewind the car.
        p.send(4,struct.pack('<HHB',1,2,3),seq=0)
        for q in self.peers:q.command(4)
        self.phase(4)
        p.send(4,struct.pack('<HHB',999,999,0),seq=2)
        self.wait(lambda:all(q.state[24]==5 and q.state[26:30]==b'\0'*4 for q in self.peers))
        previous=p.race; p.command(5)
        self.wait(lambda:all(q.race==previous+1 for q in self.peers)); self.phase(0)
        p.send(4,b'\0'*5,seq=123,race=previous)
        for data in [b'',b'a',b'FZVS',b'x'*1000,encode(4,b'\0'*5),encode(3,b'\xff')]:p.sock.send(data)
        self.start_race()
        self.stop()
        records=[json.loads(line) for line in (self.root/'log.jsonl').read_text().splitlines()]
        self.assertTrue(any('state=loading->countdown' in r['message'] for r in records))
        self.assertTrue(any('new_lobby' in r['message'] for r in records))
    def test_host_authority_duplicate_commands_and_disconnect(self):
        host,other=self.peers[:2]
        seq=other.command(6,bytes([4,4,2]))
        self.wait(lambda:other.ack==seq)
        self.assertEqual(other.state[3:5],b'\0\0')
        seq=host.command(6,bytes([4,4,2]))
        self.wait(lambda:all(p.state and p.state[3:5]==bytes([4,2]) for p in self.peers))
        host.send(3,bytes([6,4,0,0]),seq=seq)
        self.wait(lambda:host.ack==seq)
        self.assertEqual(host.state[3:5],bytes([4,2]))
        # Dropping one socket releases its lobby slot after the heartbeat timeout.
        gone=self.peers.pop(); gone.sock.close()
        self.wait(lambda:all(p.state and p.state[72]==0 for p in self.peers),timeout=7)
        replacement=Peer(self.port,999); self.addCleanup(replacement.sock.close); self.peers.append(replacement)
        replacement.hello(); self.wait(lambda:replacement.id==3)

if __name__=='__main__':unittest.main()
