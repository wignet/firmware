#!/usr/bin/env python3
"""Test a PhotoPainter alarm end to end over USB, with no platform involved.

    python3 esp32/test_alarm.py --in 60                  # awake, the sound
    python3 esp32/test_alarm.py wake.mml --in 90 --sleep # deep sleep, then wake
    python3 esp32/test_alarm.py wake.mml --in 90 --sleep-rtc
    python3 esp32/test_alarm.py --status                 # clock and alarms

Sets a one-off test alarm on the frame through its USB bench console
(src/usb_console.cpp): it rings through the real alarm path (fade-in,
volume, duration, buttons), with the tune if one is given, else the frame's
sound. The platform's cached alarm is left as it is, and a /schedule poll
does not touch the test alarm.

--sleep puts the frame into deep sleep right away, even though USB is
connected, so the alarm has to wake it; --sleep-rtc also sets the timer
backup two minutes late, so only the RTC interrupt can wake it in time. The
USB port disappears while it sleeps; the script waits for it to come back
and reads the frame's report: when it rang, how late, and what woke it.

A ringing alarm does not stop on serial input (a real alarm must not):
press a button on the frame, or keep --duration short. Needs pyserial.
"""
import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import play_mml  # noqa: E402  (Console, port handling)
from play_mml import mml  # noqa: E402

STATUS_LINES = ("clock", "test alarm", "last test alarm", "platform alarm")


def read_status(console, timeout=10.0):
    """The frame's `alarm` report, as {first words: line}; None if it did not answer."""
    console.send("alarm")
    lines = {}
    try:
        while True:
            line = console.wait(*STATUS_LINES, timeout=timeout)
            key = next(k for k in sorted(STATUS_LINES, key=len, reverse=True) if line.startswith(k))
            lines[key] = line
            if key == "platform alarm":
                return lines
    except TimeoutError:
        return lines or None


def wait_ready(console, attempts=40):
    """Status once the frame answers: it is silent while it redraws or rings."""
    for attempt in range(attempts):
        status = read_status(console, timeout=3)
        if status:
            return status
        if attempt == 0:
            print("Waiting for the frame (it may be redrawing or fetching)...")
    raise TimeoutError("the frame's bench console never answered: is it awake, "
                       "and running firmware with usb_console?")


def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


def wait_for(condition, timeout, every=0.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(every)
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", nargs="?", help="tune file (default: the frame's sound)")
    parser.add_argument("--in", dest="seconds", type=int, default=60, help="seconds from now (60)")
    parser.add_argument("--volume", type=int, default=50)
    parser.add_argument("--fade", type=int, default=10, help="fade-in seconds (10)")
    parser.add_argument("--duration", type=int, default=30, help="seconds at full volume (30, minimum)")
    sleep = parser.add_mutually_exclusive_group()
    sleep.add_argument("--sleep", action="store_true", help="deep sleep until the alarm")
    sleep.add_argument("--sleep-rtc", action="store_true", help="deep sleep; only the RTC can wake it in time")
    parser.add_argument("--status", action="store_true", help="print clock and alarms, change nothing")
    parser.add_argument("--port")
    args = parser.parse_args(argv)

    tune = None
    if args.file:
        try:
            tune = mml.parse(open(args.file, encoding="utf-8").read())
        except mml.MmlError as exc:
            print(f"Tune refused: {exc}")
            return 1

    port = play_mml.open_serial(args.port or play_mml.find_port())
    console = play_mml.Console(port)
    try:
        status = wait_ready(console)
        if args.status:
            for line in status.values():
                print(play_mml.PREFIX + line)
            return 0
        if tune is not None:
            print(play_mml.PREFIX + console.upload(tune.text))
        command = f"alarm in {args.seconds} {args.volume} {args.fade} {args.duration}"
        if tune is None:
            command += " sound"
        console.send(command)
        reply = console.wait("alarm set", timeout=30)
        due = int(reply.split()[3])
        print(play_mml.PREFIX + reply)
        rings_for = args.fade + max(30, args.duration)
        print(f"Due at {time.strftime('%H:%M:%S', time.localtime(due))}, then about {rings_for} s "
              "of alarm (a button on the frame stops it).")

        if args.sleep or args.sleep_rtc:
            console.send("sleep rtc" if args.sleep_rtc else "sleep")
            print(play_mml.PREFIX + console.wait("sleeping", timeout=30))
            port.close()
            path = port.port
            if not wait_for(lambda: path not in ports(), 30):
                print("The frame did not go to sleep (its USB port stayed).")
                return 1
            print(f"Asleep at {time.strftime('%H:%M:%S')}; waiting for it to wake...")
            if not wait_for(lambda: ports(), max(0, due - time.time()) + 240):
                print("The frame did not come back on USB.")
                return 1
            print(f"Back on USB at {time.strftime('%H:%M:%S')}.")
            time.sleep(1)
            port = play_mml.open_serial(ports()[0])
            console = play_mml.Console(port)
        else:
            print("Staying awake; waiting for the alarm...")

        # The report appears once the alarm has rung; the console is silent
        # while it rings.
        deadline = time.time() + max(0, due - time.time()) + rings_for + 180
        while time.time() < deadline:
            status = read_status(console, timeout=5)
            report = (status or {}).get("last test alarm", "")
            if report and str(time.strftime("%H:%M:%S", time.gmtime(due))) in report:
                print(play_mml.PREFIX + report)
                return 0 if "rang" in report and "MISSED" not in report else 1
            time.sleep(3)
        print("No report of the test alarm; check the frame's log (pio device monitor).")
        return 1
    except play_mml.ConsoleError as exc:
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
