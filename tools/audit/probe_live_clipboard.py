#!/usr/bin/env python3
"""Opt-in live text/clear/private-MIME round trips using the deployed Deskflow pair.

The old SSH clipboard service must be stopped. SSH here only sets/observes test
clipboard data; Deskflow transports the clipboard between the machines. Content
is never printed. Restores the original Linux text to the shared clipboard.
"""
import argparse
import subprocess
import time
import uuid


def run(argv, data=None):
    if argv[0] == 'wl-copy':
        # Its background selection owner inherits output descriptors; don't wait
        # for EOF on a captured pipe that deliberately outlives the command.
        subprocess.run(argv, input=data, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=8, check=True)
        return b''
    return subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8, check=True).stdout


def wait_for(reader, expected, label):
    start = time.monotonic()
    while time.monotonic() - start < 8:
        if reader() == expected:
            print(f"PASS {label}: {len(expected)} bytes, observed within {(time.monotonic()-start)*1000:.1f} ms", flush=True)
            return
        time.sleep(0.05)
    raise RuntimeError(f"{label}: clipboard did not match (contents withheld)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--live', action='store_true', required=True)
    parser.parse_args()
    if subprocess.run(['systemctl', '--user', 'is-active', '--quiet', 'mac-clipboard.service']).returncode == 0:
        raise RuntimeError('Stop the legacy SSH bridge first; otherwise this does not test Deskflow')
    ssh = ['ssh', '-o', 'BatchMode=yes', 'mac']
    local_read = lambda: run(['wl-paste', '--no-newline', '--type', 'text'])
    # Deskflow's existing Mac text converter represents line breaks as CR.
    remote_read = lambda: run(ssh + ['LC_ALL=en_US.UTF-8 pbpaste']).replace(b'\r\n', b'\n').replace(b'\r', b'\n')
    try:
        original = local_read()
    except subprocess.CalledProcessError:
        types = subprocess.run(['wl-paste', '--list-types'], capture_output=True, timeout=8)
        if types.returncode == 0 and types.stdout.strip():
            raise RuntimeError('Non-text clipboard present; refusing to overwrite it for a text-only probe')
        original = b''
    try:
        token = uuid.uuid4().hex.encode()
        small = b'Deskflow Linux ' + token + ' ✓\nline two'.encode()
        run(['wl-copy', '--type', 'text/plain;charset=utf-8'], small)
        wait_for(remote_read, small, 'Linux to Mac Unicode')
        other = b'Deskflow Mac ' + token + ' é\nsecond line'.encode()
        run(ssh + ['LC_ALL=en_US.UTF-8 pbcopy'], other)
        wait_for(local_read, other, 'Mac to Linux Unicode')
        for size in (0, 1024 * 1024):
            value = (token * ((size + len(token) - 1) // len(token)))[:size]
            run(['wl-copy', '--type', 'text/plain;charset=utf-8'], value)
            wait_for(remote_read, value, f'Linux to Mac {size}')
            reverse = value + b'reverse'
            run(ssh + ['LC_ALL=en_US.UTF-8 pbcopy'], reverse)
            wait_for(local_read, reverse, f'Mac to Linux {len(reverse)}')
        # Deliberately mark a harmless sentinel as private. It must never arrive.
        private = b'PRIVATE-TEST-' + token
        run(['wl-copy', '--type', 'x-kde-passwordManagerHint'], private)
        time.sleep(0.4)
        if private in remote_read():
            raise RuntimeError('Private MIME sentinel was exported')
        print('PASS private MIME sentinel excluded', flush=True)
    finally:
        run(['wl-copy', '--type', 'text/plain;charset=utf-8'], original)


if __name__ == '__main__':
    main()
