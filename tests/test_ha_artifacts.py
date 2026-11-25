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


def load_yaml_any(path: Path):
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
        return _pyyaml.load(text, Loader=_HALoader)

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
    return json.loads(proc.stdout)


def load_yaml_file(path: Path) -> dict:
    """Parse a YAML mapping file using PyYAML if installed, else Ruby stdlib Psych."""
    data = load_yaml_any(path)
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


def walk_lovelace_nodes(
    node, card_types: list[str], strings: list[str], *, is_card: bool = False
) -> None:
    if isinstance(node, dict):
        if is_card and "type" in node and isinstance(node["type"], str):
            card_types.append(node["type"])
        for k, v in node.items():
            if is_card and k == "type":
                continue
            if k == "cards" and isinstance(v, list):
                for child in v:
                    walk_lovelace_nodes(child, card_types, strings, is_card=True)
            elif k == "card" and isinstance(v, dict):
                walk_lovelace_nodes(v, card_types, strings, is_card=True)
            else:
                walk_lovelace_nodes(v, card_types, strings, is_card=False)
    elif isinstance(node, list):
        for item in node:
            walk_lovelace_nodes(item, card_types, strings, is_card=is_card)
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

        paths = [v.get("path") for v in views]
        self.assertTrue(all(isinstance(p, str) and p for p in paths))
        self.assertEqual(len(set(paths)), 4, "View paths must be unique")

        card_types: list[str] = []
        strings: list[str] = []
        for view in views:
            self.assertIsInstance(view, dict)
            self.assertTrue(view.get("cards"))
            walk_lovelace_nodes(view["cards"], card_types, strings, is_card=True)

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

        # View 1 tile grid must use 2 columns for mobile readability
        view1_grids = [c for c in views[0]["cards"] if c.get("type") == "grid"]
        self.assertTrue(view1_grids)
        for g in view1_grids:
            self.assertEqual(g.get("columns"), 2)

    def test_dashboard_entities_match_firmware_discovery_exact(self):
        dev_id = parse_ha_device_id()
        self.assertEqual(dev_id, "shoto_ils6300")
        expected_entities = parse_discovery_entities(dev_id, max_packs=4)
        # 64 global + 4 * (17 static + 5 temps + 15 cells = 37) = 212
        self.assertEqual(len(expected_entities), 212)

        dash = load_yaml_file(self.DASHBOARD_PATH)
        card_types: list[str] = []
        strings: list[str] = []
        walk_lovelace_nodes(dash["views"], card_types, strings, is_card=False)

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
        missing_all = sorted(expected_entities - referenced_entities)
        self.assertEqual(
            missing_all,
            [],
            f"Dashboard is missing firmware discovery entities: {missing_all}",
        )

    def test_dashboard_view3_eeprom_attributes_and_guards(self):
        eeprom_keys = parse_eeprom_json_keys()
        prot_keys = {k for k in eeprom_keys if k.startswith("p_") and k != "p_ok"}
        alarm_keys = {k for k in eeprom_keys if k.startswith("a_") and k != "a_ok"}
        self.assertEqual(len(prot_keys), 13)
        self.assertEqual(len(alarm_keys), 43)

        dash = load_yaml_file(self.DASHBOARD_PATH)
        view3 = dash["views"][2]
        card_types: list[str] = []
        strings: list[str] = []
        walk_lovelace_nodes(view3["cards"], card_types, strings, is_card=True)
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
            self.assertIn(
                f"state_attr('binary_sensor.shoto_ils6300_pack{pack_num}_eep_valid', 'valid')",
                joined,
            )

        self.assertIn("Belum ada data audit EEPROM", joined)

        cond_entities = {
            cond.get("entity"): cond.get("state")
            for card in view3["cards"]
            if card.get("type") == "conditional"
            for cond in card.get("conditions", [])
        }
        self.assertEqual(cond_entities.get("binary_sensor.shoto_ils6300_pack3_online"), "on")
        self.assertEqual(cond_entities.get("binary_sensor.shoto_ils6300_pack4_online"), "on")

    def test_dashboard_view4_critical_controls_gated_by_maint_unlock(self):
        dash = load_yaml_file(self.DASHBOARD_PATH)
        view4 = dash["views"][3]

        # Top-level Kontrol Kritis must NOT be exposed outside a conditional guard.
        for card in view4["cards"]:
            self.assertNotIn(
                "Kontrol Kritis",
                card.get("title", ""),
                "Kontrol Kritis must be gated inside conditional sw_maint_unlock == on",
            )

        # Find conditional cards in View 4 (inside vertical-stack or top-level).
        conditionals: list[dict] = []

        def _collect_conditionals(node) -> None:
            if isinstance(node, dict):
                if node.get("type") == "conditional":
                    conditionals.append(node)
                for v in node.values():
                    _collect_conditionals(v)
            elif isinstance(node, list):
                for item in node:
                    _collect_conditionals(item)

        _collect_conditionals(view4["cards"])

        unlock_on_cards = [
            c["card"]
            for c in conditionals
            if any(
                cond.get("entity") == "switch.shoto_ils6300_sw_maint_unlock"
                and cond.get("state") == "on"
                for cond in c.get("conditions", [])
            )
        ]
        unlock_off_cards = [
            c["card"]
            for c in conditionals
            if any(
                cond.get("entity") == "switch.shoto_ils6300_sw_maint_unlock"
                and cond.get("state") == "off"
                for cond in c.get("conditions", [])
            )
        ]

        self.assertEqual(len(unlock_on_cards), 1, "Expected 1 conditional card for sw_maint_unlock == on")
        self.assertEqual(len(unlock_off_cards), 1, "Expected 1 conditional card for sw_maint_unlock == off")

        on_types: list[str] = []
        on_strings: list[str] = []
        walk_lovelace_nodes(unlock_on_cards[0], on_types, on_strings, is_card=True)
        on_joined = "\n".join(on_strings)
        for critical_ent in (
            "select.shoto_ils6300_sel_bridge_mode",
            "switch.shoto_ils6300_sw_dyn_discovery",
            "number.shoto_ils6300_num_policy_cvl",
            "number.shoto_ils6300_num_policy_dvl",
            "number.shoto_ils6300_num_policy_ccl",
            "number.shoto_ils6300_num_policy_dcl",
            "number.shoto_ils6300_num_policy_max_dv",
            "button.shoto_ils6300_btn_policy_reset",
            "button.shoto_ils6300_btn_restart",
        ):
            self.assertIn(critical_ent, on_joined)


def parse_ha_duration_s(val) -> int:
    """Parse a Home Assistant duration ('HH:MM:SS', int seconds, or dict) into seconds."""
    if isinstance(val, (int, float)):
        return int(val)
    if isinstance(val, str):
        parts = val.strip().split(":")
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            return int(parts[0]) * 3600 + int(parts[1]) * 60 + int(parts[2])
    if isinstance(val, dict):
        return (
            int(val.get("hours", 0)) * 3600
            + int(val.get("minutes", 0)) * 60
            + int(val.get("seconds", 0))
        )
    raise AssertionError(f"Unsupported HA duration format: {val!r}")


def collect_yaml_strings(node, out: list[str]) -> None:
    if isinstance(node, dict):
        for v in node.values():
            collect_yaml_strings(v, out)
    elif isinstance(node, list):
        for item in node:
            collect_yaml_strings(item, out)
    elif isinstance(node, str):
        out.append(node)


def extract_action_steps(actions) -> list[dict]:
    """Flatten action/sequence step dicts (including choose/if branches) for inspection."""
    steps: list[dict] = []
    if isinstance(actions, dict):
        actions = [actions]
    if not isinstance(actions, list):
        return steps
    for step in actions:
        if not isinstance(step, dict):
            continue
        steps.append(step)
        if "choose" in step and isinstance(step["choose"], list):
            for branch in step["choose"]:
                if isinstance(branch, dict):
                    steps.extend(extract_action_steps(branch.get("sequence", [])))
            if "default" in step:
                steps.extend(extract_action_steps(step["default"]))
        for sub_key in ("then", "else", "sequence"):
            if sub_key in step:
                steps.extend(extract_action_steps(step[sub_key]))
    return steps


class TestShotoAutomationsAndScripts(unittest.TestCase):
    AUTOMATIONS_PATH = HA_DIR / "automations_shoto_bms.yaml"
    SCRIPTS_PATH = HA_DIR / "scripts_shoto_bms.yaml"

    REQUIRED_AUTOMATION_IDS = (
        "shoto_bms_bank_inhibit_alert",
        "shoto_bms_pack_offline_alert",
        "shoto_bms_low_soc_warning",
        "shoto_bms_low_soc_critical",
        "shoto_bms_cell_imbalance_alert",
        "shoto_bms_high_temp_alert",
        "shoto_bms_bridge_or_inverter_stalled_alert",
        "shoto_bms_maint_unlock_auto_relock",
    )

    REQUIRED_SCRIPT_IDS = (
        "shoto_test_notification",
        "shoto_audit_all_packs_and_notify",
    )

    def test_automations_yaml_structure_triggers_and_safety(self):
        self.assertTrue(
            self.AUTOMATIONS_PATH.is_file(),
            f"Missing automations YAML: {self.AUTOMATIONS_PATH}",
        )
        automations = load_yaml_any(self.AUTOMATIONS_PATH)
        self.assertIsInstance(automations, list)
        self.assertGreaterEqual(len(automations), len(self.REQUIRED_AUTOMATION_IDS))

        seen_ids: set[str] = set()
        seen_aliases: set[str] = set()
        by_id: dict[str, dict] = {}
        for item in automations:
            self.assertIsInstance(item, dict)
            aid = item.get("id")
            alias = item.get("alias")
            self.assertIsInstance(aid, str)
            self.assertTrue(aid.strip())
            self.assertIsInstance(alias, str)
            self.assertTrue(alias.strip())
            self.assertNotIn(aid, seen_ids, f"Duplicate automation id: {aid}")
            self.assertNotIn(alias, seen_aliases, f"Duplicate automation alias: {alias}")
            seen_ids.add(aid)
            seen_aliases.add(alias)
            self.assertTrue(item.get("trigger") or item.get("triggers"))
            self.assertTrue(item.get("action") or item.get("actions"))
            by_id[aid] = item

        for req_id in self.REQUIRED_AUTOMATION_IDS:
            self.assertIn(req_id, by_id, f"Missing required automation: {req_id}")

        # 1. Bank inhibit alert
        inhibit_triggers = by_id["shoto_bms_bank_inhibit_alert"].get(
            "trigger", by_id["shoto_bms_bank_inhibit_alert"].get("triggers")
        )
        self.assertEqual(inhibit_triggers[0].get("entity_id"), "binary_sensor.shoto_ils6300_bank_valid")
        self.assertEqual(inhibit_triggers[0].get("to"), "off")
        self.assertEqual(parse_ha_duration_s(inhibit_triggers[0].get("for")), 10)

        # 2. Pack offline / hot-unplug alert (only on->off for 30s so unused slots 3/4 don't false-alert)
        pack_off_triggers = by_id["shoto_bms_pack_offline_alert"].get(
            "trigger", by_id["shoto_bms_pack_offline_alert"].get("triggers")
        )
        self.assertEqual(
            pack_off_triggers[0].get("entity_id"),
            [f"binary_sensor.shoto_ils6300_pack{i}_online" for i in range(1, 5)],
        )
        self.assertEqual(pack_off_triggers[0].get("from"), "on")
        self.assertEqual(pack_off_triggers[0].get("to"), "off")
        self.assertEqual(parse_ha_duration_s(pack_off_triggers[0].get("for")), 30)

        # 3 & 4. Low SOC warning (<= 25%) & critical (<= 20%) via exact template triggers
        soc_warn_tr = by_id["shoto_bms_low_soc_warning"].get(
            "trigger", by_id["shoto_bms_low_soc_warning"].get("triggers")
        )
        self.assertEqual(soc_warn_tr[0].get("platform", soc_warn_tr[0].get("trigger")), "template")
        soc_warn_tpl = soc_warn_tr[0].get("value_template", "")
        self.assertIn("is_number(states('sensor.shoto_ils6300_bank_soc'))", soc_warn_tpl)
        self.assertIn("<= 25", soc_warn_tpl)
        self.assertEqual(parse_ha_duration_s(soc_warn_tr[0].get("for")), 60)

        soc_crit_tr = by_id["shoto_bms_low_soc_critical"].get(
            "trigger", by_id["shoto_bms_low_soc_critical"].get("triggers")
        )
        self.assertEqual(soc_crit_tr[0].get("platform", soc_crit_tr[0].get("trigger")), "template")
        soc_crit_tpl = soc_crit_tr[0].get("value_template", "")
        self.assertIn("is_number(states('sensor.shoto_ils6300_bank_soc'))", soc_crit_tpl)
        self.assertIn("<= 20", soc_crit_tpl)
        self.assertEqual(parse_ha_duration_s(soc_crit_tr[0].get("for")), 30)

        # 5. Cell imbalance alert (>= 80 mV) via exact template trigger
        dv_tr = by_id["shoto_bms_cell_imbalance_alert"].get(
            "trigger", by_id["shoto_bms_cell_imbalance_alert"].get("triggers")
        )
        self.assertEqual(dv_tr[0].get("platform", dv_tr[0].get("trigger")), "template")
        dv_tpl = dv_tr[0].get("value_template", "")
        self.assertIn("is_number(states('sensor.shoto_ils6300_bank_cell_dv_mv'))", dv_tpl)
        self.assertIn(">= 80", dv_tpl)
        self.assertEqual(parse_ha_duration_s(dv_tr[0].get("for")), 60)

        # 6. High temp alert (>= 48 C) via exact template trigger
        temp_tr = by_id["shoto_bms_high_temp_alert"].get(
            "trigger", by_id["shoto_bms_high_temp_alert"].get("triggers")
        )
        self.assertEqual(temp_tr[0].get("platform", temp_tr[0].get("trigger")), "template")
        temp_tpl = temp_tr[0].get("value_template", "")
        self.assertIn("is_number(states('sensor.shoto_ils6300_bank_temp_max'))", temp_tpl)
        self.assertIn(">= 48", temp_tpl)
        self.assertEqual(parse_ha_duration_s(temp_tr[0].get("for")), 60)

        # 7. Bridge offline / Inverter 63H stalled > 2 min
        stalled_auto = by_id["shoto_bms_bridge_or_inverter_stalled_alert"]
        stalled_tr = stalled_auto.get("trigger", stalled_auto.get("triggers"))
        stalled_by_id = {t.get("id"): t for t in stalled_tr if isinstance(t, dict)}
        self.assertIn("bridge_offline", stalled_by_id)
        self.assertIn("inv_63_stalled", stalled_by_id)
        bridge_offline_tr = stalled_by_id["bridge_offline"]
        self.assertEqual(bridge_offline_tr.get("entity_id"), "binary_sensor.shoto_ils6300_bank_valid")
        self.assertEqual(bridge_offline_tr.get("to"), "unavailable")
        self.assertEqual(parse_ha_duration_s(bridge_offline_tr.get("for")), 120)

        stalled_strings: list[str] = []
        collect_yaml_strings(stalled_auto, stalled_strings)
        stalled_joined = "\n".join(stalled_strings)
        self.assertIn("binary_sensor.shoto_ils6300_bank_valid", stalled_joined)
        self.assertIn("sensor.shoto_ils6300_diag_inv_63", stalled_joined)
        self.assertIn("unavailable", stalled_joined)
        self.assertIn("> 120", stalled_joined)

        # 8. Maintenance unlock backup auto-relock <= 300s (5 minutes)
        relock_auto = by_id["shoto_bms_maint_unlock_auto_relock"]
        relock_tr = relock_auto.get("trigger", relock_auto.get("triggers"))
        self.assertEqual(relock_tr[0].get("entity_id"), "switch.shoto_ils6300_sw_maint_unlock")
        self.assertEqual(relock_tr[0].get("to"), "on")
        relock_s = parse_ha_duration_s(relock_tr[0].get("for"))
        self.assertGreater(relock_s, 0)
        self.assertLessEqual(relock_s, 300, "Maintenance unlock auto-relock must be <= 5 minutes (300s)")
        relock_steps = extract_action_steps(relock_auto.get("action", relock_auto.get("actions")))
        self.assertTrue(
            any(
                s.get("service", s.get("action")) == "switch.turn_off"
                and s.get("target", {}).get("entity_id") == "switch.shoto_ils6300_sw_maint_unlock"
                for s in relock_steps
            ),
            "Auto-relock must call switch.turn_off on switch.shoto_ils6300_sw_maint_unlock",
        )

        # Every alert automation must create persistent_notification and guard notify.notify with continue_on_error: true
        for alert_id in self.REQUIRED_AUTOMATION_IDS[:-1]:
            auto = by_id[alert_id]
            steps = extract_action_steps(auto.get("action", auto.get("actions")))
            services = [s.get("service", s.get("action")) for s in steps]
            self.assertIn("persistent_notification.create", services, f"{alert_id} missing persistent_notification.create")
            notify_steps = [s for s in steps if s.get("service", s.get("action")) == "notify.notify"]
            self.assertTrue(notify_steps, f"{alert_id} missing notify.notify")
            for ns in notify_steps:
                self.assertIs(ns.get("continue_on_error"), True, f"{alert_id} notify.notify must set continue_on_error: true")
            strings: list[str] = []
            collect_yaml_strings(auto, strings)
            self.assertIn("PLTS Samoto ILS6300", "\n".join(strings), f"{alert_id} missing HA_SITE_NAME")

    def test_scripts_yaml_structure_and_read_only_audit(self):
        self.assertTrue(
            self.SCRIPTS_PATH.is_file(),
            f"Missing scripts YAML: {self.SCRIPTS_PATH}",
        )
        scripts = load_yaml_file(self.SCRIPTS_PATH)
        for req_id in self.REQUIRED_SCRIPT_IDS:
            self.assertIn(req_id, scripts, f"Missing required script: {req_id}")
            sc = scripts[req_id]
            self.assertIsInstance(sc, dict)
            self.assertTrue(sc.get("alias"))
            self.assertIsInstance(sc.get("sequence"), list)
            self.assertGreater(len(sc["sequence"]), 0)

        # Test notification script
        test_notif = scripts["shoto_test_notification"]
        test_steps = extract_action_steps(test_notif["sequence"])
        test_services = [s.get("service", s.get("action")) for s in test_steps]
        self.assertIn("persistent_notification.create", test_services)
        self.assertIn("notify.notify", test_services)
        for ns in [s for s in test_steps if s.get("service", s.get("action")) == "notify.notify"]:
            self.assertIs(ns.get("continue_on_error"), True)

        # Audit EEPROM all packs script (strictly read-only)
        audit_sc = scripts["shoto_audit_all_packs_and_notify"]
        audit_steps = extract_action_steps(audit_sc["sequence"])
        self.assertTrue(
            any(
                s.get("service", s.get("action")) == "button.press"
                and s.get("target", {}).get("entity_id") == "button.shoto_ils6300_btn_audit_eeprom_all"
                for s in audit_steps
            ),
            "Audit script must press button.shoto_ils6300_btn_audit_eeprom_all",
        )
        delays = [parse_ha_duration_s(s["delay"]) for s in audit_steps if "delay" in s]
        self.assertTrue(any(d >= 25 for d in delays), "Audit script must wait >= 25s for 1..4 pack EEPROM scan completion")
        audit_strings: list[str] = []
        collect_yaml_strings(audit_sc, audit_strings)
        audit_joined = "\n".join(audit_strings)
        for slot in range(1, 5):
            self.assertIn(f"binary_sensor.shoto_ils6300_pack{slot}_eep_valid", audit_joined)
        audit_services = [s.get("service", s.get("action")) for s in audit_steps]
        self.assertIn("persistent_notification.create", audit_services)
        self.assertIn("notify.notify", audit_services)
        audit_notify_steps = [
            s for s in audit_steps if s.get("service", s.get("action")) == "notify.notify"
        ]
        for ns in audit_notify_steps:
            self.assertIs(ns.get("continue_on_error"), True)
            msg = ns.get("data", {}).get("message", "")
            self.assertIn("namespace(parts=[])", msg)
            self.assertIn("| join(', ')", msg)
            self.assertNotIn("loop.last", msg)
            try:
                import jinja2  # type: ignore
            except ImportError:
                jinja2 = None
            if jinja2 is not None:
                # Render 2-pack online scenario (pack 3 & 4 offline) to ensure no trailing comma
                env = jinja2.Environment()
                rendered = env.from_string(msg).render(
                    state_attr=lambda eid, key: (
                        True if key == "valid" and eid.endswith(("pack1_eep_valid", "pack2_eep_valid"))
                        else (3650 if key == "p_cov" else (2800 if key == "p_cuv" else None))
                    ),
                    is_state=lambda _eid, _val: False,
                ).strip()
                self.assertFalse(rendered.endswith(","), f"Trailing comma in rendered push message: {rendered!r}")
                self.assertEqual(
                    rendered,
                    "Audit EEPROM read-only PLTS Samoto ILS6300 selesai. "
                    "Pack#1=OK(CellOV=3650mV,CellUV=2800mV), Pack#2=OK(CellOV=3650mV,CellUV=2800mV)",
                )

        # Ensure zero Shoto EEPROM write commands anywhere in automations/scripts
        raw_combined = (
            self.AUTOMATIONS_PATH.read_text(encoding="utf-8")
            + "\n"
            + self.SCRIPTS_PATH.read_text(encoding="utf-8")
        )
        self.assertNotIn("0xAA", raw_combined.upper().replace("0XAA", "0xAA"))

    def test_automations_and_scripts_entities_and_attrs_match_firmware(self):
        dev_id = parse_ha_device_id()
        expected_entities = parse_discovery_entities(dev_id, max_packs=4)
        eeprom_keys = parse_eeprom_json_keys()

        automations = load_yaml_any(self.AUTOMATIONS_PATH)
        scripts = load_yaml_file(self.SCRIPTS_PATH)

        strings: list[str] = []
        collect_yaml_strings(automations, strings)
        collect_yaml_strings(scripts, strings)
        joined = "\n".join(strings)

        entity_re = re.compile(r"\b[a-z_]+\.shoto_[a-z0-9_]+\b")
        referenced_entities = set(entity_re.findall(joined))
        self.assertGreater(len(referenced_entities), 0)

        unknown = sorted(referenced_entities - expected_entities)
        self.assertEqual(
            unknown,
            [],
            f"Automations/scripts reference unknown/orphan entities not in ha_mqtt_codec.hpp: {unknown}",
        )

        # Verify all state_attr(..., '<key>') calls reference valid build_pack_eeprom_json() keys
        attr_re = re.compile(
            r"state_attr\(\s*[^,]+,\s*['\"]([a-z0-9_]+)['\"]\s*\)"
        )
        attr_keys = attr_re.findall(joined)
        self.assertGreater(
            len(attr_keys),
            0,
            "Scripts must reference state_attr(..., '<key>') for pack*_eep_valid",
        )
        for attr_key in attr_keys:
            self.assertIn(
                attr_key,
                eeprom_keys,
                f"Scripts reference unknown EEPROM attribute '{attr_key}'",
            )


if __name__ == "__main__":
    unittest.main()
