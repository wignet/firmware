"""Run the Buildroot NTP startup script with an offline/online network."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SERVICE = (Path(__file__).parent / "buildroot/board/pi0w/overlay"
           / "etc/init.d/S45ntpd")


class TimeSyncStartupTests(unittest.TestCase):
    def start_service(self, online):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            commands = {
                "ip": '#!/bin/sh\n[ "$NETWORK_READY" = yes ] && echo "default via 192.0.2.1"\nexit 0\n',
                "start-stop-daemon": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$DAEMON_CALLS"\n',
            }
            for name, contents in commands.items():
                program = root / name
                program.write_text(contents)
                program.chmod(0o755)
            calls = root / "calls"
            env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}",
                       NETWORK_READY="yes" if online else "no", DAEMON_CALLS=str(calls))
            subprocess.run(["sh", str(SERVICE), "start"], env=env, check=True)
            return calls.read_text().splitlines() if calls.exists() else []

    def test_offline_boot_defers_time_sync_until_network_is_ready(self):
        self.assertEqual(self.start_service(online=False), [])

    def test_online_boot_starts_time_sync_and_saves_successful_syncs(self):
        calls = self.start_service(online=True)
        self.assertEqual(len(calls), 1)
        self.assertIn("-x /usr/sbin/ntpd", calls[0])
        self.assertIn("-S /usr/libexec/wignet-tv/ntp-event", calls[0])
        self.assertIn("-O /var/log/ntpd.log", calls[0])


if __name__ == "__main__":
    unittest.main()
