import io
import os
from pathlib import Path
import pty
import socket
import subprocess
import sys
import tty
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from check_rtcm_usb import RtcmReader, crc24q, read_port


def frame(payload):
    header = bytes([0xD3, len(payload) >> 8, len(payload) & 255])
    return header + payload + crc24q(header + payload).to_bytes(3, 'big')


class UsbTests(unittest.TestCase):
    def simulated_chain(self, drop=False):
        executable = os.environ.get('ESPNOW_BRIDGE_TEST_EXE')
        if not executable:
            self.skipTest('run scripts/test_espnow_host.sh to compile the real C harness')
        expected = frame(b'\x00\x11\r\n\xff') + frame(bytes(i % 256 for i in range(1023)))
        response = b'HTTP/1.1 200 OK\r\nContent-Type: gnss/data\r\n\r\n' + expected
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            def caster():
                connection, _ = server.accept()
                with connection:
                    for i in range(0, len(response), 13):
                        connection.sendall(response[i:i + 13])
            thread = threading.Thread(target=caster)
            thread.start()
            with socket.create_connection(server.getsockname(), timeout=2) as client:
                received = bytearray()
                while chunk := client.recv(37):
                    received.extend(chunk)
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
        result = subprocess.run([executable, *(['--drop-fragment'] if drop else [])],
                                input=received, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        return expected, result.stdout

    def test_synthetic_tcp_real_c_codec_simulated_radio_to_linux_serial(self):
        expected, received = self.simulated_chain()
        self.assertEqual(received, expected)
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            os.write(master, received)
            output = io.BytesIO()
            result = read_port(os.ttyname(slave), frames=2, timeout=1, output=output)
            self.assertEqual(output.getvalue(), expected)
            self.assertEqual(result['frames'], 2)
        finally:
            os.close(master)
            os.close(slave)

    def test_synthetic_chain_fragment_loss_never_emits_partial_usb_frame(self):
        expected, received = self.simulated_chain(drop=True)
        self.assertEqual(received, frame(b'\x00\x11\r\n\xff'))
        self.assertNotEqual(received, expected)
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            os.write(master, received)
            with self.assertRaisesRegex(TimeoutError, '1/2 frames'):
                read_port(os.ttyname(slave), frames=2, timeout=0.05)
        finally:
            os.close(master)
            os.close(slave)

    def test_all_payload_sizes_and_split_reads(self):
        for size in range(1024):
            expected = frame(bytes(i % 256 for i in range(size)))
            parser = RtcmReader()
            found = []
            for i in range(0, len(expected), 7):
                found += parser.feed(expected[i:i + 7])
            self.assertEqual(found, [expected])
            self.assertEqual(parser.pending, b'')

    def test_noise_is_never_silently_discarded(self):
        with self.assertRaisesRegex(ValueError, 'pollution'):
            RtcmReader().feed(b'boot log\r\n' + frame(b'test'))

    def test_corrupted_frame_rejected(self):
        data = bytearray(frame(b'\x00\x11\xff'))
        data[4] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC24Q'):
            RtcmReader().feed(data)

    def test_reserved_bits_rejected(self):
        with self.assertRaisesRegex(ValueError, 'reserved'):
            RtcmReader().feed(b'\xd3\x80\x00')

    def test_linux_pty_binary_capture_order(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            expected = frame(b'\x00\r\n\xff') + frame(bytes(range(256)))
            os.write(master, expected)
            output = io.BytesIO()
            result = read_port(os.ttyname(slave), frames=2, timeout=1, output=output)
            self.assertEqual(output.getvalue(), expected)
            self.assertEqual(result['frames'], 2)
            self.assertEqual(result['noise_bytes'], 0)
        finally:
            os.close(master)
            os.close(slave)

    def test_linux_pty_incomplete_frame_times_out(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            os.write(master, frame(b'abc')[:-1])
            with self.assertRaisesRegex(TimeoutError, 'pending bytes'):
                read_port(os.ttyname(slave), frames=1, timeout=0.05)
        finally:
            os.close(master)
            os.close(slave)

    def test_continuous_stream_partial_next_frame_is_not_corruption(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            os.write(master, frame(b'abc') + frame(b'def')[:4])
            result = read_port(os.ttyname(slave), frames=1, timeout=1)
            self.assertEqual(result['pending_bytes'], 4)
        finally:
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    unittest.main()
