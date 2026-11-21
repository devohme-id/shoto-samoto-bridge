"""Read-only Modbus/TianBangda/YD/T1363 probe, autodetect, passive capture, and offline decoder.
No inverter responses or register writes. Install pyserial + minimalmodbus for hardware modes.
"""
import argparse, datetime, json, re, time
from pathlib import Path

# SmartLi DC/DC community mapping (hpeyerl/esphome-shoto-smartli)
REGS = {
    "vbus": 0x0000, "vbat": 0x0001, "tmax": 0x0005,
    "ibus_raw": 0x1030, "soc": 0x1034, "soh": 0x1035,
    "alarm1": 0x1037, "alarm2": 0x1038, "alarm3": 0x1039,
    "alarm4": 0x103A, "alarm5": 0x103B, "protection": 0x103C,
    "operation": 0x103D, "online_count": 0x103F,
}

CID2_NAMES = {
    "42": "Analog Data (Pack)",
    "44": "Alarm / Protection Info (Pack)",
    "47": "Charge/Discharge Management (Pack)",
    "4F": "Protocol Version",
    "51": "Manufacturer Info",
    "61": "System Analog Data (Master)",
    "62": "System Alarm Info (Master)",
    "63": "System Charge/Discharge Limits (Master)",
    "92": "Module Info / Management",
}


def modbus_crc(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def build_modbus_read(addr: int, fc: int, start_reg: int, count: int) -> bytes:
    payload = bytes([
        addr & 0xFF, fc & 0xFF,
        (start_reg >> 8) & 0xFF, start_reg & 0xFF,
        (count >> 8) & 0xFF, count & 0xFF,
    ])
    crc = modbus_crc(payload)
    return payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def build_modbus_fc04(addr: int, start_reg: int, count: int) -> bytes:
    return build_modbus_read(addr, 0x04, start_reg, count)


def tbd_checksum(data: bytes) -> int:
    """TianBangda Form1::check (token 0x06000026): (XOR(bytes) ^ SUM(bytes)) & 0xFF."""
    x = 0
    s = 0
    for b in data:
        x ^= b
        s += b
    return (x ^ s) & 0xFF


def build_tbd_poll(addr: int, header: int = 0x7C, cmd: int = 0x01) -> bytes:
    body = bytes([header & 0xFF, addr & 0xFF, cmd & 0xFF, 0x00])
    return body + bytes([tbd_checksum(body), 0x0D])


def pylon_checksum(body: bytes) -> int:
    return (-sum(body)) & 0xFFFF


def build_ydt1363_frame(ver: str = "26", adr: str = "00", cid1: str = "46",
                        cid2: str = "42", info_hex: str = "") -> bytes:
    n = len(info_hex)
    lchk = (-((n >> 8) + ((n >> 4) & 0xF) + (n & 0xF))) & 0xF
    length_str = f"{lchk:X}{n:03X}"
    body = f"{ver}{adr}{cid1}{cid2}{length_str}{info_hex}".encode("ascii")
    chk = f"{pylon_checksum(body):04X}".encode("ascii")
    return b"~" + body + chk + b"\r"


def inspect_ascii(frame: bytes) -> dict:
    """Validate and unpack a ~...CR YD/T 1363.3 / Pylontech ASCII frame."""
    if not (frame.startswith(b"~") and frame.endswith(b"\r")):
        raise ValueError("bad delimiter")
    body, check = frame[1:-5], frame[-5:-1]
    if len(body) < 12 or len(body) % 2 or not re.fullmatch(b"[0-9A-Fa-f]+", body + check):
        raise ValueError("bad ASCII hex")
    if pylon_checksum(body) != int(check, 16):
        raise ValueError("bad checksum")
    field = int(body[8:12], 16)
    n = field & 0xFFF
    lchk = (-((n >> 8) + ((n >> 4) & 15) + (n & 15))) & 15
    if n != len(body[12:]) or field >> 12 != lchk:
        raise ValueError("nonstandard or invalid LENGTH/LCHKSUM")
    return dict(
        ver=body[:2].decode(), adr=body[2:4].decode(), cid1=body[4:6].decode(),
        cid2=body[6:8].decode(), payload=body[12:].decode(),
    )


def decode_shoto_modbus_v12(frame: bytes) -> dict:
    """Decode official Shoto BMS Modbus V1.2 FC04 response for 0x0FFF..0x1025 (39 regs)."""
    if len(frame) < 5:
        raise ValueError("frame too short")
    expected_crc = (frame[-1] << 8) | frame[-2]
    if modbus_crc(frame[:-2]) != expected_crc:
        raise ValueError("bad modbus CRC16")
    addr, fc, byte_len = frame[0], frame[1], frame[2]
    if fc != 0x04 or len(frame) != 3 + byte_len + 2 or byte_len < 78:
        raise ValueError(f"unexpected fc/length: fc={fc:#x}, bytes={byte_len}")
    regs = [((frame[3 + 2 * i] << 8) | frame[4 + 2 * i]) for i in range(byte_len // 2)]
    pack_v = round(regs[0] * 0.01, 2)
    raw_i = regs[1]
    # PDF Note 3: charge is negative, discharge is positive -> (raw - 10000) * 0.1
    # Canonical bridge (+ = charge, - = discharge): (10000 - raw) * 0.1
    current_pdf_a = round((raw_i - 10000) * 0.1, 2)
    current_canonical_a = round((10000 - raw_i) * 0.1, 2)
    rem_cap_ah = round(regs[2] * 0.1, 2)
    avg_cell_t_c = round((regs[3] - 400) * 0.1, 2)
    env_t_c = round((regs[4] - 400) * 0.1, 2)
    warn_flag = regs[5]
    prot_flag = regs[6]
    reg_1006 = regs[7]
    # Verified from PDF p.10 golden frame 08 00 (Status=0x08 DisgMOSConnect, Fault=0x00)
    fault_byte = reg_1006 & 0xFF
    status_byte = (reg_1006 >> 8) & 0xFF
    soc_pct = round(regs[8] * 0.01, 2)
    ba430_shifted = len(regs) >= 35 and regs[12] > 16 and 1 <= regs[13] <= 16
    if ba430_shifted:
        cycles = regs[9]
        soh_pct = round(regs[10] * 0.01, 2)
        full_cap_ah = round(rem_cap_ah * 100.0 / soc_pct, 1) if soc_pct > 1.0 and rem_cap_ah > 0 else 100.0
        cell_num = min(regs[13], 16)
        cells_v = [
            round(regs[14 + i] * 0.001, 3)
            for i in range(cell_num) if regs[14 + i] != 0xFFFF
        ]
        ntc_num = min(regs[30], 4)
        temps_c = [
            float(regs[31 + k]) if regs[31 + k] < 200 else round((regs[31 + k] - 400) * 0.1, 2)
            for k in range(ntc_num)
        ]
        max_chg_cur_a = regs[35] if len(regs) > 35 else 100
        max_chg_temp_c = 55.0
        min_chg_temp_c = 0.0
        float_v = 53.2
        boost_v = 54.4
    else:
        soh_pct = round(regs[9] * 0.01, 2)
        full_cap_ah = round(regs[10] * 0.1, 2)
        cycles = regs[11]
        cell_num = min(regs[12], 16)
        cells_v = [
            round(regs[13 + i] * 0.001, 3)
            for i in range(cell_num) if regs[13 + i] != 0xFFFF
        ]
        ntc_num = min(regs[29], 4)
        temps_c = [round((regs[30 + k] - 400) * 0.1, 2) for k in range(ntc_num)]
        max_chg_cur_a = regs[34]
        max_chg_temp_c = round((regs[35] - 400) * 0.1, 2)
        min_chg_temp_c = round((regs[36] - 400) * 0.1, 2)
        float_v = round(regs[37] * 0.01, 2)
        boost_v = round(regs[38] * 0.01, 2)
    return {
        "protocol": "SHOTO_MODBUS_V12",
        "addr": addr,
        "pack_v": pack_v,
        "current_pdf_a": current_pdf_a,
        "current_canonical_a": current_canonical_a,
        "rem_cap_ah": rem_cap_ah,
        "full_cap_ah": full_cap_ah,
        "avg_cell_t_c": avg_cell_t_c,
        "env_t_c": env_t_c,
        "warn_flag": warn_flag,
        "prot_flag": prot_flag,
        "fault_byte": fault_byte,
        "status_byte": status_byte,
        "chg_mos_connect": bool(status_byte & 0x04),
        "disg_mos_connect": bool(status_byte & 0x08),
        "soc_pct": soc_pct,
        "soh_pct": soh_pct,
        "cycles": cycles,
        "cell_num": cell_num,
        "cells_v": cells_v,
        "temps_c": temps_c,
        "max_chg_cur_a": max_chg_cur_a,
        "max_chg_temp_c": max_chg_temp_c,
        "min_chg_temp_c": min_chg_temp_c,
        "float_v": float_v,
        "boost_v": boost_v,
    }


def decode_tbd_telemetry(rx: bytes) -> dict:
    """Decode TianBangda binary live-data frame (both V1.634 TLV 140B & legacy V1.1 86/88B)."""
    if len(rx) < 48 or rx[-1] != 0x0D:
        raise ValueError("bad TBD frame length or EOI")
    if rx[-2] != tbd_checksum(rx[:-2]):
        raise ValueError("bad TBD checksum")

    # Format B: SD-BA430 / BMS PLUS V1.634 TLV frame (7E <addr> 01 <plen> <tags...> <chk> 0D)
    plen = rx[3]
    if rx[0] == 0x7E and rx[2] == 0x01 and plen >= 40 and len(rx) == 4 + plen + 2 and rx[4] == 0x01:
        pos = 4
        end = 4 + plen
        cells_v = []
        current_a = 0.0
        soc_pct = 0.0
        soh_pct = 100.0
        full_cap_ah = 100.0
        temps_c = []
        flags_hex = ""
        cycles = 0
        pack_v = 0.0
        bus_v = 0.0
        while pos + 2 <= end:
            tag = rx[pos]
            cnt = rx[pos + 1]
            pos += 2
            elem_sz = 4 if 0x0B <= tag <= 0x10 else 2
            nbytes = cnt * elem_sz
            if pos + nbytes > end:
                break
            if tag == 0x01:
                cells_v = [
                    round(((((rx[pos + 2 * i] & 0x1F) << 8) | rx[pos + 2 * i + 1])) / 1000.0, 3)
                    for i in range(min(cnt, 16))
                ]
            elif tag == 0x02 and cnt >= 1:
                raw_i = (rx[pos] << 8) | rx[pos + 1]
                current_a = round((30000 - raw_i) / 100.0, 2)
            elif tag == 0x03 and cnt >= 1:
                soc_pct = round(((rx[pos] << 8) | rx[pos + 1]) / 100.0, 2)
            elif tag == 0x04 and cnt >= 1:
                soh_pct = round(((rx[pos] << 8) | rx[pos + 1]) / 100.0, 2)
            elif tag == 0x05:
                temps_c = [rx[pos + 2 * k + 1] - 50 for k in range(cnt)]
            elif tag == 0x06:
                flags_hex = bytes(rx[pos:pos + nbytes]).hex()
            elif tag == 0x07 and cnt >= 1:
                cycles = (rx[pos] << 8) | rx[pos + 1]
            elif tag == 0x08 and cnt >= 1:
                pack_v = round(((rx[pos] << 8) | rx[pos + 1]) / 100.0, 2)
            elif tag == 0x09 and cnt >= 1:
                full_cap_ah = round(((rx[pos] << 8) | rx[pos + 1]) / 100.0, 2)
            elif tag == 0x11 and cnt >= 1:
                bus_v = round(((rx[pos] << 8) | rx[pos + 1]) / 100.0, 2)
            pos += nbytes
        if pack_v <= 0 and bus_v > 0:
            pack_v = bus_v
        rem_cap_ah = round(full_cap_ah * soc_pct / 100.0, 2)
        return {
            "protocol": "TBD_BINARY",
            "format": "TLV",
            "header": hex(rx[0]),
            "addr": rx[1],
            "cell_num": len(cells_v),
            "cells_v": cells_v,
            "current_canonical_a": current_a,
            "soc_pct": soc_pct,
            "soh_pct": soh_pct,
            "full_cap_ah": full_cap_ah,
            "rem_cap_ah": rem_cap_ah,
            "temps_c": temps_c,
            "cycles": cycles,
            "pack_v": pack_v,
            "bus_v": bus_v,
            "flags_hex": flags_hex,
        }

    # Format A: Legacy fixed 86/88-byte frame (MS-ShotoV1.1.exe timer_data_Tick)
    n_cells = rx[5]
    if n_cells not in (15, 16) or len(rx) < 56 + 2 * n_cells:
        raise ValueError(f"unsupported TBD cell count {n_cells} for len {len(rx)}")
    c = 2 * n_cells
    cells_v = [
        round((((rx[6 + 2 * i] & 0x1F) << 8) | rx[7 + 2 * i]) / 1000.0, 3)
        for i in range(n_cells)
    ]
    raw_i = (rx[8 + c] << 8) | rx[9 + c]
    current_a = round((30000 - raw_i) / 100.0, 2)
    soc_pct = round(((rx[12 + c] << 8) | rx[13 + c]) / 100.0, 2)
    full_cap_ah = round(((rx[16 + c] << 8) | rx[17 + c]) / 100.0, 2)
    rem_cap_ah = round(full_cap_ah * soc_pct / 100.0, 2)
    t_cnt = rx[19 + c]
    t_loop = 5 if t_cnt == 5 else 5
    temps_c = [rx[21 + c + 2 * k] - 50 for k in range(t_loop)]
    b = 2 * n_cells + 2 * t_loop
    flags = list(rx[22 + b:34 + b])
    cycles = (rx[34 + b] << 8) | rx[35 + b]
    pack_v = round(((rx[38 + b] << 8) | rx[39 + b]) / 100.0, 2)
    soh_pct = round(((rx[42 + b] << 8) | rx[43 + b]) / 100.0, 2)
    return {
        "protocol": "TBD_BINARY",
        "header": hex(rx[0]),
        "addr": rx[1],
        "cell_num": n_cells,
        "cells_v": cells_v,
        "current_canonical_a": current_a,
        "soc_pct": soc_pct,
        "soh_pct": soh_pct,
        "full_cap_ah": full_cap_ah,
        "rem_cap_ah": rem_cap_ah,
        "temps_c": temps_c,
        "cycles": cycles,
        "pack_v": pack_v,
        "flags_hex": bytes(flags).hex(),
    }


def decode_ydt1363_42(payload_hex: str) -> dict:
    """Decode YD/T 1363.3 / TBD_1363 CID2=42 response payload (BMS Communication Protocol V2.8)."""
    raw = bytes.fromhex(payload_hex)
    if len(raw) < 16:
        raise ValueError("payload too short")
    m = raw[2]
    idx = 3
    cells_v = []
    for _ in range(m):
        cells_v.append(round(((raw[idx] << 8) | raw[idx + 1]) / 1000.0, 3))
        idx += 2
    n = raw[idx]
    idx += 1
    temps_c = []
    for _ in range(n):
        t_raw = (raw[idx] << 8) | raw[idx + 1]
        temps_c.append(round((t_raw - 2731) / 10.0, 1))
        idx += 2
    cur_raw = (raw[idx] << 8) | raw[idx + 1]
    if cur_raw >= 0x8000:
        cur_raw -= 0x10000
    current_a = round(cur_raw / 100.0, 2)
    idx += 2
    pack_v = round(((raw[idx] << 8) | raw[idx + 1]) / 100.0, 2)
    idx += 2
    rem_cap_ah = round(((raw[idx] << 8) | raw[idx + 1]) / 100.0, 2)
    idx += 2
    _custom_cnt = raw[idx]
    idx += 1
    total_cap_ah = round(((raw[idx] << 8) | raw[idx + 1]) / 100.0, 2)
    idx += 2
    cycles = (raw[idx] << 8) | raw[idx + 1]
    soc_pct = round(100.0 * rem_cap_ah / total_cap_ah, 2) if total_cap_ah > 0 else 0.0
    return {
        "protocol": "TBD_1363",
        "pack_loc": raw[1],
        "cell_num": m,
        "cells_v": cells_v,
        "temps_c": temps_c,
        "current_canonical_a": current_a,
        "pack_v": pack_v,
        "rem_cap_ah": rem_cap_ah,
        "full_cap_ah": total_cap_ah,
        "soc_pct": soc_pct,
        "cycles": cycles,
    }


def probe(args):
    import minimalmodbus
    import serial
    for baud in args.baud:
        for addr in args.ids:
            bms = minimalmodbus.Instrument(args.port, addr)
            try:
                bms.serial.baudrate = baud
                bms.serial.bytesize = 8
                bms.serial.parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[args.parity]
                bms.serial.stopbits = args.stop
                bms.serial.timeout = 0.4
                bms.clear_buffers_before_each_transaction = True
                raw = bms.read_register(REGS["vbus"], 0, functioncode=4)
                print(json.dumps(dict(id=addr, baud=baud, fc=4, vbus_raw=raw, vbus_candidate=raw / 100)), flush=True)
                if args.detail:
                    for name, reg in REGS.items():
                        try:
                            value = bms.read_register(reg, 0, functioncode=4)
                            print(json.dumps(dict(id=addr, register=hex(reg), name=name, raw=value, raw_hex=hex(value))))
                        except (minimalmodbus.ModbusException, OSError) as e:
                            print(f"{name}: {e}")
                        time.sleep(0.12)
            except (minimalmodbus.ModbusException, OSError) as e:
                print(f"id={addr} baud={baud}: {e}")
            finally:
                bms.serial.close()
            time.sleep(0.15)


def _serial_tx_rx(port: str, baud: int, tx: bytes, wait_s: float = 0.45, idle_gap_s: float = 0.08) -> bytes:
    import serial
    with serial.Serial(port, baud, bytesize=8, parity=serial.PARITY_NONE,
                       stopbits=1, timeout=0.04) as s:
        s.reset_input_buffer()
        s.reset_output_buffer()
        s.write(tx)
        s.flush()
        t_end = time.monotonic() + wait_s
        last_rx = None
        buf = bytearray()
        while time.monotonic() < t_end:
            chunk = s.read(512)
            if chunk:
                buf.extend(chunk)
                last_rx = time.monotonic()
            elif last_rx is not None and (time.monotonic() - last_rx) >= idle_gap_s:
                break
        return bytes(buf)


def _log_raw_rx(proto: str, baud: int, addr: int, rx: bytes):
    if rx:
        print(f"[RAW RX proto={proto} baud={baud} id={addr} len={len(rx)}] {rx.hex(' ')}")


def autodetect(args):
    """Read-only auto-detect across all 4 Shoto/TianBangda protocols over Autonics SCM-US48I."""
    found = []
    tbd_ids = args.ids if args.ids else [1, 2]
    modbus_ids = args.ids if args.ids else [61, 62, 214, 215, 38, 39, 1, 2, 3, 4]
    dcdc_ids = args.ids if args.ids else [61, 62, 214, 215, 1, 2, 3, 4]
    for baud in args.baud:
        print(f"=== Scanning baud {baud} (8N1, read-only) ===", flush=True)
        # 1. TianBangda Binary (TBD: try raw 0x7C/0x7E first for SD-BA430/V1.634, then 16x 0xAA fallback)
        for addr in tbd_ids:
            if not (1 <= addr <= 15):
                continue
            matched_tbd = False
            for hdr in (0x7C, 0x7E):
                for prefix in (b"", b"\xAA" * 16):
                    tx = prefix + build_tbd_poll(addr, hdr, 0x01)
                    rx = _serial_tx_rx(args.port, baud, tx, 0.45)
                    if len(rx) >= 48:
                        try:
                            dec = decode_tbd_telemetry(rx)
                            print("[FOUND] TBD_BINARY:", json.dumps(dec))
                            found.append(("PROTO_TBD_BINARY", baud, addr, dec))
                            matched_tbd = True
                            break
                        except ValueError:
                            pass
                    _log_raw_rx(f"TBD_BINARY_{hdr:02X}", baud, addr, rx)
                    if len(rx) > 0:
                        break
                    time.sleep(0.05)
                if matched_tbd:
                    break

        # 2. Shoto BMS Modbus V1.2 (0x0FFF..0x1025, 39 regs)
        for addr in modbus_ids:
            rx = _serial_tx_rx(args.port, baud, build_modbus_fc04(addr, 0x0FFF, 0x0027), 0.35)
            if len(rx) >= 83 and rx[2] >= 78 and len(rx) >= 3 + rx[2] + 2:
                try:
                    dec = decode_shoto_modbus_v12(rx[:3 + rx[2] + 2])
                    print("[FOUND] SHOTO_MODBUS_V12:", json.dumps(dec))
                    found.append(("PROTO_MODBUS_V12", baud, addr, dec))
                    continue
                except ValueError:
                    pass
            _log_raw_rx("SHOTO_MODBUS_V12", baud, addr, rx)
            time.sleep(0.05)

        # 3. SmartLi DC/DC Modbus (0x0000 Vbus / 0x1034 SOC; factory addr 214 or site addr 61)
        for addr in dcdc_ids:
            matched_dcdc = False
            for fc, reg, qty in ((0x04, 0x0000, 2), (0x03, 0x0000, 2), (0x04, 0x1030, 6), (0x03, 0x1030, 6)):
                rx = _serial_tx_rx(args.port, baud, build_modbus_read(addr, fc, reg, qty), 0.25)
                if not rx:
                    continue
                if len(rx) >= 5 and rx[0] == (addr & 0xFF) and rx[1] == fc and len(rx) == 3 + rx[2] + 2:
                    crc = (rx[-1] << 8) | rx[-2]
                    if modbus_crc(rx[:-2]) == crc and rx[2] >= 4:
                        w0 = (rx[3] << 8) | rx[4]
                        w1 = (rx[5] << 8) | rx[6]
                        dec = {"protocol": "SMARTLI_DCDC", "addr": addr, "fc": fc, "reg": hex(reg)}
                        if reg == 0x0000:
                            dec["vbus_v"] = round(w0 * 0.01, 2)
                            dec["vbat_v"] = round(w1 * 0.01, 2)
                        else:
                            dec["ibus_raw"] = w0
                            if len(rx) >= 15:
                                dec["soc"] = (rx[11] << 8) | rx[12]
                        print("[FOUND] SMARTLI_DCDC:", json.dumps(dec))
                        found.append(("PROTO_SMARTLI_DCDC", baud, addr, dec))
                        matched_dcdc = True
                        break
                _log_raw_rx(f"SMARTLI_DCDC_FC{fc:02X}", baud, addr, rx)
            if matched_dcdc:
                continue
            time.sleep(0.05)

        # 4. TianBangda YD/T 1363.3 ASCII (TBD_1363)
        for grp in ("01", "02"):
            tx = build_ydt1363_frame("26", "00", "46", "42", grp)
            rx = _serial_tx_rx(args.port, baud, tx, 0.35)
            matched_1363 = False
            for frame in re.findall(rb"~[^~\r]{0,2048}\r", rx):
                try:
                    info = inspect_ascii(frame)
                    if info["cid2"] == "00" and info["payload"]:
                        dec = decode_ydt1363_42(info["payload"])
                        print("[FOUND] TBD_1363:", json.dumps(dec))
                        found.append(("PROTO_TBD_1363", baud, int(grp, 16), dec))
                        matched_1363 = True
                except ValueError:
                    pass
            if rx and not matched_1363:
                _log_raw_rx("TBD_1363", baud, int(grp, 16), rx)
            time.sleep(0.05)

    if not found:
        print("\n[NO RESPONSE] Cek: (1) Baterai aktif & tombol RUN/RESET menyala, "
              "(2) Pinout RJ45 Pin 1 & 2, (3) Polaritas A(+)/B(-) pada adaptor RS485, "
              "(4) Pada software Windows tab 'DCDC Data' baris 16 cek nilai 'Modbus address' (mis. 61 atau 214) lalu uji `--ids <ADDR>`.")
    else:
        print("\n=== SUMMARY UNTUK firmware/shoto_samoto_bridge/config.h ===")
        priority = ("PROTO_TBD_BINARY", "PROTO_MODBUS_V12", "PROTO_TBD_1363", "PROTO_SMARTLI_DCDC")
        proto = next((p for p in priority if any(fp == p for fp, _, _, _ in found)), found[0][0])
        matches = [(b, a) for fp, b, a, _ in found if fp == proto]
        baud = matches[0][0]
        addrs = sorted({a for b, a in matches if b == baud})
        print(f"#define SHOTO_PROTOCOL {proto}")
        print(f"#define SHOTO_BAUD     {baud}")
        print(f"#define PACK_COUNT     {min(2, len(addrs))}")
        print(f"#define PACK1_ADDR     {addrs[0]}")
        if len(addrs) > 1:
            print(f"#define PACK2_ADDR     {addrs[1]}")


def capture(args):
    import serial
    # Must use a hardware adapter with TX physically inhibited (or controlled isolated tap) on monitored bus.
    with serial.Serial(args.port, args.baud, bytesize=8, parity=args.parity,
                       stopbits=args.stop, timeout=0.05) as s, open(args.out, "x", encoding="utf-8") as f:
        start = time.monotonic()
        while time.monotonic() - start < args.seconds:
            data = s.read(256)
            if data:
                row = dict(
                    utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    elapsed_ms=round((time.monotonic() - start) * 1000),
                    hex=data.hex(),
                )
                line = json.dumps(row)
                print(line, flush=True)
                f.write(line + "\n")
                f.flush()


def decode(args):
    buf = b"".join(
        bytes.fromhex(json.loads(line)["hex"])
        for line in Path(args.input).read_text().splitlines() if line.strip()
    )
    ascii_frames = re.findall(rb"~[^~\r]{0,2048}\r", buf)
    if ascii_frames:
        print(f"=== Found {len(ascii_frames)} ASCII (~...\\r) frame(s) ===")
        for frame in ascii_frames:
            try:
                info = inspect_ascii(frame)
                cid2_desc = CID2_NAMES.get(info["cid2"].upper(), "Unknown/Response")
                print("ASCII OK:", frame.decode("ascii", "replace").strip(), "->", {**info, "cid2_desc": cid2_desc})
            except ValueError as e:
                print("ASCII ERR:", frame, str(e))

    # Also check Modbus RTU / TianBangda binary candidates if present
    if not ascii_frames and buf:
        print(f"=== Raw buffer {len(buf)} bytes (no ~...\\r frames) ===")
        print("HEX:", buf[:256].hex(" "))
        for i in range(len(buf)):
            if buf[i] in (0x7C, 0x7E) and i + 6 <= len(buf) and buf[i + 5] == 0x0D:
                cand = buf[i:i + 6]
                if cand[4] == tbd_checksum(cand[:4]):
                    print(f"TBD Poll @+{i}: addr={cand[1]} cmd={cand[2]:#04x} hex={cand.hex(' ')}")
            if i + 8 <= len(buf) and buf[i + 1] == 0x04:
                cand = buf[i:i + 8]
                if modbus_crc(cand[:6]) == ((cand[7] << 8) | cand[6]):
                    reg = (cand[2] << 8) | cand[3]
                    cnt = (cand[4] << 8) | cand[5]
                    print(f"Modbus FC04 Req @+{i}: addr={cand[0]} start={reg:#06x} count={cnt}")
                elif i + 5 < len(buf):
                    blen = buf[i + 2]
                    if 4 <= blen <= 120 and i + 3 + blen + 2 <= len(buf):
                        resp = buf[i:i + 3 + blen + 2]
                        if modbus_crc(resp[:-2]) == ((resp[-1] << 8) | resp[-2]):
                            print(f"Modbus FC04 Resp @+{i}: addr={resp[0]} bytes={blen}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sp = p.add_subparsers(dest="mode", required=True)

    q = sp.add_parser("probe", help="Read-only SmartLi Modbus FC04 scan")
    q.add_argument("--port", required=True)
    q.add_argument("--baud", type=int, nargs="+", default=[9600])
    q.add_argument("--ids", type=int, nargs="+", default=[1])
    q.add_argument("--parity", choices=["N", "E", "O"], default="N")
    q.add_argument("--stop", type=int, choices=[1, 2], default=1)
    q.add_argument("--detail", action="store_true")
    q.set_defaults(fn=probe)

    q = sp.add_parser("autodetect", help="Read-only autodetect across all 4 Shoto/TBD protocols")
    q.add_argument("--port", required=True)
    q.add_argument("--baud", type=int, nargs="+", default=[9600, 19200])
    q.add_argument("--ids", type=int, nargs="+", default=None)
    q.set_defaults(fn=autodetect)

    q = sp.add_parser("capture", help="Passive byte capture to JSONL")
    q.add_argument("--port", required=True)
    q.add_argument("--baud", type=int, required=True)
    q.add_argument("--parity", choices=["N", "E", "O"], default="N")
    q.add_argument("--stop", type=int, choices=[1, 2], default=1)
    q.add_argument("--seconds", type=int, default=60)
    q.add_argument("--out", required=True)
    q.set_defaults(fn=capture)

    q = sp.add_parser("decode", help="Offline decode of captured JSONL")
    q.add_argument("input")
    q.set_defaults(fn=decode)

    a = p.parse_args()
    if a.mode == "probe" and any(not 1 <= i <= 247 for i in a.ids):
        p.error("address must be 1..247")
    if a.mode == "autodetect" and a.ids and any(not 1 <= i <= 247 for i in a.ids):
        p.error("address must be 1..247")
    a.fn(a)


if __name__ == "__main__":
    main()
