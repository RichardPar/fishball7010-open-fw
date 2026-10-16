"""Minimal MQTT 3.1.1 subscriber (no deps). usage: mqtt_watch.py <seconds> <topic-filter>..."""
import socket,struct,sys,time
def enc(s): b=s.encode(); return struct.pack('>H',len(b))+b
def rl(n):
    o=b''
    while True:
        d=n%128; n//=128; o+=bytes([d|128 if n else d])
        if not n: return o
def pkt(t,body): return bytes([t])+rl(len(body))+body
def run(secs,filters,host='192.168.2.1'):
    s=socket.create_connection((host,1883),timeout=5)
    s.sendall(pkt(0x10,enc('MQTT')+b'\x04\x02\x00\x00'+enc('cw%d'%int(time.time()))))
    s.sendall(pkt(0x82,struct.pack('>H',1)+b''.join(enc(f)+b'\x00' for f in filters)))
    s.settimeout(1.0); buf=b''; t0=time.time(); out=[]
    while time.time()-t0<secs:
        try: d=s.recv(65536)
        except socket.timeout: continue
        if not d: break
        buf+=d
        while len(buf)>=2:
            i=1; ln=0; sh=0
            while i<len(buf):
                b=buf[i]; ln|=(b&127)<<sh; sh+=7; i+=1
                if not b&128: break
            else: break
            if len(buf)<i+ln: break
            body=buf[i:i+ln]; typ=buf[0]>>4; buf=buf[i+ln:]
            if typ==3:
                tl=struct.unpack('>H',body[:2])[0]; out.append((time.time()-t0,body[2:2+tl].decode(),body[2+tl:].decode(errors='replace')))
    return out
if __name__=='__main__':
    for t,top,val in run(float(sys.argv[1]),sys.argv[2:]): print('%5.1f %s = %s'%(t,top,val[:1500]))
