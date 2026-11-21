import unittest, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from serial_lab import (
    build_modbus_fc04,
    build_tbd_poll,
    build_ydt1363_frame,
    decode_shoto_modbus_v12,
    decode_tbd_telemetry,
    decode_ydt1363_42,
    inspect_ascii,
    modbus_crc,
    pylon_checksum,
    tbd_checksum,
)


class TestLab(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(modbus_crc(bytes.fromhex("01030000000A")), 0xCDC5)

    def test_ascii(self):
        body = b"20024661E00202"
        frame = b"~" + body + f"{pylon_checksum(body):04X}".encode() + b"\r"
        self.assertEqual(inspect_ascii(frame)["cid2"], "61")
        with self.assertRaises(ValueError):
            inspect_ascii(frame[:-3] + b"00\r")

    def test_length(self):
        body = b"20024661000202"
        frame = b"~" + body + f"{pylon_checksum(body):04X}".encode() + b"\r"
        with self.assertRaises(ValueError):
            inspect_ascii(frame)

    def test_shoto_modbus_v12_golden_pdf_p10(self):
        # Official Shoto BMS Modbus V1.2 PDF p.10 golden TX and RX
        self.assertEqual(build_modbus_fc04(0x27, 0x0FFF, 0x0027), bytes.fromhex("27040FFF00278432"))
        golden_rx = bytes.fromhex(
            "27044E"
            "13EB271001F402B202BC0000000008001388271003E80000000F"
            "0D4E0D480D3D0D4A0D400D480D460D470D3E0D400D4F0D540D470D4C0D4DFFFF"
            "000402BC028A02BC02BC0078041A012C14C816082820"
        )
        d = decode_shoto_modbus_v12(golden_rx)
        self.assertEqual(d["addr"], 39)
        self.assertEqual(d["pack_v"], 50.99)
        self.assertEqual(d["current_canonical_a"], 0.0)
        self.assertEqual(d["rem_cap_ah"], 50.0)
        self.assertEqual(d["full_cap_ah"], 100.0)
        self.assertEqual(d["soc_pct"], 50.0)
        self.assertEqual(d["soh_pct"], 100.0)
        self.assertEqual(d["cell_num"], 15)
        self.assertEqual(d["cells_v"][0], 3.406)
        self.assertEqual(d["temps_c"], [30.0, 25.0, 30.0, 30.0])
        self.assertEqual(d["fault_byte"], 0x00)
        self.assertEqual(d["status_byte"], 0x08)
        self.assertTrue(d["disg_mos_connect"])
        self.assertEqual(d["max_chg_cur_a"], 120)
        self.assertEqual(d["float_v"], 53.20)
        self.assertEqual(d["boost_v"], 56.40)

    def test_doc_v28_ydt1363_golden_frames(self):
        # Official BMS Communication Protocol V2.8 DOC golden TX frames
        self.assertEqual(build_ydt1363_frame("26", "00", "46", "42", "01"), b"~26004642E00201FD30\r")
        self.assertEqual(build_ydt1363_frame("26", "00", "46", "44", "01"), b"~26004644E00201FD2E\r")
        self.assertEqual(build_ydt1363_frame("26", "00", "46", "4F", ""), b"~2600464F0000FD94\r")
        self.assertEqual(build_ydt1363_frame("26", "00", "46", "51", ""), b"~260046510000FDA8\r")

    def test_tbd_binary_golden_polls_and_decode(self):
        # Verified from TianBangda LFP MS-ShotoV1.1.634-26.exe (Form1::check & timer_*_Tick)
        self.assertEqual(build_tbd_poll(1, 0x7C, 0x01), bytes.fromhex("7C010100020D"))
        self.assertEqual(build_tbd_poll(1, 0x7C, 0x33), bytes.fromhex("7C013300FE0D"))
        self.assertEqual(build_tbd_poll(1, 0x7C, 0x42), bytes.fromhex("7C014200800D"))
        self.assertEqual(build_tbd_poll(1, 0x7C, 0x06), bytes.fromhex("7C010600F80D"))

        # Build synthetic 15S TBD RX telemetry matching timer_data_Tick offsets (len = 56 + 2*15 = 86)
        buf = bytearray(86)
        buf[0], buf[1], buf[5] = 0x7C, 0x01, 15
        for i in range(15):
            mv = 3300 + i
            buf[6 + 2 * i] = (mv >> 8) & 0x1F
            buf[7 + 2 * i] = mv & 0xFF
        c = 30
        # Current = +5.00A -> raw = 30000 - 500 = 29500 (0x733C)
        buf[8 + c], buf[9 + c] = 0x73, 0x3C
        # SOC = 82.50% -> 8250 (0x203A)
        buf[12 + c], buf[13 + c] = 0x20, 0x3A
        # Full Cap = 100.00Ah -> 10000 (0x2710)
        buf[16 + c], buf[17 + c] = 0x27, 0x10
        buf[19 + c] = 5
        for k in range(5):
            buf[21 + c + 2 * k] = 75 + k  # 25..29 C (raw - 50)
        b = 2 * 15 + 10
        buf[34 + b], buf[35 + b] = 0x00, 0x2A  # 42 cycles
        buf[38 + b], buf[39 + b] = 0x13, 0x88  # 50.00 V
        buf[42 + b], buf[43 + b] = 0x26, 0xAC  # 99.00 % SOH
        buf[84] = tbd_checksum(bytes(buf[:84]))
        buf[85] = 0x0D
        d = decode_tbd_telemetry(bytes(buf))
        self.assertEqual(d["cell_num"], 15)
        self.assertEqual(d["cells_v"][0], 3.300)
        self.assertEqual(d["current_canonical_a"], 5.00)
        self.assertEqual(d["soc_pct"], 82.50)
        self.assertEqual(d["full_cap_ah"], 100.00)
        self.assertEqual(d["temps_c"], [25, 26, 27, 28, 29])
        self.assertEqual(d["cycles"], 42)
        self.assertEqual(d["pack_v"], 50.00)
        self.assertEqual(d["soh_pct"], 99.00)

    def test_ydt1363_42_decode(self):
        # 1 pack, 2 cells (3.300V, 3.310V), 2 temps (25.0C=2981=0x0BA5, 26.0C=2991=0x0BAF),
        # I=-10.00A (0xFC18), V=52.80V (0x14A0), Rem=80.00Ah (0x1F40), Full=100.00Ah (0x2710), Cyc=12
        payload = bytes([
            0x00, 0x01, 0x02,
            0x0C, 0xE4, 0x0C, 0xEE,
            0x02,
            0x0B, 0xA5, 0x0B, 0xAF,
            0xFC, 0x18,
            0x14, 0xA0,
            0x1F, 0x40,
            0x03,
            0x27, 0x10,
            0x00, 0x0C,
        ]).hex().upper()
        d = decode_ydt1363_42(payload)
        self.assertEqual(d["cells_v"], [3.300, 3.310])
        self.assertEqual(d["temps_c"], [25.0, 26.0])
        self.assertEqual(d["current_canonical_a"], -10.0)
        self.assertEqual(d["pack_v"], 52.80)
        self.assertEqual(d["soc_pct"], 80.0)
        self.assertEqual(d["cycles"], 12)


if __name__ == "__main__":
    unittest.main()
