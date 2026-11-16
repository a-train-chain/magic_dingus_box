#!/usr/bin/env python3
"""Attended TV driver for checklist runs on a real box (not part of the product).

`serve` keeps ONE virtual gamepad open for the whole session (a fresh device per
press loses its first press while the kiosk opens the evdev node) and reads
commands from a root-only FIFO. Any other invocation sends commands to it:

    mdbdrive.py tap OK            mdbdrive.py tap DOWN 3 0.4
    mdbdrive.py hold BLACK 0.6    mdbdrive.py stop

Run `serve` as root in a transient unit with RuntimeMaxSec so it cannot outlive
the session.
"""
import os
import sys
import time

sys.path.insert(0, '/opt/magic_dingus_box')
RUN = '/run/mdb-drive'
FIFO = RUN + '/cmd'


def serve():
    from magic_dingus_box.web.remote.uinput_writer import UinputWriter
    os.makedirs(RUN, mode=0o700, exist_ok=True)
    if not os.path.exists(FIFO):
        os.mkfifo(FIFO, 0o600)
    writer = UinputWriter()
    time.sleep(2)  # let the kiosk open the new evdev node before any press
    print('[mdb-drive] ready', flush=True)
    while True:
        with open(FIFO) as stream:
            for line in stream:
                parts = line.split()
                if not parts:
                    continue
                if parts[0] == 'stop':
                    writer.release_all()
                    print('[mdb-drive] stop', flush=True)
                    return
                try:
                    run(writer, parts)
                except Exception as error:  # keep serving; report the bad line
                    print('[mdb-drive] error %r: %s' % (parts, error), flush=True)


def run(writer, parts):
    verb, button = parts[0], parts[1]
    if verb == 'tap':
        count = int(parts[2]) if len(parts) > 2 else 1
        gap = float(parts[3]) if len(parts) > 3 else 0.35
        for _ in range(count):
            writer.press(button, 'down')
            time.sleep(0.06)
            writer.press(button, 'up')
            time.sleep(gap)
    elif verb == 'hold':
        writer.press(button, 'down')
        time.sleep(float(parts[2]))
        writer.press(button, 'up')
    else:
        raise ValueError('unknown verb')
    print('[mdb-drive] %s' % ' '.join(parts), flush=True)


if __name__ == '__main__':
    if sys.argv[1:] == ['serve']:
        serve()
    else:
        with open(FIFO, 'w') as stream:
            stream.write(' '.join(sys.argv[1:]) + '\n')
