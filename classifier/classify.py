"""Classification layer (prototype): turns a measured emitter into a label with a confidence. Pure rules on measured bandwidth,
shape, burstiness, frequency band (prior) and known board-clock harmonics."""
import numpy as np
from detect4 import clock_harmonic

BANDS = {  # name: (low_Hz, high_Hz)
 'FM broadcast':(87.5e6,108.0e6),'DAB':(174e6,240e6),'DVB-T':(470e6,694e6),'LTE800 DL':(791e6,821e6),'LTE800 UL':(832e6,862e6),
 'SRD 868':(863e6,870e6),'GSM900 UL':(880e6,915e6),'GSM900 DL':(925e6,960e6),'GSM1800 UL':(1710e6,1785e6),'GSM1800 DL':(1805e6,1880e6),
 'UMTS2100 DL':(2110e6,2170e6),'ISM 2.4':(2400e6,2483.5e6),'LTE2600 DL':(2620e6,2690e6),'ISM 5':(5150e6,5875e6),'ISM 433':(433.05e6,434.79e6)}
def band_of(f):
    return [n for n,(a,b) in BANDS.items() if a<=f<=b]

LTE_OBW = [(1.4e6,1.08e6),(3e6,2.7e6),(5e6,4.5e6),(10e6,9.0e6),(15e6,13.5e6),(20e6,18.0e6)]

def classify(e, intervals_seen=1):
    f=e['f_hz']; bands=band_of(f); bw=e['bw_hz']; kind=e['kind']; burst=e.get('burst_db',0.0); snr=e['peak_over_floor']
    notes=[]
    def conf(base,*adds): return float(max(0.05,min(0.97,base+sum(adds))))
    if kind=='narrow':
        bw3=e['bw3_hz']; bw26=e['bw26_hz']; prom=e['prom_db']; centre_lo=abs(f-e.get('fc_hz',f))<12e3
        h=clock_harmonic(f)
        if abs(f-e.get('fc_hz',-1e18))<20e3 and bw3<20e3 and prom>12 and burst<4.0 and not h:
            return ('Carrier on the LO frequency (LO leakage or board spur)','spur',conf(0.6,0.1 if prom>30 else 0))
        on_mhz=abs(f/1e6-round(f/1e6))*1e6<3e3          # a carrier sitting on an exact integer MHz
        fm_raster=abs(f/1e5-round(f/1e5))*1e5<10e3        # within 10 kHz of the 100 kHz FM raster
        gsm_band=[b for b in bands if b.startswith('GSM')]
        gsm_raster=bool(gsm_band) and abs((f-935.0e6)/0.2e6-round((f-935.0e6)/0.2e6))*0.2e6<15e3 or bool(gsm_band) and abs((f-1805.0e6)/0.2e6-round((f-1805.0e6)/0.2e6))*0.2e6<15e3
        if bw3<20e3 and burst<4.0 and prom>12 and (h or (on_mhz and prom>20)):
            why='%d x %.2f MHz'%(h[1],h[0]/1e6) if h else 'exact %.0f MHz'%round(f/1e6)
            return ('Board clock harmonic (%s)'%why,'spur',conf(0.75 if h else 0.6,0.1 if prom>30 else 0,0.05 if intervals_seen>2 else 0))
        if 'FM broadcast' in bands and prom>=12 and bw3>=8e3 and bw<=320e3 and (fm_raster or bw26>=100e3):
            return ('FM broadcast','fm',conf(0.55,0.15 if prom>25 else 0,0.15 if fm_raster else 0,0.1 if bw26>=120e3 else 0,0.05 if intervals_seen>2 else 0))
        if gsm_band and (prom>=15 or (gsm_raster and prom>=8)) and burst<12 and bw<=400e3 and (gsm_raster or 150e3<=bw<=300e3):
            return ('GSM carrier (200 kHz)','gsm',conf(0.5,0.2 if gsm_raster else 0,0.1 if 150e3<=bw<=300e3 else 0,0.1 if prom>30 else 0,0.1 if burst<4 else 0))
        if burst>=10 and bw<=400e3:
            kindb='LoRa-like' if 90e3<=bw<=400e3 else 'FSK/narrowband'
            return ('Burst transmitter (%s, ~%.0f kHz)'%(kindb,bw/1e3),'burst',conf(0.45,0.15 if any(b.startswith(('SRD','ISM')) for b in bands) else 0,0.1 if burst>20 else 0))
        if bw3<=8e3 and burst<4 and prom>=15:
            return ('Carrier (CW)','cw',conf(0.5,0.1 if prom>30 else 0))
        if bw<=40e3: return ('Narrowband signal (%.0f kHz)'%(bw/1e3),'narrow',conf(0.35))
        return ('Signal (%.0f kHz)'%(bw/1e3),'unk',conf(0.3))
    # wide blocks: match the occupied bandwidth to known channel widths
    lte_bands=[b for b in bands if b.startswith(('LTE','GSM','UMTS'))]
    best=None
    for nom,obw in LTE_OBW:
        err=abs(bw-obw)/obw
        if err<0.12 and (best is None or err<best[0]): best=(err,nom)
    if best and lte_bands:
        return ('LTE %g MHz'%(best[1]/1e6),'lte',conf(0.6,0.2*(1-best[0]/0.12),0.1 if e['flat_db']<6 else 0,0.05 if e.get('clipped') is False else 0))
    if 'DVB-T' in bands and 6.5e6<=bw<=8.2e6: return ('DVB-T/T2 (8 MHz channel)','dvbt',conf(0.7,0.1))
    if 'DAB' in bands and 1.3e6<=bw<=1.7e6: return ('DAB ensemble (1.5 MHz)','dab',conf(0.7,0.1))
    if any(b.startswith('ISM') for b in bands) and 14e6<=bw<=24e6: return ('Wi-Fi (20 MHz OFDM)','wifi',conf(0.6,0.1))
    if best: return ('OFDM-like block, ~%g MHz'%(best[1]/1e6),'ofdm',conf(0.4))
    return ('Wideband signal (%.2f MHz)'%(bw/1e6),'wide',conf(0.3))
