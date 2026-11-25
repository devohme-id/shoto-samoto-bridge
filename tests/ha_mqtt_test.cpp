#include "../firmware/shoto_samoto_bridge/ha_mqtt_codec.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

static bool validate_json_object(const char* s, size_t len) {
    if (!s || len < 2 || len > HA_MQTT_MAX_PAYLOAD) return false;
    if (std::strlen(s) != len) return false;
    if (s[0] != '{' || s[len - 1] != '}') return false;

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    std::vector<char> stack;
    for (size_t i = 0; i < len; ++i) {
        unsigned char uc = static_cast<unsigned char>(s[i]);
        if (uc < 0x20u) return false; // no raw control chars
        char c = static_cast<char>(uc);
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            stack.push_back(c);
            depth++;
        } else if (c == '}' || c == ']') {
            if (stack.empty()) return false;
            char top = stack.back();
            if ((c == '}' && top != '{') || (c == ']' && top != '[')) return false;
            stack.pop_back();
            depth--;
            if (depth == 0 && i + 1 != len) return false;
        }
    }
    if (in_str || esc || !stack.empty() || depth != 0) return false;

    // Ensure no unquoted nan/inf literals slipped in
    std::string str(s, len);
    if (str.find(":nan") != std::string::npos ||
        str.find(":NaN") != std::string::npos ||
        str.find(":inf") != std::string::npos ||
        str.find(":-inf") != std::string::npos) {
        return false;
    }
    return true;
}

static std::string extract_json_str_field(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\":\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

int main() {
    // 1. clamp_runtime_policy tests
    {
        RuntimePolicyOverride pol{};
        pol.cvl_v = 53.6f;
        pol.dvl_v = 46.0f;
        pol.ccl_max_a = 60.0f;
        pol.dcl_max_a = 100.0f;
        pol.max_delta_v = 1.0f;
        clamp_runtime_policy(pol);
        assert(std::fabs(pol.cvl_v - 53.6f) < 0.001f);
        assert(std::fabs(pol.dvl_v - 46.0f) < 0.001f);
        assert(std::fabs(pol.ccl_max_a - 60.0f) < 0.001f);
        assert(std::fabs(pol.dcl_max_a - 100.0f) < 0.001f);
        assert(std::fabs(pol.max_delta_v - 1.0f) < 0.001f);

        pol.cvl_v = 60.0f;
        pol.dvl_v = 50.0f;
        pol.ccl_max_a = 150.0f;
        pol.dcl_max_a = 200.0f;
        pol.max_delta_v = 3.0f;
        clamp_runtime_policy(pol);
        assert(std::fabs(pol.cvl_v - 54.4f) < 0.001f);
        assert(std::fabs(pol.dvl_v - 48.0f) < 0.001f);
        assert(std::fabs(pol.ccl_max_a - 90.0f) < 0.001f);
        assert(std::fabs(pol.dcl_max_a - 120.0f) < 0.001f);
        assert(std::fabs(pol.max_delta_v - 1.5f) < 0.001f);

        pol.cvl_v = 40.0f;
        pol.dvl_v = 30.0f;
        pol.ccl_max_a = 2.0f;
        pol.dcl_max_a = 5.0f;
        pol.max_delta_v = 0.1f;
        clamp_runtime_policy(pol);
        assert(std::fabs(pol.cvl_v - 51.0f) < 0.001f);
        assert(std::fabs(pol.dvl_v - 44.0f) < 0.001f);
        assert(std::fabs(pol.ccl_max_a - 10.0f) < 0.001f);
        assert(std::fabs(pol.dcl_max_a - 10.0f) < 0.001f);
        assert(std::fabs(pol.max_delta_v - 0.5f) < 0.001f);

        // NaN / Inf must sanitize back to safe defaults
        float qnan = std::numeric_limits<float>::quiet_NaN();
        float pinf = std::numeric_limits<float>::infinity();
        pol.cvl_v = qnan;
        pol.dvl_v = pinf;
        pol.ccl_max_a = -pinf;
        pol.dcl_max_a = qnan;
        pol.max_delta_v = qnan;
        clamp_runtime_policy(pol);
        assert(std::fabs(pol.cvl_v - 53.6f) < 0.001f);
        assert(std::fabs(pol.dvl_v - 46.0f) < 0.001f);
        assert(std::fabs(pol.ccl_max_a - 90.0f) < 0.001f);
        assert(std::fabs(pol.dcl_max_a - 120.0f) < 0.001f);
        assert(std::fabs(pol.max_delta_v - 1.0f) < 0.001f);
        assert(pol.cvl_v > pol.dvl_v);
    }

    // 2. parse_ha_command two-step safety gate & validation tests
    {
        HaActionCmd cmd{};

        // Safe diagnostic commands work when locked (maintenance_unlocked = false)
        assert(parse_ha_command("rescan", "PRESS", false, cmd));
        assert(cmd.type == HA_ACT_RESCAN);

        assert(parse_ha_command("audit_eeprom", "all", false, cmd));
        assert(cmd.type == HA_ACT_AUDIT_EEPROM && cmd.u8_val == 0);

        assert(parse_ha_command("audit_eeprom", "2", false, cmd));
        assert(cmd.type == HA_ACT_AUDIT_EEPROM && cmd.u8_val == 2);

        assert(parse_ha_command("reset_diag", "PRESS", false, cmd));
        assert(cmd.type == HA_ACT_RESET_DIAG);

        assert(parse_ha_command("republish_discovery", "PRESS", false, cmd));
        assert(cmd.type == HA_ACT_REPUBLISH_DISCOVERY);

        assert(parse_ha_command("cell_log", "ON", false, cmd));
        assert(cmd.type == HA_ACT_SET_CELL_LOG && cmd.bool_val == true);

        assert(parse_ha_command("cell_log", "OFF", false, cmd));
        assert(cmd.type == HA_ACT_SET_CELL_LOG && cmd.bool_val == false);

        assert(parse_ha_command("maint_unlock", "ON", false, cmd));
        assert(cmd.type == HA_ACT_SET_MAINT_UNLOCK && cmd.bool_val == true);

        // Critical commands MUST be denied when locked (maintenance_unlocked = false)
        const char* locked_cmds[][2] = {
            {"restart", "PRESS"},
            {"bridge_mode", "ACTIVE"},
            {"dyn_discovery", "OFF"},
            {"policy_cvl", "53.8"},
            {"policy_dvl", "45.5"},
            {"policy_ccl", "60"},
            {"policy_dcl", "100"},
            {"policy_max_dv", "1.2"},
            {"policy_reset", "PRESS"},
        };
        for (size_t i = 0; i < sizeof(locked_cmds) / sizeof(locked_cmds[0]); ++i) {
            assert(!parse_ha_command(locked_cmds[i][0], locked_cmds[i][1], false, cmd));
            assert(cmd.type == HA_ACT_DENIED_LOCKED);
        }

        // Critical commands MUST succeed & clamp when unlocked (maintenance_unlocked = true)
        assert(parse_ha_command("restart", "PRESS", true, cmd));
        assert(cmd.type == HA_ACT_RESTART);

        assert(parse_ha_command("bridge_mode", "ACTIVE", true, cmd));
        assert(cmd.type == HA_ACT_SET_BRIDGE_MODE && cmd.bool_val == true);

        assert(parse_ha_command("bridge_mode", "MONITOR_ONLY", true, cmd));
        assert(cmd.type == HA_ACT_SET_BRIDGE_MODE && cmd.bool_val == false);

        assert(parse_ha_command("dyn_discovery", "ON", true, cmd));
        assert(cmd.type == HA_ACT_SET_DYN_DISCOVERY && cmd.bool_val == true);

        assert(parse_ha_command("policy_cvl", "60.0", true, cmd));
        assert(cmd.type == HA_ACT_SET_CVL && std::fabs(cmd.float_val - 54.4f) < 0.001f);

        assert(parse_ha_command("policy_cvl", "40.0", true, cmd));
        assert(cmd.type == HA_ACT_SET_CVL && std::fabs(cmd.float_val - 51.0f) < 0.001f);

        assert(parse_ha_command("policy_dvl", "30.0", true, cmd));
        assert(cmd.type == HA_ACT_SET_DVL && std::fabs(cmd.float_val - 44.0f) < 0.001f);

        assert(parse_ha_command("policy_ccl", "150.0", true, cmd));
        assert(cmd.type == HA_ACT_SET_CCL && std::fabs(cmd.float_val - 90.0f) < 0.001f);

        assert(parse_ha_command("policy_dcl", "200.0", true, cmd));
        assert(cmd.type == HA_ACT_SET_DCL && std::fabs(cmd.float_val - 120.0f) < 0.001f);

        assert(parse_ha_command("policy_max_dv", "0.2", true, cmd));
        assert(cmd.type == HA_ACT_SET_MAX_DV && std::fabs(cmd.float_val - 0.5f) < 0.001f);

        assert(parse_ha_command("policy_reset", "PRESS", true, cmd));
        assert(cmd.type == HA_ACT_RESET_POLICY);

        // Garbage / NaN / Inf / unknown commands -> HA_ACT_INVALID
        const char* bad_payloads[] = {"NaN", "nan", "Inf", "-inf", "abc", "53.6V", "", nullptr};
        for (size_t i = 0; i < sizeof(bad_payloads) / sizeof(bad_payloads[0]); ++i) {
            assert(!parse_ha_command("policy_cvl", bad_payloads[i], true, cmd));
            assert(cmd.type == HA_ACT_INVALID);
        }
        assert(!parse_ha_command("audit_eeprom", "9", true, cmd));
        assert(cmd.type == HA_ACT_INVALID);
        assert(!parse_ha_command("bridge_mode", "GARBAGE", true, cmd));
        assert(cmd.type == HA_ACT_INVALID);
        assert(!parse_ha_command("maint_unlock", "MAYBE", true, cmd));
        assert(cmd.type == HA_ACT_INVALID);
        assert(!parse_ha_command("unknown_topic", "PRESS", true, cmd));
        assert(cmd.type == HA_ACT_INVALID);
        assert(!parse_ha_command(nullptr, "PRESS", true, cmd));
        assert(cmd.type == HA_ACT_INVALID);
    }

    // 3. JSON builders & buffer safety (<= 1400 bytes)
    {
        char buf[HA_MQTT_MAX_PAYLOAD + 64];

        // 3a. Bank state JSON
        BankExt bank{};
        bank.core.valid = true;
        bank.core.charge = true;
        bank.core.discharge = true;
        bank.core.soc = 84.5f;
        bank.core.capacity_ah = 200.0f;
        bank.core.bus_a = -22.5f;
        bank.core.ccl = 60.0f;
        bank.core.dcl = 100.0f;
        bank.core.cvl = 53.6f;
        bank.core.dvl = 46.0f;
        bank.has_telemetry = true;
        bank.pack_count = 2;
        bank.bus_v = 52.84f;
        bank.raw_soc = 84.5f;
        bank.raw_bus_a = -22.5f;
        bank.rem_cap_ah = 169.0f;
        bank.soh = 99.5f;
        bank.min_temp_c = 24.5f;
        bank.max_temp_c = 28.0f;
        bank.avg_temp_c = 26.2f;
        bank.min_cell_mv = 3320;
        bank.max_cell_mv = 3331;
        bank.min_cell_pack = 1;
        bank.min_cell_idx = 4;
        bank.max_cell_pack = 2;
        bank.max_cell_idx = 12;
        bank.cycles = 42;
        bank.warn_flags = 0x0001;
        bank.prot_flags = 0x0000;
        bank.fault_flags = 0x00;

        RuntimePolicyOverride pol{};
        pol.cvl_v = 53.6f;
        pol.dvl_v = 46.0f;
        pol.ccl_max_a = 90.0f;
        pol.dcl_max_a = 120.0f;
        pol.max_delta_v = 1.0f;
        pol.modified = false;

        size_t n = build_bank_state_json(bank, INHIBIT_NONE, true, true, true, 240u, pol, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));
        std::string bjson(buf, n);
        assert(bjson.find("\"soc\":84.5") != std::string::npos);
        assert(bjson.find("\"bridge_mode\":\"ACTIVE\"") != std::string::npos);
        assert(bjson.find("\"inh\":\"NONE\"") != std::string::npos);

        // NaN resilience in bank JSON
        bank.bus_v = std::numeric_limits<float>::quiet_NaN();
        n = build_bank_state_json(bank, INHIBIT_DELTA_V, false, false, false, 0u, pol, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));

        // 3b. Diag state JSON with max-length TX frames + special characters
        HaDiagSnapshot diag{};
        diag.proto = PROTO_TBD_BINARY;
        diag.cell_log = true;
        diag.bat_poll_ok = 999999u;
        diag.bat_poll_fail = 12u;
        diag.inv_req_count = 500000u;
        diag.inv_resp_count = 499999u;
        diag.inv_raw_bytes = 9999999u;
        diag.inhibit_count = 3u;
        diag.dv_retry_ok = 2u;
        diag.dv_retry_fail = 1u;
        diag.last_inhibit_reason = INHIBIT_DELTA_V;
        diag.last_inhibit_age_ms = 12345u;
        diag.last_inhibit_dv = 1.15f;
        diag.peak_dv = 1.15f;
        diag.peak_age_ms = 2300u;
        diag.inv_60 = 100u;
        diag.inv_61 = 100000u;
        diag.inv_62 = 100000u;
        diag.inv_63 = 100000u;
        diag.inv_other = 50u;
        diag.inv_mtx_miss = 0u;
        std::memset(diag.tx61, 'A', sizeof(diag.tx61) - 1);
        diag.tx61[0] = '"';
        diag.tx61[1] = '\\';
        diag.tx61[sizeof(diag.tx61) - 1] = '\0';
        std::memset(diag.tx62, 'B', sizeof(diag.tx62) - 1);
        diag.tx62[sizeof(diag.tx62) - 1] = '\0';
        std::memset(diag.tx63, 'C', sizeof(diag.tx63) - 1);
        diag.tx63[sizeof(diag.tx63) - 1] = '\0';
        diag.tx61_age_ms = 500u;
        diag.tx62_age_ms = 500u;
        diag.tx63_age_ms = 500u;

        HaSystemHealth sys{};
        sys.wifi_rssi_dbm = -58;
        sys.free_heap_bytes = 185432u;
        sys.min_free_heap_bytes = 142000u;
        sys.uptime_s = 864000u;
        sys.mqtt_reconnects = 2u;
        std::snprintf(sys.ip_addr, sizeof(sys.ip_addr), "192.168.100.254");

        n = build_diag_state_json(diag, sys, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));
        std::string djson(buf, n);
        assert(djson.find("\"inv_mode\":\"PYL_FULL\"") != std::string::npos);
        assert(djson.find("\"proto\":\"TBD_BINARY\"") != std::string::npos);

        // 3c. Pack state & cells JSON (all 15 cells, 5 temps)
        PackState ps{};
        ps.detected_proto = PROTO_TBD_BINARY;
        ps.addr = 1;
        ps.core.bus_v = 52.84f;
        ps.core.bus_a = -11.25f;
        ps.core.soc = 84.5f;
        ps.core.capacity_ah = 100.0f;
        ps.core.allow_charge = true;
        ps.core.allow_discharge = true;
        ps.core.critical = false;
        ps.ext.soh = 99.5f;
        ps.ext.rem_cap_ah = 84.5f;
        ps.ext.cycles = 42u;
        ps.ext.cell_count = 15u;
        for (int c = 0; c < 15; ++c) ps.ext.cell_mv[c] = static_cast<uint16_t>(3320 + c);
        ps.ext.min_cell_mv = 3320;
        ps.ext.max_cell_mv = 3334;
        ps.ext.min_cell_idx = 1;
        ps.ext.max_cell_idx = 15;
        ps.ext.temp_count = 5;
        ps.ext.temps_c[0] = -5.5f;
        ps.ext.temps_c[1] = 25.0f;
        ps.ext.temps_c[2] = 26.5f;
        ps.ext.temps_c[3] = 27.0f;
        ps.ext.temps_c[4] = 28.5f;
        ps.ext.min_temp_c = -5.5f;
        ps.ext.max_temp_c = 28.5f;
        ps.ext.avg_temp_c = 20.3f;

        n = build_pack_state_json(1, true, ps, 250u, 0u, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));
        std::string pjson(buf, n);
        assert(pjson.find("\"t1\":-5.5") != std::string::npos);
        assert(pjson.find("\"t5\":28.5") != std::string::npos);

        n = build_pack_cells_json(ps, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));
        std::string cjson(buf, n);
        assert(cjson.find("\"c01\":3320") != std::string::npos);
        assert(cjson.find("\"c15\":3334") != std::string::npos);
        assert(cjson.find("\"dv_mv\":14") != std::string::npos);

        // 3d. Pack EEPROM JSON with ALL 13 prot + ALL 43 alarm params at worst-case max lengths
        PackEepromSnapshot eep{};
        eep.addr = 4;
        eep.valid = true;
        eep.audited_ms = 4294967295u;
        eep.prot.valid = true;
        eep.prot.audited_ms = 4294967294u;
        eep.prot.cell_ov_prot_mv = 10001;
        eep.prot.cell_ov_rec_mv = 10002;
        eep.prot.cell_uv_prot_mv = 10003;
        eep.prot.cell_uv_rec_mv = 10004;
        eep.prot.pack_ov_prot_cv = 10005;
        eep.prot.pack_ov_rec_cv = 10006;
        eep.prot.pack_uv_prot_cv = 10007;
        eep.prot.pack_uv_rec_cv = 10008;
        eep.prot.const_chg_cv = 10009;
        eep.prot.const_chg_ci = 10010;
        eep.prot.cell_sleep_mv = 10011;
        eep.prot.uv_off_interval = 10012;
        eep.prot.sleep_time_min = 10013;

        eep.alarm.valid = true;
        eep.alarm.tag_count = 43;
        eep.alarm.cell_ov_alarm_mv = 20001;
        eep.alarm.cell_ov_delay_ms = 20002;
        eep.alarm.cell_ov_alarm_rec_mv = 20003;
        eep.alarm.cell_uv_alarm_mv = 20004;
        eep.alarm.cell_uv_delay_ms = 20005;
        eep.alarm.cell_uv_alarm_rec_mv = 20006;
        eep.alarm.pack_ov_alarm_cv = 20007;
        eep.alarm.pack_ov_delay_ms = 20008;
        eep.alarm.pack_ov_alarm_rec_cv = 20009;
        eep.alarm.pack_uv_alarm_cv = 20010;
        eep.alarm.pack_uv_delay_ms = 20011;
        eep.alarm.pack_uv_alarm_rec_cv = 20012;
        eep.alarm.chg_oc_ca = 20013;
        eep.alarm.chg_oc_delay_ms = 20014;
        eep.alarm.chg_oc_rec_ca = 20015;
        eep.alarm.dsg_oc_ca = 20016;
        eep.alarm.dsg_oc_delay_ms = 20017;
        eep.alarm.dsg_oc_rec_ca = 20018;
        eep.alarm.chg_ot_c = -10019;
        eep.alarm.chg_ot_delay_ms = 20020;
        eep.alarm.chg_ot_rec_c = -10021;
        eep.alarm.chg_ut_c = -10022;
        eep.alarm.chg_ut_delay_ms = 20023;
        eep.alarm.chg_ut_rec_c = -10024;
        eep.alarm.dsg_ot_c = -10025;
        eep.alarm.dsg_ot_delay_ms = 20026;
        eep.alarm.dsg_ot_rec_c = -10027;
        eep.alarm.dsg_ut_c = -10028;
        eep.alarm.dsg_ut_delay_ms = 20029;
        eep.alarm.dsg_ut_rec_c = -10030;
        eep.alarm.mos_ot_c = -10031;
        eep.alarm.mos_ot_delay_ms = 20032;
        eep.alarm.mos_ot_rec_c = -10033;
        eep.alarm.low_soc_alarm_pct = 20034;
        eep.alarm.low_soc_alarm_rec_pct = 20035;
        eep.alarm.cell_diff_alarm_mv = 20036;
        eep.alarm.cell_diff_alarm_rec_mv = 20037;
        eep.alarm.env_ot_c = -10038;
        eep.alarm.env_ot_delay_ms = 20039;
        eep.alarm.env_ot_rec_c = -10040;
        eep.alarm.env_ut_c = -10041;
        eep.alarm.env_ut_delay_ms = 20042;
        eep.alarm.env_ut_rec_c = -10043;

        n = build_pack_eeprom_json(eep, buf, sizeof(buf));
        assert(n > 0 && n <= HA_MQTT_MAX_PAYLOAD);
        assert(validate_json_object(buf, n));
        std::string ejson(buf, n);

        // Verify every single one of the 13 prot values and 43 alarm values is present
        for (int v = 10001; v <= 10013; ++v) {
            char needle[16];
            std::snprintf(needle, sizeof(needle), ":%d", v);
            assert(ejson.find(needle) != std::string::npos);
        }
        for (int idx = 1; idx <= 43; ++idx) {
            char needle[16];
            if ((idx >= 19 && idx <= 33 && (idx % 3 != 2)) ||
                (idx >= 38 && idx <= 43 && (idx % 3 != 0))) {
                std::snprintf(needle, sizeof(needle), ":-%d", 10000 + idx);
            } else {
                std::snprintf(needle, sizeof(needle), ":%d", 20000 + idx);
            }
            assert(ejson.find(needle) != std::string::npos);
        }

        // Small buffer guard: must fail cleanly (return 0) and not overflow
        char tiny[16];
        assert(build_pack_eeprom_json(eep, tiny, sizeof(tiny)) == 0);
        assert(build_bank_state_json(bank, INHIBIT_NONE, true, true, false, 0u, pol, tiny, sizeof(tiny)) == 0);
    }

    // 4. Discovery payload uniqueness & multi-site isolation tests
    {
        const uint8_t max_packs = 4;
        size_t total = ha_discovery_entity_count(max_packs);
        assert(total > 100);

        std::set<std::string> seen_topics;
        std::set<std::string> seen_unique_ids;
        std::set<std::string> seen_entity_ids;
        bool saw_p1_cell01 = false, saw_p4_cell15 = false;
        bool saw_p1_temp5 = false, saw_p4_eep_valid = false;
        bool saw_p2_dv_attrs = false;

        char topic[256];
        char payload[HA_MQTT_MAX_PAYLOAD + 64];

        for (size_t i = 0; i < total; ++i) {
            bool ok = build_ha_discovery_payload(
                i, max_packs,
                "shoto_ils6300", "PLTS Samoto ILS6300", "shoto_bridge/ils6300", "3.1.0-ha",
                topic, sizeof(topic), payload, sizeof(payload));
            assert(ok);
            size_t plen = std::strlen(payload);
            assert(plen > 0 && plen <= HA_MQTT_MAX_PAYLOAD);
            assert(validate_json_object(payload, plen));

            std::string tstr(topic);
            std::string pstr(payload, plen);
            assert(tstr.rfind("homeassistant/", 0) == 0);
            assert(tstr.find("/shoto_ils6300/") != std::string::npos);
            assert(tstr.rfind("/config") == tstr.size() - 7);

            std::string uid = extract_json_str_field(pstr, "unique_id");
            std::string deid = extract_json_str_field(pstr, "default_entity_id");
            assert(!uid.empty());
            assert(!deid.empty());
            assert(uid.rfind("shoto_ils6300_", 0) == 0);
            assert(seen_topics.insert(tstr).second);
            assert(seen_unique_ids.insert(uid).second);
            assert(seen_entity_ids.insert(deid).second);

            assert(pstr.find("\"availability_topic\":\"shoto_bridge/ils6300/status\"") != std::string::npos);
            assert(pstr.find("\"payload_available\":\"online\"") != std::string::npos);
            assert(pstr.find("\"payload_not_available\":\"offline\"") != std::string::npos);
            assert(pstr.find("\"identifiers\":[\"shoto_ils6300\"]") != std::string::npos);
            assert(pstr.find("\"name\":\"PLTS Samoto ILS6300\"") != std::string::npos);
            assert(pstr.find("\"sw_version\":\"3.1.0-ha\"") != std::string::npos);

            if (uid == "shoto_ils6300_pack1_cell_01") saw_p1_cell01 = true;
            if (uid == "shoto_ils6300_pack4_cell_15") saw_p4_cell15 = true;
            if (uid == "shoto_ils6300_pack1_temp_5") saw_p1_temp5 = true;
            if (uid == "shoto_ils6300_pack4_eep_valid") {
                saw_p4_eep_valid = true;
                assert(pstr.find("\"json_attributes_topic\":\"shoto_bridge/ils6300/pack4/eeprom\"") != std::string::npos);
            }
            if (uid == "shoto_ils6300_pack2_cell_dv_mv") {
                saw_p2_dv_attrs = true;
                assert(pstr.find("\"json_attributes_topic\":\"shoto_bridge/ils6300/pack2/cells\"") != std::string::npos);
            }
        }
        assert(saw_p1_cell01 && saw_p4_cell15 && saw_p1_temp5 && saw_p4_eep_valid && saw_p2_dv_attrs);
        assert(!build_ha_discovery_payload(
            total, max_packs,
            "shoto_ils6300", "PLTS Samoto ILS6300", "shoto_bridge/ils6300", "3.1.0-ha",
            topic, sizeof(topic), payload, sizeof(payload)));

        // Multi-site isolation: second site produces zero colliding topics or unique_ids
        assert(build_ha_discovery_payload(
            0, max_packs,
            "shoto_ihf6200", "PLTS Samoto IHF6200", "shoto_bridge/ihf6200", "3.1.0-ha",
            topic, sizeof(topic), payload, sizeof(payload)));
        std::string p2(payload);
        assert(seen_topics.find(topic) == seen_topics.end());
        assert(seen_unique_ids.find(extract_json_str_field(p2, "unique_id")) == seen_unique_ids.end());
        assert(p2.find("\"availability_topic\":\"shoto_bridge/ihf6200/status\"") != std::string::npos);
    }

    std::cout << "ALL 4 C++17 HA MQTT codec & safety suites PASS\n";
    return 0;
}
