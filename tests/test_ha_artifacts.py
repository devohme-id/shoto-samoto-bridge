import json
import re
import subprocess
import unittest
from pathlib import Path

try:
    import yaml as _pyyaml  # type: ignore
except ImportError:
    _pyyaml = None


REPO_ROOT = Path(__file__).resolve().parents[1]
HA_DIR = REPO_ROOT / "homeassistant"


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
        ruby_script = (
            "require 'yaml'; require 'json'; "
            "data = YAML.load(ARGF.read); "
            "puts JSON.generate(data)"
        )
        proc = subprocess.run(
            ["ruby", "-e", ruby_script, str(path)],
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


if __name__ == "__main__":
    unittest.main()
