#!/usr/bin/env python3
"""Check WatchTasks' interactive overlay through AmiNetXDuo's WebSocket shell.

Requires pyte and AmiNetXDuo's tests/tools/hwshell.py. Stage the command on
an Amiga first; see README.md. Uses session 1 and restores the Shell prompt.
"""

import argparse
import codecs
import importlib.util
import re
import time
from pathlib import Path

import pyte


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--hwshell", type=Path, required=True)
    parser.add_argument("--command", default="RAM:WatchTasks-overlay-test")
    parser.add_argument("--session", type=int, choices=(0, 1), default=1)
    parser.add_argument("--log-file", type=Path)
    args = parser.parse_args()
    filter_name = args.command.rsplit("/", 1)[-1].rsplit(":", 1)[-1]
    if not filter_name or len(filter_name) > 31:
        parser.error("command name must fit the 31-character task filter")

    spec = importlib.util.spec_from_file_location('hwshell', str(args.hwshell))
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    d = harness.load_drill(args.address, args.port)
    deadline = time.time() + 15
    while True:
        ws = d.WsConn(timeout=10, path=f"/shell?session={args.session}")
        if ws.status == 101 or time.time() >= deadline:
            break
        ws.close()
        time.sleep(1)
    assert ws.status == 101, f'WebSocket status {ws.status}: {ws.buf!r}'
    screen = pyte.Screen(80, 24)
    stream = pyte.Stream(screen)
    raw = bytearray()
    decoder = codecs.getincrementaldecoder("utf-8")()
    running = False

    def keys(text):
        ws.send(d.ws_frame(2, text.encode('utf-8')))

    def word(text):
        ws.send(d.ws_frame(1, text))

    def pump(seconds):
        start = len(raw)
        deadline = time.time() + seconds
        while time.time() < deadline:
            frame = ws.frame(deadline=deadline)
            if frame is None:
                break
            _, opcode, payload, _ = frame
            if opcode == 2:
                raw.extend(payload)
                stream.feed(decoder.decode(payload).replace('\x9b', '\x1b['))
            elif opcode == 8:
                raise RuntimeError('Shell connection closed')
        return bytes(raw[start:])

    def display():
        return '\n'.join(screen.display)

    def wait_for(predicate, label, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            pump(0.2)
            if predicate():
                pump(0.3)
                print(f'PASS: {label}', flush=True)
                return
        print(display(), flush=True)
        raise AssertionError(label)

    def send_and_wait(text, predicate, label):
        keys(text)
        wait_for(predicate, label)

    def resize(columns, lines):
        screen.resize(lines=lines, columns=columns)
        word(f'size {columns} {lines}')
        pump(3.0)
        assert 'TASK DETAILS' in display(), display()
        # The overlay's title and key bars are blue (SGR 43) cells: each spans
        # the panel, centred with a margin on both sides.
        bars = [y for y, line in enumerate(screen.display)
                if 'TASK DETAILS' in line or 'Enter/Esc' in line]
        assert len(bars) == 2, display()
        for y in bars:
            cells = [x for x in range(columns) if screen.buffer[y][x].bg == 'brown']
            start, end = cells[0], cells[-1]
            assert start > 0 and end < columns - 1 and start == columns - 1 - end, \
                (columns, screen.display[y])
        print(f'PASS: overlay resize {columns}x{lines}', flush=True)

    try:
        pump(1)
        keys('Prompt "OVERLAY-QA %R*N"\n')
        wait_for(lambda: b'OVERLAY-QA 0\n' in raw, 'shell prompt')
        word('size 80 24')
        keys(f'{args.command} TICKS=10\n')
        running = True
        wait_for(lambda: 'Enter info' in display(), 'task table and Return shortcut')
        send_and_wait(f'/{filter_name}\n',
                      lambda: filter_name in display() and 'TASK DETAILS' not in display(),
                      'Return finishes filter without opening details')
        send_and_wait('\r', lambda: 'TASK DETAILS' in display(), 'Return opens overlay')
        for field in ('NAME', 'TYPE', 'TASK ID', 'CLI', 'COMMAND', 'ARGUMENTS', 'CPU%', 'STACK RANGE', 'SIG ALLOC'):
            assert field in display(), (field, display())
        identity = re.search(r'TASK ID\s+([0-9A-F]{8})', display()).group(1)
        pump(1)
        assert identity in display(), display()
        print('PASS: task identity survives live refresh', flush=True)
        print('--- 80x24 ---\n' + display(), flush=True)
        send_and_wait('n', lambda: 'Saved SP may be stale' in display(), 'page reaches remaining fields')
        send_and_wait('u', lambda: 'STATUS' in display(), 'page returns to top')
        send_and_wait('\x1b', lambda: 'TASK DETAILS' not in display(), 'Escape restores table')
        assert identity in display(), display()
        send_and_wait('\n', lambda: 'TASK DETAILS' in display(), 'LF Return opens overlay')
        resize(40, 14)
        print('--- 40x14 ---\n' + display(), flush=True)
        send_and_wait('n', lambda: 'SIG ALLOC' in display(), 'narrow overlay page')
        send_and_wait('n', lambda: 'Saved SP may be stale' in display(), 'narrow overlay reaches final fields')
        resize(120, 30)
        assert 'Saved SP may be stale' in display(), display()
        print('PASS: scroll clamps after enlargement', flush=True)
        send_and_wait('\r', lambda: 'TASK DETAILS' not in display(), 'Return restores resized table')
        send_and_wait('q', lambda: 'OVERLAY-QA 0' in display(), 'clean exit to Shell')
        running = False
        keys('Run >NIL: Wait 8 SECS\n')
        pump(0.5)
        keys(f'{args.command} TICKS=10\n')
        running = True
        wait_for(lambda: 'Enter info' in display(), 'second monitor run')
        send_and_wait('/Wait\n', lambda: 'Wait' in display(), 'temporary process filter')
        send_and_wait('\r', lambda: 'TASK DETAILS' in display(), 'temporary process details')
        assert re.search(r'COMMAND\s+Wait', display()), display()
        wait_for(lambda: 'MISSING: last captured values' in display(), 'exited task retains marked snapshot', timeout=12)
        send_and_wait('q', lambda: 'OVERLAY-QA 0' in display(), 'quit directly from overlay')
        running = False
        print('watchtasks-console: all checks passed', flush=True)
    finally:
        if running:
            word('break')
            pump(3)
        keys('Prompt "%N.%S> "\n')
        pump(0.3)
        ws.close()
        if args.log_file:
            args.log_file.write_bytes(raw)


if __name__ == "__main__":
    main()
