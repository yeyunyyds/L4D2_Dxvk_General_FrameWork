"""Optional bounded Windows GPU Engine PDH sampling; never gates rendering."""
import ctypes
import json
import math
import os
from pathlib import Path
import re
import threading
import time

GPU_PATH=r'\GPU Engine(*)\Utilization Percentage'
MORE_DATA=0x800007D2

def select_rows(rows,pids):
    """Keep per-engine instances: summing parallel engines is not GPU utilization."""
    accepted=[]
    for name,status,value in rows:
        if len(name)>256:continue
        match=re.match(r'pid_(\d+)_',name)
        if not match or int(match[1]) not in pids or status not in (0,1):continue
        if not math.isfinite(value) or not 0<=value<=100:continue
        if len(accepted)>=128:break
        accepted.append({'instance':name[:256],'pid':int(match[1]),'percent':value})
    return accepted

class PdhQuery:
    def __init__(self):
        if os.name!='nt':raise OSError('Windows PDH unavailable')
        from ctypes import wintypes as wt
        class ValueUnion(ctypes.Union):
            _fields_=[('doubleValue',ctypes.c_double),('longValue',ctypes.c_int32),('largeValue',ctypes.c_int64)]
        class Value(ctypes.Structure):
            _fields_=[('CStatus',wt.DWORD),('value',ValueUnion)]
        class Item(ctypes.Structure):
            _fields_=[('name',wt.LPWSTR),('formatted',Value)]
        self.Item=Item;self.api=ctypes.WinDLL('pdh.dll');self.query=ctypes.c_void_p();self.counter=ctypes.c_void_p()
        signatures={
            'PdhOpenQueryW':([wt.LPCWSTR,ctypes.c_size_t,ctypes.POINTER(ctypes.c_void_p)],ctypes.c_long),
            'PdhAddEnglishCounterW':([ctypes.c_void_p,wt.LPCWSTR,ctypes.c_size_t,ctypes.POINTER(ctypes.c_void_p)],ctypes.c_long),
            'PdhCollectQueryData':([ctypes.c_void_p],ctypes.c_long),
            'PdhGetFormattedCounterArrayW':([ctypes.c_void_p,wt.DWORD,ctypes.POINTER(wt.DWORD),ctypes.POINTER(wt.DWORD),ctypes.c_void_p],ctypes.c_long),
            'PdhCloseQuery':([ctypes.c_void_p],ctypes.c_long)}
        for name,(args,result) in signatures.items():f=getattr(self.api,name);f.argtypes=args;f.restype=result
        self.check(self.api.PdhOpenQueryW(None,0,ctypes.byref(self.query)))
        try:
            self.check(self.api.PdhAddEnglishCounterW(self.query,GPU_PATH,0,ctypes.byref(self.counter)))
            self.check(self.api.PdhCollectQueryData(self.query))
        except Exception:self.close();raise
    @staticmethod
    def check(code):
        if code:raise OSError(f'PDH status 0x{code&0xffffffff:08x}')
    def sample(self):
        from ctypes import wintypes as wt
        self.check(self.api.PdhCollectQueryData(self.query));size=wt.DWORD();count=wt.DWORD()
        status=self.api.PdhGetFormattedCounterArrayW(self.counter,0x200,ctypes.byref(size),ctypes.byref(count),None)
        if status&0xffffffff!=MORE_DATA:self.check(status)
        if not size.value:return []
        if size.value>4*1024*1024:raise OSError('PDH counter array exceeds 4MiB')
        memory=ctypes.create_string_buffer(size.value)
        self.check(self.api.PdhGetFormattedCounterArrayW(self.counter,0x200,ctypes.byref(size),ctypes.byref(count),memory))
        if count.value>size.value//ctypes.sizeof(self.Item):raise OSError('PDH counter count exceeds allocation')
        items=ctypes.cast(memory,ctypes.POINTER(self.Item))
        return [(items[i].name or '',items[i].formatted.CStatus,items[i].formatted.value.doubleValue) for i in range(count.value)]
    def close(self):
        if self.query:self.api.PdhCloseQuery(self.query);self.query=ctypes.c_void_p()

class Sampler:
    def __init__(self,directory):
        self.directory=Path(directory);self.pids=set();self.lock=threading.Lock();self.stop_event=threading.Event();self.started=time.monotonic()
        self.thread=threading.Thread(target=self.run,daemon=True,name='rt-gpu-telemetry')
    def start(self):self.thread.start();return self
    def track(self,pids):
        with self.lock:self.pids.update(int(p) for p in pids if int(p)>0)
    def run(self):
        query=None;count=0;error=None
        try:
            query=PdhQuery()
            with (self.directory/'gpu-engine-samples.jsonl').open('w',encoding='utf-8') as stream:
                while not self.stop_event.wait(1) and count<960:
                    with self.lock:pids=set(self.pids)
                    rows=select_rows(query.sample(),pids);count+=1
                    stream.write(json.dumps({'sample':count,'unix_ns':time.time_ns(),'elapsed_ms':(time.monotonic()-self.started)*1000,'tracked_pids':sorted(pids),'engines':rows})+'\n');stream.flush()
        except Exception as exc:error=f'{type(exc).__name__}: {exc}'
        finally:
            if query:query.close()
            (self.directory/'gpu-engine-status.json').write_text(json.dumps({'schema':1,'status':'unavailable' if error else 'sampled','samples':count,'error':error,'path':GPU_PATH,'limits':'Per-process per-engine PDH utilization, one-second cadence. Engines may overlap; never sum as whole-GPU utilization. Not Vulkan pass timings.'},indent=2)+'\n',encoding='utf-8')
    def stop(self):
        self.stop_event.set();self.thread.join(timeout=3)
        return not self.thread.is_alive()
