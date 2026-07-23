#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import time
import serial


class ArduinoBridge:
    def __init__(self, port="COM3", baud=115200, timeout=0.2, boot_wait=2.5):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.boot_wait = boot_wait
        self.ser = None

    def open(self):
        if self.ser is not None and self.ser.is_open:
            return

        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        time.sleep(self.boot_wait)  # allow Arduino reset after serial open
        self.flush_input()

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def flush_input(self):
        if self.ser is None:
            return
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass

    def _readline(self):
        if self.ser is None:
            raise RuntimeError("Serial port is not open")

        raw = self.ser.readline()
        if not raw:
            return ""
        return raw.decode("utf-8", errors="ignore").strip()

    def send_command(self, cmd, ack_timeout=2.0, done_timeout=120.0, verbose=True):
        """
        Send one Arduino command and wait for:
          ACK
          ... optional DBG lines ...
          DONE
        or:
          ERR <message>
        """
        if self.ser is None or not self.ser.is_open:
            self.open()

        self.flush_input()

        if verbose:
            print("[ARD->] {}".format(cmd))

        self.ser.write((cmd.strip() + "\n").encode("utf-8"))
        self.ser.flush()

        ack_received = False
        t0 = time.time()

        while True:
            now = time.time()
            elapsed = now - t0

            if not ack_received and elapsed > ack_timeout:
                raise RuntimeError("No ACK from Arduino for command: {}".format(cmd))

            if ack_received and elapsed > done_timeout:
                raise RuntimeError("Timeout waiting for DONE for command: {}".format(cmd))

            line = self._readline()
            if not line:
                continue

            if verbose:
                print("[ARD<-] {}".format(line))

            if line == "ACK":
                ack_received = True
                continue

            if line == "DONE":
                if not ack_received:
                    raise RuntimeError("Arduino sent DONE before ACK")
                return True

            if line.startswith("ERR"):
                raise RuntimeError(line)

            # Ignore all DBG or any other informational lines

    def ping(self, verbose=True):
        return self.send_command("PING", ack_timeout=2.0, done_timeout=5.0, verbose=verbose)

    def set_zero(self, verbose=True):
        return self.send_command("Z", ack_timeout=2.0, done_timeout=5.0, verbose=verbose)

    def go_xy(self, x_mm, y_mm, verbose=True):
        cmd = "G {} {}".format(x_mm, y_mm)
        return self.send_command(cmd, ack_timeout=2.0, done_timeout=120.0, verbose=verbose)

    def move_piece(self, from_sq, to_sq, dry_run=False, verbose=True):
        cmd = "PMV {} {}".format(from_sq.upper(), to_sq.upper())
        if dry_run:
            cmd += " DRY"
        return self.send_command(cmd, verbose=verbose)

    def capture_piece(self, from_sq, to_sq, piece_token, dry_run=False, verbose=True):
        cmd = "CAP {} {} {}".format(from_sq.upper(), to_sq.upper(), piece_token)
        if dry_run:
            cmd += " DRY"
        return self.send_command(cmd, verbose=verbose)


def main():
    bridge = ArduinoBridge()

    try:
        bridge.open()
        bridge.ping()
        print("[OK] Arduino bridge connected successfully.")
    finally:
        bridge.close()


if __name__ == "__main__":
    main()