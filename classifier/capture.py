#!/usr/bin/env python3
"""Capture /waterfall frames from the radio at a series of centre frequencies / sample rates and save them with metadata.
Radio settings are restored at the end. Usage: capture.py [name:fc_MHz:fs_MHz ...]"""
import socket,os,base64,struct,time,json,sys,urllib.request
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
orig=api('GET','ad9361'); spec0=api('GET','spectrometer'); print('original:',orig,'| spectrometer',spec0)
try:
    for item in sys.argv[1:]:
        name,fc,fs=item.split(':'); fc=float(fc)*1e6; fs=float(fs)*1e6
        api('PATCH','ad9361',{'sampling_frequency':int(fs),'rx_rf_bandwidth':int(min(fs*0.8,56e6)),'rx_lo_frequency':int(fc),'rx_gain_mode':'SlowAttack'})
        api('PATCH','spectrometer',{'output_sampling_frequency':10.0}); time.sleep(2.5)
        st=api('GET','ad9361'); sp=api('GET','spectrometer')
        fr=frames(40)
        if not fr: print(name,'no frames'); continue
        n=len(fr[0])//4
        open('captures/%s.bin'%name,'wb').write(b''.join(f[:n*4] for f in fr if len(f)>=n*4))
        json.dump({'name':name,'fc_hz':st['rx_lo_frequency'],'fs_hz':st['sampling_frequency'],'rf_bw_hz':st['rx_rf_bandwidth'],'gain_db':st['rx_gain'],'gain_mode':st['rx_gain_mode'],'floats_per_frame':n,'frames':len(fr),'fft_size':sp['fft_size'],'spectrometer_fps':sp['output_sampling_frequency'],'integrations':sp['number_integrations']},open('captures/%s.json'%name,'w'))
        print('%-10s fc %8.3f MHz fs %6.2f MS/s gain %s (%s) -> %d frames x %d floats'%(name,st['rx_lo_frequency']/1e6,st['sampling_frequency']/1e6,st['rx_gain'],st['rx_gain_mode'],len(fr),n))
finally:
    api('PATCH','ad9361',{'sampling_frequency':int(orig['sampling_frequency']),'rx_rf_bandwidth':int(orig['rx_rf_bandwidth']),'rx_lo_frequency':int(orig['rx_lo_frequency']),'rx_gain_mode':orig['rx_gain_mode']})
    if orig['rx_gain_mode'].lower()=='manual': api('PATCH','ad9361',{'rx_gain':orig['rx_gain']})
    api('PATCH','spectrometer',{'output_sampling_frequency':spec0['output_sampling_frequency']})
    print('restored:',api('GET','ad9361'))
