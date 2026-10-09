"""Read-only combined ESP serial + HTTP runtime timeline. Never resets, enables or moves hardware."""
import argparse
import datetime
import json
from pathlib import Path
import re
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--port', help='Optional ESP diagnostic serial port; no reset')
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--quiet', action='store_true', help='Write only to file; avoid stdout pipe backpressure during long monitoring')
    args = parser.parse_args()
    if args.seconds < 0: parser.error('seconds must be >= 0 (0 means until Ctrl-C)')
    base = args.url.rstrip('/')
    output = args.output or Path('artifacts/lamp/runtime-monitor') / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.ndjson')
    output.parent.mkdir(parents=True, exist_ok=True)
    port = None
    if args.port:
        import serial
        port = serial.Serial()
        port.port, port.baudrate, port.timeout = args.port, 115200, 0
        port.dtr = port.rts = False
        port.open()
        port.dtr = True  # Native USB output enabled; RTS unchanged, no hardware reset.
    cursor, latest, token = 0, 0, ''
    started = time.monotonic()
    serial_pending = b''
    try:
        with output.open('w', encoding='utf-8') as stream:
            def record(item):
                item['host_time'] = datetime.datetime.now().astimezone().isoformat()
                line = json.dumps(item, ensure_ascii=False)
                stream.write(line + '\n'); stream.flush()
                if not args.quiet:
                    print(line, flush=True)
            while not args.seconds or time.monotonic() - started < args.seconds:
                if port:
                    serial_pending += port.read(min(port.in_waiting, 8192))
                    while b'\n' in serial_pending:
                        raw, serial_pending = serial_pending.split(b'\n', 1)
                        text = raw.decode('utf-8', errors='replace').strip()
                        if any(x in text for x in ['SSL send failed', 'SSL receive failed', 'LampRuntime', 'Guru Meditation', 'Backtrace']):
                            text = re.sub(r'sk-[A-Za-z0-9_-]+', '[KEY REDACTED]', text)
                            record({'source':'serial', 'text':text[:2048]})
                    serial_pending = serial_pending[-8192:]
                try:
                    if not token:
                        with urllib.request.urlopen(base + '/', timeout=2) as response:
                            html = response.read().decode('utf-8')
                        token = re.search(r"const token\s*=\s*['\"]([^'\"]+)", html).group(1)
                    request = urllib.request.Request(base + '/api/lamp/history?after=' + str(cursor), headers={'X-Lamp-Token':token})
                    with urllib.request.urlopen(request, timeout=2) as response:
                        history = json.load(response)
                    if history['latest'] < latest:
                        cursor = 0
                        record({'source':'runtime','kind':'boot_reset'})
                    else:
                        if history.get('gap'): record({'source':'runtime','kind':'history_gap'})
                        for event in history['events']: record({'source':'runtime', **event})
                        cursor = history['cursor']
                    latest = history['latest']
                except urllib.error.HTTPError as error:
                    if error.code in (401,403): token = ''
                    record({'source':'monitor','error':'HTTP ' + str(error.code)})
                except (OSError, ValueError, AttributeError) as error:
                    record({'source':'monitor','error':type(error).__name__})
                time.sleep(.25)
    except KeyboardInterrupt:
        pass
    finally:
        if port:
            port.dtr = False
            port.close()
    print('Saved:', output.resolve())


if __name__ == '__main__':
    main()
