"""Optional bounded Windows GPU Engine PDH sampling; never gates rendering.

The per-engine samples are directional diagnostics, not an aggregate GPU load.
"""
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
PROCESS_QUERY_LIMITED_INFORMATION=0x1000

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

def process_cpu_delta(previous,current,elapsed_ns,logical_processors):
    """Convert cumulative 100ns process times into bounded interval evidence."""
    if not previous or previous[0]!=current[0] or elapsed_ns<=0:return None
    kernel=max(0,current[1]-previous[1]);user=max(0,current[2]-previous[2]);cpu_ms=(kernel+user)/10000
    wall_ms=elapsed_ns/1000000
    return {'kernel_ms':kernel/10000,'user_ms':user/10000,'cpu_ms':cpu_ms,'interval_ms':wall_ms,
            'one_core_percent':100*cpu_ms/wall_ms,
            'machine_percent':100*cpu_ms/(wall_ms*max(1,logical_processors))}

class ProcessTimes:
    def __init__(self):
        if os.name!='nt':raise OSError('Windows process times unavailable')
        from ctypes import wintypes as wt
        self.api=ctypes.WinDLL('kernel32.dll',use_last_error=True)
        self.api.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD];self.api.OpenProcess.restype=wt.HANDLE
        self.api.GetProcessTimes.argtypes=[wt.HANDLE,ctypes.POINTER(wt.FILETIME),ctypes.POINTER(wt.FILETIME),ctypes.POINTER(wt.FILETIME),ctypes.POINTER(wt.FILETIME)];self.api.GetProcessTimes.restype=wt.BOOL
        self.api.CloseHandle.argtypes=[wt.HANDLE];self.api.CloseHandle.restype=wt.BOOL
        self.wt=wt
    @staticmethod
    def value(value):return (int(value.dwHighDateTime)<<32)|int(value.dwLowDateTime)
    def sample(self,pid):
        handle=self.api.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,False,int(pid))
        if not handle:return None
        try:
            created=self.wt.FILETIME();exited=self.wt.FILETIME();kernel=self.wt.FILETIME();user=self.wt.FILETIME()
            if not self.api.GetProcessTimes(handle,ctypes.byref(created),ctypes.byref(exited),ctypes.byref(kernel),ctypes.byref(user)):return None
            return self.value(created),self.value(kernel),self.value(user)
        finally:self.api.CloseHandle(handle)

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
        self.directory=Path(directory);self.pids=set();self.roles={};self.lock=threading.Lock();self.stop_event=threading.Event();self.started=time.monotonic()
        self.thread=threading.Thread(target=self.run,daemon=True,name='rt-gpu-telemetry')
    def start(self):self.thread.start();return self
    def track(self,pids,role='unknown'):
        with self.lock:
            for raw in pids:
                pid=int(raw)
                if pid>0:self.pids.add(pid);self.roles[pid]=role if role!='unknown' or pid not in self.roles else self.roles[pid]
    def run(self):
        query=None;processes=None;count=0;gpu_error=None;cpu_error=None;previous={};previous_ns={};logical=os.cpu_count() or 1
        try:
            try:query=PdhQuery()
            except Exception as exc:gpu_error=f'{type(exc).__name__}: {exc}'
            try:processes=ProcessTimes()
            except Exception as exc:cpu_error=f'{type(exc).__name__}: {exc}'
            if query is None and processes is None:raise OSError('GPU and process CPU telemetry unavailable')
            with (self.directory/'gpu-engine-samples.jsonl').open('w',encoding='utf-8') as stream:
                while not self.stop_event.wait(1) and count<960:
                    with self.lock:pids=set(self.pids);roles=dict(self.roles)
                    now_ns=time.monotonic_ns();unix_ns=time.time_ns();rows=[]
                    if query:
                        try:rows=select_rows(query.sample(),pids)
                        except Exception as exc:gpu_error=f'{type(exc).__name__}: {exc}';query.close();query=None
                    cpu=[]
                    if processes:
                        for pid in sorted(pids):
                            try:current=processes.sample(pid)
                            except Exception as exc:cpu_error=f'{type(exc).__name__}: {exc}';processes=None;break
                            if current is None:continue
                            delta=process_cpu_delta(previous.get(pid),current,now_ns-previous_ns.get(pid,now_ns),logical)
                            previous[pid]=current;previous_ns[pid]=now_ns
                            if delta:cpu.append({'pid':pid,'role':roles.get(pid,'unknown'),'interval_begin_unix_ns':unix_ns-int(delta['interval_ms']*1000000),**delta})
                    count+=1
                    stream.write(json.dumps({'sample':count,'unix_ns':unix_ns,'elapsed_ms':(time.monotonic()-self.started)*1000,'tracked_pids':sorted(pids),'engines':rows,'process_cpu':cpu})+'\n');stream.flush()
        except Exception as exc:
            if gpu_error is None:gpu_error=f'{type(exc).__name__}: {exc}'
            if cpu_error is None:cpu_error=f'{type(exc).__name__}: {exc}'
        finally:
            if query:query.close()
            (self.directory/'gpu-engine-status.json').write_text(json.dumps({'schema':2,'status':'unavailable' if gpu_error else 'sampled','samples':count,'error':gpu_error,'path':GPU_PATH,
                'process_cpu_status':'unavailable' if cpu_error else 'sampled','process_cpu_error':cpu_error,'logical_processors':logical,
                'limits':'Per-process per-engine PDH utilization and cumulative process CPU deltas at one-second cadence. Engines may overlap; never sum as whole-GPU utilization. CPU one_core_percent can exceed 100 for multithreaded processes. Not Vulkan pass timings.'},indent=2)+'\n',encoding='utf-8')
    def stop(self):
        self.stop_event.set();self.thread.join(timeout=3)
        return not self.thread.is_alive()
