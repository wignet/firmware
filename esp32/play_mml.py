#!/usr/bin/env python3
"""Play an MML tune on a PhotoPainter over USB, with no platform involved.

    python3 esp32/play_mml.py wake.mml
    python3 esp32/play_mml.py wake.mml --volume 40 --seconds 60
    echo "A t140 o5 l8 c e g >c" | python3 esp32/play_mml.py -
    python3 esp32/play_mml.py wake.mml --check    # parse only, no frame

The tune is checked and normalized with the platform's own parser
(backoffice/mml.py) first, so a mistake is reported here with its line, and
the frame receives the form it expects. Then it goes over the USB serial
port to the firmware's bench console (src/usb_console.cpp), which plays it
through the alarm's audio path: no fade-in, full `--volume` from the start.
Ctrl+C stops it; so does any button on the frame.

The frame must be awake: connected to this computer and past its boot (it
stays awake while USB is connected). The port opens without moving DTR/RTS,
which would reset the ESP32-S3.

Needs pyserial (pip install pyserial).
"""
import argparse
import glob
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
from backoffice import mml  # noqa: E402

PREFIX = "[bench] "


class ConsoleError(RuntimeError):
    pass


class Console:
    """The firmware's bench console, over anything with write() and readline()."""

    def __init__(self, port, log=print, clock=time.monotonic):
        self.port = port
        self.log = log
        self.clock = clock

    def send(self, text):
        self.port.write((text + "\n").encode("ascii"))

    def wait(self, *answers, timeout=10.0):
        """The next console reply starting with one of `answers`.

        Firmware log lines are shown as they come; a console error raises.
        """
        deadline = self.clock() + timeout
        while self.clock() < deadline:
            raw = self.port.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line.startswith(PREFIX):
                self.log(f"  frame | {line}")
                continue
            reply = line[len(PREFIX):]
            if reply.startswith("error"):
                raise ConsoleError(reply)
            if reply.startswith(answers):
                return reply
        raise TimeoutError(f"no {' or '.join(answers)!r} from the frame within {timeout:.0f} s")

    def open_tune(self, attempts=40, every=3.0):
        """Ask for the tune prompt until the frame answers.

        The loop is busy for tens of seconds during an e-paper refresh or a
        fetch, so one unanswered request means little.
        """
        for attempt in range(attempts):
            self.send("mml")
            try:
                return self.wait("ready", timeout=every)
            except TimeoutError:
                if attempt == 0:
                    self.log("Waiting for the frame (it may be redrawing or fetching)...")
        raise TimeoutError("the frame's bench console never answered: is it awake, "
                           "and running firmware with usb_console?")

    def upload(self, normalized):
        self.open_tune()
        for line in normalized.split("\n"):
            self.send(line)
            self.wait("+", timeout=90.0)
        self.send("end")
        return self.wait("ok", timeout=90.0)

    def play(self, volume, seconds):
        self.send(f"play {volume} {seconds}")
        self.log(PREFIX + self.wait("playing", timeout=90.0))
        try:
            return self.wait("done", timeout=seconds + 30.0)
        except KeyboardInterrupt:
            self.send("stop")
            self.log("Stopping...")
            # "idle" if the tune had just ended on its own.
            return self.wait("done", "idle", timeout=15.0)


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit("No PhotoPainter found (/dev/cu.usbmodem*, /dev/ttyACM*). "
                         "Plug it in, or pass --port.")
    if len(ports) > 1:
        print(f"Several ports, using {ports[0]} (others: {', '.join(ports[1:])}); "
              "pass --port to choose.")
    return ports[0]


def open_serial(path):
    try:
        import serial
    except ImportError:
        raise SystemExit("pyserial is missing: pip install pyserial") from None
    port = serial.Serial()
    port.port = path
    port.baudrate = 115200
    port.timeout = 0.2
    # Both stay raised, as macOS raises them on open. On the ESP32-S3's
    # native USB, RTS high with DTR low resets the chip (esptool's reset);
    # lowering both passes through that state and rebooted the frame on
    # every open (measured 2026-09-26). Left raised, nothing moves.
    port.dtr = True
    port.rts = True
    port.open()
    return port


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", help="tune file, or - for standard input")
    parser.add_argument("--volume", type=int, default=60, help="0-100 (default 60)")
    parser.add_argument("--seconds", type=int, default=30, help="1-3600 (default 30)")
    parser.add_argument("--port", help="serial port (default: the first USB modem)")
    parser.add_argument("--check", action="store_true", help="check the tune, touch no frame")
    parser.add_argument("--upload-only", action="store_true", help="send it, don't play it")
    args = parser.parse_args(argv)

    text = sys.stdin.read() if args.file == "-" else open(args.file, encoding="utf-8").read()
    try:
        tune = mml.parse(text)
    except mml.MmlError as exc:
        print(f"Tune refused: {exc}")
        return 1
    print(f"Tune OK: {tune.seconds:.1f} s per loop, t{tune.tempo}, "
          f"channels {''.join(tune.channels)}, {len(tune.text)} bytes")
    if args.check:
        return 0

    path = args.port or find_port()
    port = open_serial(path)
    console = Console(port)
    try:
        print(f"Sending to {path}...")
        print(PREFIX + console.upload(tune.text))
        if args.upload_only:
            return 0
        print(PREFIX + console.play(max(0, min(100, args.volume)),
                                    max(1, min(3600, args.seconds))))
        return 0
    except ConsoleError as exc:
        print(f"The frame refused: {exc}")
        return 1
    except TimeoutError as exc:
        print(exc)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
