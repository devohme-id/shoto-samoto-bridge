import unittest

from test_ha_artifacts import HA_DIR, REPO_ROOT, load_yaml_file


class TestSamotoEnergy(unittest.TestCase):
    def test_dashboard_entities_and_sources(self):
        all_ids = set()
        for site in ("ihf6200", "ils6300"):
            with self.subTest(site=site):
                prefix = f"samoto_energy_{site}"
                topics = f"samoto_energy/{site}"
                package = load_yaml_file(HA_DIR / "packages" / f"{prefix}.yaml")
                dashboard = load_yaml_file(HA_DIR / "dashboards" / f"{prefix}_dashboard.yaml")
                sensors = {item["default_entity_id"]: item for item in package["mqtt"]["sensor"]}
                self.assertEqual(len(sensors), len(package["mqtt"]["sensor"]))
                supplied = set(sensors) | {f"sensor.{key}" for key in package["utility_meter"]}
                self.assertFalse(all_ids & supplied)
                all_ids.update(supplied)

                def inspect(value):
                    if isinstance(value, dict):
                        if "entity" in value:
                            self.assertIn(value["entity"], supplied)
                        for child in value.values():
                            inspect(child)
                    elif isinstance(value, list):
                        for child in value:
                            inspect(child)

                inspect(dashboard)
                for channel in ("grid", "load", "pv"):
                    source = f"sensor.{prefix}_{channel}_energy"
                    self.assertEqual(sensors[source]["state_topic"], f"{topics}/{channel}")
                    self.assertEqual(sensors[source]["device_class"], "energy")
                    self.assertEqual(sensors[source]["state_class"], "total_increasing")
                    self.assertEqual(sensors[source]["unit_of_measurement"], "kWh")
                    for cycle in ("daily", "monthly", "yearly"):
                        meter = package["utility_meter"][f"{prefix}_{channel}_{cycle}"]
                        self.assertEqual(meter["source"], source)
                        self.assertEqual(meter["cycle"], cycle)
                        self.assertFalse(meter["periodically_resetting"])

                battery = sensors[f"sensor.{prefix}_battery_power"]
                self.assertEqual(battery["state_topic"], f"shoto_bridge/{site}/bank")
                self.assertEqual(battery["expire_after"], 15)
                self.assertIn("value_json.valid is sameas true", battery["availability"][1]["value_template"])
                pv_status = sensors[f"sensor.{prefix}_pv_status"]
                self.assertEqual(pv_status["state_topic"], f"{topics}/pv")
                self.assertEqual(pv_status["expire_after"], 20)

    def test_profile_topics_and_default_pv_gate(self):
        firmware = REPO_ROOT / "firmware" / "samoto_energy_monitor"
        settings = (firmware / "energy_local.example.h").read_text()
        self.assertIn("#define ENERGY_ENABLE_PV 0", settings)
        for site in ("ihf6200", "ils6300"):
            profile = (firmware / "profiles" / f"{site}.h").read_text()
            self.assertIn(f'"samoto_energy/{site}"', profile)
            self.assertIn(f'"samoto-energy-{site}"', profile)
