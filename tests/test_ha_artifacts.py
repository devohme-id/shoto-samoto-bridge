import json
import re
import shutil
import subprocess
import unittest
from pathlib import Path

try:
    import yaml as _pyyaml  # type: ignore
except ImportError:
    _pyyaml = None


REPO_ROOT = Path(__file__).resolve().parents[1]
HA_DIR = REPO_ROOT / "homeassistant"
CODEC_HPP = REPO_ROOT / "firmware" / "shoto_samoto_bridge" / "ha_mqtt_codec.hpp"
CONFIG_H = REPO_ROOT / "firmware" / "shoto_samoto_bridge" / "config.h"

BUILTIN_LOVELACE_CARDS = {
    "gauge",
    "entities",
    "entity",
    "history-graph",
    "markdown",
    "conditional",
    "horizontal-stack",
    "vertical-stack",
    "grid",
    "button",
    "tile",
    "statistics-graph",
}

HA_DISC_TYPE_TO_DOMAIN = {
    "HA_DISC_SENSOR": "sensor",
    "HA_DISC_BINARY_SENSOR": "binary_sensor",
    "HA_DISC_BUTTON": "button",
    "HA_DISC_SWITCH": "switch",
    "HA_DISC_SELECT": "select",
    "HA_DISC_NUMBER": "number",
}


def load_yaml_file(path: Path) -> dict:
    """Parse a YAML file using PyYAML if installed, else Ruby stdlib Psych."""
    text = path.read_text(encoding="utf-8")
    if _pyyaml is not None:
        # Support Home Assistant custom tags (!include, !secret, !env_var) safely.
        class _HALoader(_pyyaml.SafeLoader):
            pass

        for tag in ("!include", "!include_dir_list", "!include_dir_named",
                    "!include_dir_merge_list", "!include_dir_merge_named",
                    "!secret", "!env_var"):
            _HALoader.add_constructor(
                tag,
                lambda loader, node, _t=tag: f"{_t} {loader.construct_scalar(node)}",
            )
        data = _pyyaml.load(text, Loader=_HALoader)
    else:
        ruby_bin = shutil.which("ruby")
        if ruby_bin is None:
            raise RuntimeError(
                "YAML parser unavailable: install Python 'pyyaml' or ensure 'ruby' is on PATH."
            )
        ruby_script = (
            "require 'yaml'; require 'json'; "
            "data = YAML.load(ARGF.read); "
            "puts JSON.generate(data)"
        )
        proc = subprocess.run(
            [ruby_bin, "-e", ruby_script, str(path)],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise ValueError(f"YAML parse failed for {path}: {proc.stderr.strip()}")
        data = json.loads(proc.stdout)

    if not isinstance(data, dict):
        raise ValueError(f"Expected YAML mapping at top level of {path}")
    return data


def active_conf_lines(path: Path) -> list[str]:
    """Return stripped non-empty, non-comment lines from a config file."""
    lines = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            lines.append(line)
    return lines


class TestHomeAssistantArtifacts(unittest.TestCase):
    def test_configuration_yaml_http_hardening(self):
        cfg = load_yaml_file(HA_DIR / "configuration.yaml")
        self.assertIn("http", cfg)
        http = cfg["http"]
        self.assertIsInstance(http, dict)
        self.assertIs(http.get("use_x_forwarded_for"), True)
        self.assertIs(http.get("ip_ban_enabled"), True)

        threshold = http.get("login_attempts_threshold")
        self.assertIsInstance(threshold, int)
        self.assertGreaterEqual(threshold, 1)
        self.assertLessEqual(threshold, 5)

        proxies = http.get("trusted_proxies")
        self.assertIsInstance(proxies, list)
        for required_proxy in (
            "127.0.0.1",
            "::1",
            "172.30.33.0/24",
            "172.16.0.0/12",
            "192.168.1.50",
        ):
            self.assertIn(required_proxy, proxies)

    def test_configuration_yaml_recorder_ssd_protection(self):
        cfg = load_yaml_file(HA_DIR / "configuration.yaml")
        self.assertIn("recorder", cfg)
        rec = cfg["recorder"]
        self.assertIsInstance(rec, dict)
        self.assertEqual(rec.get("purge_keep_days"), 30)
        self.assertEqual(rec.get("commit_interval"), 15)

        exclude = rec.get("exclude", {})
        self.assertIsInstance(exclude, dict)
        globs = exclude.get("entity_globs", [])
        self.assertIsInstance(globs, list)
        for tx_glob in ("sensor.*_diag_tx61", "sensor.*_diag_tx62", "sensor.*_diag_tx63"):
            self.assertIn(tx_glob, globs)

        # Ensure bank/pack/cell/EEPROM history is NOT excluded.
        joined = " ".join(globs)
        for must_not_exclude in ("bank", "pack", "cell", "eeprom"):
            self.assertNotIn(must_not_exclude, joined.lower())

    def test_configuration_yaml_includes_and_shoto_notes(self):
        cfg_path = HA_DIR / "configuration.yaml"
        cfg = load_yaml_file(cfg_path)
        self.assertIn("automation", cfg)
        self.assertIn("script", cfg)

        raw = cfg_path.read_text(encoding="utf-8")
        for ref in (
            "automations_shoto_bms.yaml",
            "scripts_shoto_bms.yaml",
            "dashboards/shoto_bms_dashboard.yaml",
        ):
            self.assertIn(ref, raw)

    def test_mosquitto_conf_hardening(self):
        conf_path = HA_DIR / "mosquitto" / "mosquitto.conf"
        lines = active_conf_lines(conf_path)
        self.assertIn("allow_anonymous false", lines)
        self.assertIn("password_file /mosquitto/config/passwd", lines)
        self.assertTrue(any(re.match(r"^listener\s+1883(\s|$)", ln) for ln in lines))
        self.assertIn("persistence true", lines)

    def test_docker_compose_hardening(self):
        compose = load_yaml_file(HA_DIR / "docker-compose.yml")
        services = compose.get("services")
        self.assertIsInstance(services, dict)
        for svc_name in ("homeassistant", "mosquitto", "caddy"):
            self.assertIn(svc_name, services)

        self.assertEqual(
            services["homeassistant"].get("image"),
            "ghcr.io/home-assistant/home-assistant:stable",
        )
        self.assertEqual(services["mosquitto"].get("image"), "eclipse-mosquitto:2")
        self.assertEqual(services["caddy"].get("image"), "caddy:2-alpine")

        # Caddy must only expose HTTPS 443 (and optional ACME HTTP 80), never 1883/8123/22.
        caddy_ports = [str(p) for p in services["caddy"].get("ports", [])]
        self.assertTrue(any("443" in p for p in caddy_ports))
        for p in caddy_ports:
            self.assertNotIn("1883", p)
            self.assertNotIn("8123", p)
            self.assertNotIn(":22", p)

        # Mosquitto must expose 1883:1883 for LAN / local ESP32 clients.
        mosq_ports = [str(p) for p in services["mosquitto"].get("ports", [])]
        self.assertTrue(any("1883:1883" in p for p in mosq_ports))

        # Home Assistant must not expose bare 0.0.0.0:8123 or public 8123:8123 without loopback.
        ha_svc = services["homeassistant"]
        ha_ports = [str(p) for p in ha_svc.get("ports", [])]
        for p in ha_ports:
            self.assertFalse(p.startswith("8123:"), f"Bare 8123 port exposed: {p}")
            self.assertFalse(p.startswith("0.0.0.0:8123:"), f"0.0.0.0:8123 exposed: {p}")

    def test_caddyfile_example_hardening(self):
        caddy_path = HA_DIR / "Caddyfile.example"
        lines = active_conf_lines(caddy_path)
        joined = "\n".join(lines)

        self.assertIn("{$HA_DOMAIN}", joined)
        self.assertIn("reverse_proxy", joined)
        self.assertIn(":8123", joined)

        # No site block should listen on :8123, :1883, or :22, nor proxy 1883/22.
        for ln in lines:
            self.assertNotRegex(ln, r"^:?(8123|1883|22)\b")
            self.assertNotIn(":1883", ln)
            self.assertNotIn(":22", ln)

    def test_env_example_variables_and_no_real_secrets(self):
        env_path = HA_DIR / ".env.example"
        raw = env_path.read_text(encoding="utf-8")
        lines = active_conf_lines(env_path)
        kv = {}
        for ln in lines:
            if "=" in ln:
                k, v = ln.split("=", 1)
                kv[k.strip()] = v.strip()

        self.assertEqual(kv.get("TZ"), "Asia/Jakarta")
        self.assertIn("HA_DOMAIN", kv)
        self.assertTrue(kv["HA_DOMAIN"])

        # Ensure placeholder values are used, not real credentials/tokens.
        for forbidden in ("ghp_", "glpat-", "BEGIN PRIVATE KEY", "BEGIN RSA PRIVATE KEY"):
            self.assertNotIn(forbidden, raw)


def parse_ha_device_id() -> str:
    text = CONFIG_H.read_text(encoding="utf-8")
    m = re.search(r'#define\s+HA_DEVICE_ID\s+"([^"]+)"', text)
    if not m:
        raise AssertionError("HA_DEVICE_ID not found in config.h")
    return m.group(1)


def parse_discovery_entities(dev_id: str, max_packs: int = 4) -> set[str]:
    text = CODEC_HPP.read_text(encoding="utf-8")

    def _extract_array_body(arr_name: str) -> str:
        m = re.search(
            rf"static\s+const\s+HaDiscoveryDesc\s+{arr_name}\[\]\s*=\s*\{{(.*?)\n\}};",
            text,
            re.DOTALL,
        )
        if not m:
            raise AssertionError(f"Could not locate {arr_name}[] in ha_mqtt_codec.hpp")
        return m.group(1)

    entry_re = re.compile(
        r'\{\s*(HA_DISC_[A-Z_]+)\s*,\s*"([a-z0-9_]+)"\s*,',
        re.MULTILINE,
    )

    entities: set[str] = set()
    for disc_type, obj_id in entry_re.findall(_extract_array_body("kGlobalDiscovery")):
        domain = HA_DISC_TYPE_TO_DOMAIN[disc_type]
        entities.add(f"{domain}.{dev_id}_{obj_id}")

    pack_static = entry_re.findall(_extract_array_body("kPackStaticDesc"))
    for slot in range(1, max_packs + 1):
        for disc_type, obj_id in pack_static:
            domain = HA_DISC_TYPE_TO_DOMAIN[disc_type]
            entities.add(f"{domain}.{dev_id}_pack{slot}_{obj_id}")
        for t in range(1, 6):
            entities.add(f"sensor.{dev_id}_pack{slot}_temp_{t}")
        for c in range(1, 16):
            entities.add(f"sensor.{dev_id}_pack{slot}_cell_{c:02d}")
    return entities


def parse_eeprom_json_keys() -> set[str]:
    text = CODEC_HPP.read_text(encoding="utf-8")
    m = re.search(
        r"inline\s+size_t\s+build_pack_eeprom_json\s*\([^)]*\)\s*\{(.*?)\n\}",
        text,
        re.DOTALL,
    )
    if not m:
        raise AssertionError("Could not locate build_pack_eeprom_json() in ha_mqtt_codec.hpp")
    body = m.group(1)
    return set(re.findall(r'w\.kv_[a-z0-9]+\(\s*"([a-z0-9_]+)"', body))


def walk_lovelace_nodes(node, card_types: list[str], strings: list[str]) -> None:
    if isinstance(node, dict):
        if "type" in node and isinstance(node["type"], str):
            card_types.append(node["type"])
        for k, v in node.items():
            if k == "type":
                continue
            walk_lovelace_nodes(v, card_types, strings)
    elif isinstance(node, list):
        for item in node:
            walk_lovelace_nodes(item, card_types, strings)
    elif isinstance(node, str):
        strings.append(node)


class TestShotoLovelaceDashboard(unittest.TestCase):
    DASHBOARD_PATH = HA_DIR / "dashboards" / "shoto_bms_dashboard.yaml"

    def test_dashboard_yaml_structure_and_builtin_cards(self):
        self.assertTrue(
            self.DASHBOARD_PATH.is_file(),
            f"Missing dashboard YAML: {self.DASHBOARD_PATH}",
        )
        dash = load_yaml_file(self.DASHBOARD_PATH)
        views = dash.get("views")
        self.assertIsInstance(views, list)
        self.assertEqual(len(views), 4, "Dashboard must have exactly 4 views")

        expected_titles = [
            "Overview & Power Flow",
            "Packs & 15-Cell Matrix",
            "Shoto EEPROM Audit (Read-Only)",
            "Action Center & Diagnostics",
        ]
        self.assertEqual([v.get("title") for v in views], expected_titles)

        card_types: list[str] = []
        strings: list[str] = []
        for view in views:
            self.assertIsInstance(view, dict)
            self.assertTrue(view.get("path"))
            self.assertTrue(view.get("cards"))
            walk_lovelace_nodes(view["cards"], card_types, strings)

        self.assertGreater(len(card_types), 0)
        for ctype in card_types:
            self.assertFalse(
                ctype.startswith("custom:"),
                f"HACS/custom card forbidden: {ctype}",
            )
            self.assertIn(
                ctype,
                BUILTIN_LOVELACE_CARDS,
                f"Non-builtin Lovelace card type used: {ctype}",
            )

    def test_dashboard_entities_match_firmware_discovery_exact(self):
        dev_id = parse_ha_device_id()
        self.assertEqual(dev_id, "shoto_ils6300")
        expected_entities = parse_discovery_entities(dev_id, max_packs=4)
        # 64 global + 4 * (17 static + 5 temps + 15 cells = 37) = 212
        self.assertEqual(len(expected_entities), 212)

        dash = load_yaml_file(self.DASHBOARD_PATH)
        card_types: list[str] = []
        strings: list[str] = []
        walk_lovelace_nodes(dash["views"], card_types, strings)

        entity_re = re.compile(
            r"\b(?:sensor|binary_sensor|button|switch|select|number)\.[a-z0-9_]+\b"
        )
        referenced_entities: set[str] = set()
        for s in strings:
            referenced_entities.update(entity_re.findall(s))

        unknown = sorted(referenced_entities - expected_entities)
        self.assertEqual(
            unknown,
            [],
            f"Dashboard references unknown/orphan entities not in ha_mqtt_codec.hpp: {unknown}",
        )

        # Verify all 60 cell entities (Pack 1..4 x Cell 01..15) are shown
        for slot in range(1, 5):
            for cell in range(1, 16):
                eid = f"sensor.{dev_id}_pack{slot}_cell_{cell:02d}"
                self.assertIn(eid, referenced_entities, f"Missing cell entity in dashboard: {eid}")
            for temp in range(1, 6):
                eid = f"sensor.{dev_id}_pack{slot}_temp_{temp}"
                self.assertIn(eid, referenced_entities, f"Missing temp entity in dashboard: {eid}")
            self.assertIn(f"binary_sensor.{dev_id}_pack{slot}_eep_valid", referenced_entities)

        # Verify key global & guarded controls are present
        required_global = [
            f"sensor.{dev_id}_bank_soc",
            f"sensor.{dev_id}_bank_voltage",
            f"sensor.{dev_id}_bank_current",
            f"sensor.{dev_id}_bank_power",
            f"binary_sensor.{dev_id}_bank_valid",
            f"sensor.{dev_id}_diag_inv_mode",
            f"switch.{dev_id}_sw_maint_unlock",
            f"sensor.{dev_id}_maint_unlock_rem_s",
            f"select.{dev_id}_sel_bridge_mode",
            f"switch.{dev_id}_sw_dyn_discovery",
            f"number.{dev_id}_num_policy_cvl",
            f"number.{dev_id}_num_policy_dvl",
            f"number.{dev_id}_num_policy_ccl",
            f"number.{dev_id}_num_policy_dcl",
            f"number.{dev_id}_num_policy_max_dv",
            f"button.{dev_id}_btn_policy_reset",
            f"button.{dev_id}_btn_restart",
        ]
        for eid in required_global:
            self.assertIn(eid, referenced_entities, f"Missing required entity in dashboard: {eid}")

    def test_dashboard_view3_eeprom_attributes_match_codec(self):
        eeprom_keys = parse_eeprom_json_keys()
        prot_keys = {k for k in eeprom_keys if k.startswith("p_") and k != "p_ok"}
        alarm_keys = {k for k in eeprom_keys if k.startswith("a_") and k != "a_ok"}
        self.assertEqual(len(prot_keys), 13)
        self.assertEqual(len(alarm_keys), 43)

        dash = load_yaml_file(self.DASHBOARD_PATH)
        view3 = dash["views"][2]
        card_types: list[str] = []
        strings: list[str] = []
        walk_lovelace_nodes(view3, card_types, strings)
        joined = "\n".join(strings)

        attr_re = re.compile(
            r"state_attr\(\s*['\"](binary_sensor\.shoto_ils6300_pack([1-4])_eep_valid)['\"]\s*,\s*['\"]([a-z0-9_]+)['\"]\s*\)"
        )
        matches = attr_re.findall(joined)
        self.assertGreater(len(matches), 0, "View 3 must render state_attr() for pack*_eep_valid")

        by_pack: dict[str, set[str]] = {"1": set(), "2": set(), "3": set(), "4": set()}
        for _eid, pack_num, attr_key in matches:
            self.assertIn(
                attr_key,
                eeprom_keys,
                f"View 3 references unknown EEPROM attribute '{attr_key}'",
            )
            by_pack[pack_num].add(attr_key)

        required_all = prot_keys | alarm_keys
        for pack_num in ("1", "2", "3", "4"):
            missing = sorted(required_all - by_pack[pack_num])
            self.assertEqual(
                missing,
                [],
                f"Pack {pack_num} in View 3 missing EEPROM attributes: {missing}",
            )


if __name__ == "__main__":
    unittest.main()
