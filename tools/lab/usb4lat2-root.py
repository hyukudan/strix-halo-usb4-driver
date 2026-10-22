#!/usr/bin/env python3
"""Fixed W4 controls, started by the administrator on BOTH hosts. No sudo in runner.

sudo python3 /home/sergioc/ai-cluster-lab/usb4-lat2/usb4lat2-root.py
Lifetime 1800s, then at most 30s to restore closed streams. Never unloads/reboots.
Protocol: write '<unique-number> A0|A1|B0|B1|Q0|Q1|T1|C3|end' to request;
or '<number> traceon <label>' / '<number> traceoff'. ACK JSON includes request ID.
All paths, knobs and events are fixed; no command from the request is executed.
"""
import errno
import fcntl
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import stat
import subprocess
import time

BASE = Path('/sys/kernel/config/thunderbolt/stream')
WQ = Path('/sys/module/thunderbolt/parameters/dma_wq_percpu')
CB = Path('/sys/module/thunderbolt_stream/parameters/contig_buffers')
RUN = Path('/run/usb4lat2')
TRACE = Path('/sys/kernel/tracing')
INST = TRACE / 'instances/usb4lat2'
ARMS = {'A0': (0, 14, 'N', 8192, 0), 'A1': (1, 14, 'N', 8192, 0),
        'B0': (0, 11, 'N', 8192, 0), 'B1': (1, 11, 'N', 8192, 0),
        'Q0': (0, 14, 'Y', 8192, 0), 'Q1': (0, 11, 'Y', 8192, 0),
        'T1': (1, 14, 'N', 0, 0), 'C3': (1, 14, 'N', 8192, 3)}
ATTRS = ('rx_napi', 'irq_cpu', 'busy_poll', 'spin_us', 'throttling')
EVENTS = ('irq/irq_handler_entry', 'irq/irq_handler_exit', 'workqueue/workqueue_queue_work',
          'workqueue/workqueue_execute_start', 'workqueue/workqueue_execute_end',
          'sched/sched_waking', 'sched/sched_wakeup', 'sched/sched_switch', 'power/cpu_idle')
FUNCTIONS = ('ring_work', 'tbstream_rx_start_poll', 'tbstream_dev_napi_rx_locked',
             'tbstream_dev_rx_callback', 'tbstream_dev_tx_callback')


def read(path):
    return path.read_text().strip()


def put(path, value):
    path.write_text(str(value) + '\n')


def closed():
    result = subprocess.run(['fuser', '/dev/tbstream0', '/dev/tbstream1',
                             '/dev/tbstream2', '/dev/tbstream3'], capture_output=True)
    if result.returncode != 1:  # 1 = none; missing utility/errors must fail closed
        raise RuntimeError('streams open or fuser failed: ' + result.stdout.decode(errors='replace'))


def qos_clear(streams):
    paths = [s / 'latency_qos_us' for s in streams]
    paths += [Path('/sys/module/thunderbolt_stream/parameters/latency_qos_us')]
    if any(read(p) != '-1' for p in paths):
        raise RuntimeError('latency_qos_us must already be -1 (helper never changes QoS)')


def main():
    if os.geteuid() != 0:
        raise RuntimeError('administrator must start this helper')
    # Do not contend with the older administrative helpers.
    procs = subprocess.run(['pgrep', '-af', '(lat-knobs|rb-knobs)\\.sh'], capture_output=True, text=True)
    if procs.returncode == 0:
        raise RuntimeError('another knob helper is alive: ' + procs.stdout)
    if procs.returncode != 1:
        raise RuntimeError('cannot check old helpers')
    RUN.mkdir(mode=0o755, exist_ok=True)
    if RUN.is_symlink() or RUN.stat().st_uid != 0:
        raise RuntimeError('unsafe run directory')
    lock = open(RUN / 'lock', 'w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    streams = sorted(BASE.glob('*/data*'))
    if len(streams) != 4:
        raise RuntimeError('requires four streams')
    qos_clear(streams)
    saved = {str(s / a): read(s / a) for s in streams for a in ATTRS}
    saved.update({str(WQ): read(WQ), str(CB): read(CB)})
    put(RUN / 'saved.json', json.dumps(saved, indent=2))
    user = pwd.getpwnam('sergioc')
    req = RUN / 'request'
    req.unlink(missing_ok=True)  # root-owned parent: no concurrent replacement by user
    fd = os.open(req, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.fchown(fd, user.pw_uid, user.pw_gid)
    os.close(fd)
    captures = RUN / ('captures-' + time.strftime('%Y%m%d-%H%M%S'))
    captures.mkdir(mode=0o755)
    state = {'pid': os.getpid(), 'captures': str(captures), 'expires_epoch': time.time() + 1800}
    (RUN / 'restored.json').unlink(missing_ok=True)
    (RUN / 'ready.json').unlink(missing_ok=True)
    stop = False
    active = None
    trace_deadline = 0.0
    created = False

    def terminate(_sig, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)

    def traceoff():
        nonlocal active
        if active is None:
            return
        put(INST / 'tracing_on', 0)
        with open(INST / 'trace', 'rb') as src, open(captures / (active + '.trace'), 'wb') as dst:
            shutil.copyfileobj(src, dst)
        stats = {p.parent.name: read(p) for p in INST.glob('per_cpu/cpu*/stats')}
        put(captures / (active + '.stats.json'), json.dumps(stats, indent=2))
        active = None

    deadline = time.monotonic() + 1800
    last = ''
    try:
        if INST.exists():
            raise RuntimeError('trace instance already exists; do not overwrite another capture')
        INST.mkdir()
        created = True
        put(INST / 'tracing_on', 0)
        put(INST / 'trace_clock', 'mono')
        put(INST / 'buffer_size_kb', 1024)  # per CPU, bounded (~32 MiB total on these nodes)
        put(INST / 'options/overwrite', 0)
        available = {line.split()[0] for line in (TRACE / 'available_filter_functions').read_text().splitlines()}
        selected = [f for f in FUNCTIONS if f in available]
        if not selected:
            raise RuntimeError('no required host completion functions traceable')
        put(INST / 'set_ftrace_filter', '\n'.join(selected))
        put(INST / 'current_tracer', 'function')
        for event in EVENTS:
            put(INST / 'events' / event / 'enable', 1)
        # Future 0046 supplies ring/frame identity, while the deployed kernel uses filtered functions.
        custom = INST / 'events/thunderbolt/tb_latency/enable'
        if custom.exists():
            put(custom, 1)
        put(captures / 'functions.json', json.dumps(selected))
        put(RUN / 'ready.json', json.dumps(state))
        while not stop and time.monotonic() < deadline:
            if active is not None and time.monotonic() >= trace_deadline:
                traceoff()
            fd = os.open(req, os.O_RDONLY | os.O_NOFOLLOW)
            try:
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    raise RuntimeError('request is not a regular file')
                words = os.read(fd, 128).decode('ascii').split()
            finally:
                os.close(fd)
            if len(words) >= 2 and words[0] != last and re.fullmatch(r'[0-9]{1,20}', words[0]):
                last, command = words[:2]
                ack = {'id': last, 'command': command, 'ok': False, 'mono_ns': time.monotonic_ns()}
                try:
                    if command == 'end' and len(words) == 2:
                        stop = True
                    elif command == 'traceoff' and len(words) == 2:
                        traceoff()
                    elif command == 'traceon' and len(words) == 3 and re.fullmatch(r'[A-Za-z0-9_-]{1,48}', words[2]):
                        traceoff()
                        active = words[2]
                        if (captures / (active + '.trace')).exists():
                            active = None
                            raise RuntimeError('capture label already used')
                        put(INST / 'trace', '')
                        put(INST / 'tracing_on', 1)
                        trace_deadline = min(deadline, time.monotonic() + 5)
                    elif command in ARMS and len(words) == 2:
                        traceoff()
                        closed()
                        if any(read(s / 'gpu_direct') != '0' for s in streams):
                            raise RuntimeError('arm requires layout 1')
                        qos_clear(streams)
                        napi, cpu, wq, itr, cb = ARMS[command]
                        put(WQ, wq)
                        put(CB, cb)
                        for s in streams:
                            for attr, value in zip(ATTRS, (napi, cpu, 0, 0, itr)):
                                put(s / attr, value)  # EBUSY aborts arm; never acknowledge partial state as success
                        expected = {str(s / a): str(v) for s in streams
                                    for a, v in zip(ATTRS, (napi, cpu, 0, 0, itr))}
                        expected.update({str(WQ): wq, str(CB): str(cb)})
                        if any(read(Path(p)) != v for p, v in expected.items()):
                            raise RuntimeError('knob readback mismatch')
                        ack['values'] = expected
                    else:
                        raise ValueError('invalid fixed command')
                    ack['ok'] = True
                except (OSError, RuntimeError, ValueError) as exc:
                    ack['error'] = str(exc)
                    stop = True  # partial arm cannot be followed by a measurement
                put(RUN / 'ack.tmp', json.dumps(ack))
                os.replace(RUN / 'ack.tmp', RUN / 'ack.json')
                print(json.dumps(ack), flush=True)
            time.sleep(0.02)
    finally:
        if created:
            try:
                traceoff()
                put(INST / 'tracing_on', 0)
                for event in EVENTS:
                    put(INST / 'events' / event / 'enable', 0)
                custom = INST / 'events/thunderbolt/tb_latency/enable'
                if custom.exists():
                    put(custom, 0)
                put(INST / 'current_tracer', 'nop')
                INST.rmdir()
            except OSError as exc:
                print('TRACE CLEANUP ERROR (restoring knobs regardless)', str(exc), flush=True)
        remaining = dict(saved)
        until = time.monotonic() + 30
        while remaining and time.monotonic() < until:
            for path, value in list(remaining.items()):
                try:
                    put(Path(path), value)
                    if read(Path(path)) == value:
                        del remaining[path]
                except OSError as exc:
                    if exc.errno != errno.EBUSY:
                        print('RESTORE ERROR', path, str(exc), flush=True)
            if remaining:
                time.sleep(0.1)
        put(RUN / 'restored.json', json.dumps({'remaining': remaining, 'mono_ns': time.monotonic_ns()}))
        (RUN / 'ready.json').unlink(missing_ok=True)
        if remaining:
            raise RuntimeError('RESTORATION INCOMPLETE: ' + repr(remaining))


if __name__ == '__main__':
    main()
