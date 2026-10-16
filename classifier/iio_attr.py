#!/usr/bin/env python3
"""Tiny libiio network-protocol client (no libiio needed): read/write one IIO device attribute on the radio."""
import socket,sys
H='192.168.2.1'
def _line(s):
    b=b''
    while not b.endswith(b'\n'): b+=s.recv(1)
    return b.decode().strip()
def read(dev,attr):
    s=socket.create_connection((H,30431),timeout=5); s.sendall(('READ %s %s\r\n'%(dev,attr)).encode()); n=int(_line(s))
    if n<0: return None
    d=b''
    while len(d)<n: d+=s.recv(n-len(d))
    s.close(); return d.decode().strip()
def write(dev,attr,val):
    s=socket.create_connection((H,30431),timeout=5); v=str(val).encode()
    s.sendall(('WRITE %s %s %d\r\n'%(dev,attr,len(v))).encode()+v); r=int(_line(s)); s.close(); return r
if __name__=='__main__':
    dev,attr=sys.argv[1],sys.argv[2]
    if len(sys.argv)>3: print('write rc',write(dev,attr,sys.argv[3]))
    print(read(dev,attr))
