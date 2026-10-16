"""Helpers for talking to the radio: Maia REST API, /waterfall frames, IIO attributes. No side effects on import."""
import socket,os,base64,struct,time,json,urllib.request
H='192.168.2.1'
def api(method,path,body=None):
    req=urllib.request.Request('http://%s/api/%s'%(H,path),method=method,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=8) as r: return json.loads(r.read() or b'null')
def frames(n,timeout=45):
    s=socket.create_connection((H,80),timeout=6); key=base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET /waterfall HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n'%key).encode())
    r=b''
    while b'\r\n\r\n' not in r: r+=s.recv(1024)
    buf=r.split(b'\r\n\r\n',1)[1]; out=[]; s.settimeout(3.0); t0=time.time()
    while len(out)<n and time.time()-t0<timeout:
        try: buf+=s.recv(65536)
        except Exception: break
        while len(buf)>=2:
            ln=buf[1]&127; i=2
            if ln==126:
                if len(buf)<4: break
                ln=struct.unpack('>H',buf[2:4])[0]; i=4
            if len(buf)<i+ln: break
            out.append(buf[i:i+ln]); buf=buf[i+ln:]
    s.close(); return out
