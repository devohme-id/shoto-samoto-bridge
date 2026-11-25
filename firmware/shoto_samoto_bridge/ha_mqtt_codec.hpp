#pragma once
/**
 * Portable C++17 (and C++11-compatible) Home Assistant MQTT codec:
 *  - Deterministic bounded snprintf JSON encoders (no Arduino String / heap alloc)
 *  - Hard 15S LFP policy clamps + NaN/Inf sanitization
 *  - Two-step Maintenance Unlock command validator
 *  - Data-driven MQTT Auto-Discovery builder with multi-site topic isolation
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "bridge_protocols.hpp"

static constexpr size_t HA_MQTT_MAX_PAYLOAD = 1400u;

// Hard 15S LFP (SD-BA430) policy limits
static constexpr float HA_POLICY_CVL_MIN_V      = 51.0f;
static constexpr float HA_POLICY_CVL_MAX_V      = 54.4f;
static constexpr float HA_POLICY_CVL_DEFAULT_V  = 53.6f;

static constexpr float HA_POLICY_DVL_MIN_V      = 44.0f;
static constexpr float HA_POLICY_DVL_MAX_V      = 48.0f;
static constexpr float HA_POLICY_DVL_DEFAULT_V  = 46.0f;

static constexpr float HA_POLICY_CCL_MIN_A      = 10.0f;
static constexpr float HA_POLICY_CCL_MAX_A      = 90.0f;
static constexpr float HA_POLICY_CCL_DEFAULT_A  = 90.0f;

static constexpr float HA_POLICY_DCL_MIN_A      = 10.0f;
static constexpr float HA_POLICY_DCL_MAX_A      = 120.0f;
static constexpr float HA_POLICY_DCL_DEFAULT_A  = 120.0f;

static constexpr float HA_POLICY_MAX_DV_MIN_V     = 0.5f;
static constexpr float HA_POLICY_MAX_DV_MAX_V     = 1.5f;
static constexpr float HA_POLICY_MAX_DV_DEFAULT_V = 1.0f;

struct RuntimePolicyOverride {
    float cvl_v;
    float dvl_v;
    float ccl_max_a;
    float dcl_max_a;
    float max_delta_v;
    bool modified;
};

inline RuntimePolicyOverride default_runtime_policy() {
    RuntimePolicyOverride p{};
    p.cvl_v = HA_POLICY_CVL_DEFAULT_V;
    p.dvl_v = HA_POLICY_DVL_DEFAULT_V;
    p.ccl_max_a = HA_POLICY_CCL_DEFAULT_A;
    p.dcl_max_a = HA_POLICY_DCL_DEFAULT_A;
    p.max_delta_v = HA_POLICY_MAX_DV_DEFAULT_V;
    p.modified = false;
    return p;
}

enum HaActionType : uint8_t {
    HA_ACT_NONE = 0,
    HA_ACT_INVALID,
    HA_ACT_DENIED_LOCKED,
    HA_ACT_RESCAN,
    HA_ACT_AUDIT_EEPROM,
    HA_ACT_RESET_DIAG,
    HA_ACT_REPUBLISH_DISCOVERY,
    HA_ACT_RESTART,
    HA_ACT_SET_BRIDGE_MODE,
    HA_ACT_SET_DYN_DISCOVERY,
    HA_ACT_SET_CELL_LOG,
    HA_ACT_SET_MAINT_UNLOCK,
    HA_ACT_SET_CVL,
    HA_ACT_SET_DVL,
    HA_ACT_SET_CCL,
    HA_ACT_SET_DCL,
    HA_ACT_SET_MAX_DV,
    HA_ACT_RESET_POLICY
};

struct HaActionCmd {
    HaActionType type = HA_ACT_NONE;
    bool bool_val = false;
    uint8_t u8_val = 0;
    float float_val = 0.0f;
};

struct HaDiagSnapshot {
    ShotoProtoKind proto = PROTO_AUTO;
    bool cell_log = false;
    uint32_t bat_poll_ok = 0;
    uint32_t bat_poll_fail = 0;
    uint32_t inv_req_count = 0;
    uint32_t inv_resp_count = 0;
    uint32_t inv_raw_bytes = 0;
    uint32_t inhibit_count = 0;
    uint32_t dv_retry_ok = 0;
    uint32_t dv_retry_fail = 0;
    InhibitReason last_inhibit_reason = INHIBIT_NONE;
    uint32_t last_inhibit_age_ms = 0;
    float last_inhibit_dv = 0.0f;
    float peak_dv = 0.0f;
    uint32_t peak_age_ms = 0;
    uint32_t inv_60 = 0;
    uint32_t inv_61 = 0;
    uint32_t inv_62 = 0;
    uint32_t inv_63 = 0;
    uint32_t inv_other = 0;
    uint32_t inv_mtx_miss = 0;
    char tx61[144] = "";
    char tx62[40] = "";
    char tx63[48] = "";
    uint32_t tx61_age_ms = 0;
    uint32_t tx62_age_ms = 0;
    uint32_t tx63_age_ms = 0;
};

struct HaSystemHealth {
    int32_t wifi_rssi_dbm = 0;
    uint32_t free_heap_bytes = 0;
    uint32_t min_free_heap_bytes = 0;
    uint32_t uptime_s = 0;
    uint32_t mqtt_reconnects = 0;
    char ip_addr[20] = "";
};

inline const char* ha_proto_name(ShotoProtoKind p) {
    switch (p) {
        case PROTO_MODBUS_V12:   return "SHOTO_MODBUS_V12";
        case PROTO_TBD_BINARY:   return "TBD_BINARY";
        case PROTO_TBD_1363:     return "TBD_1363";
        case PROTO_SMARTLI_DCDC: return "SMARTLI_DCDC";
        default:                 return "AUTO";
    }
}

namespace ha_detail {

inline float clamp_finite(float v, float lo, float hi, float def_val) {
    if (!std::isfinite(v)) return def_val;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float safe_json_float(float v) {
    if (!std::isfinite(v)) return 0.0f;
    return (std::fabs(v) < 1e-6f) ? 0.0f : v;
}

inline void trim_span(const char* s, const char*& start, size_t& len) {
    if (!s) {
        start = "";
        len = 0u;
        return;
    }
    size_t n = std::strlen(s);
    size_t a = 0u;
    while (a < n && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (n > a && std::isspace(static_cast<unsigned char>(s[n - 1u]))) --n;
    start = s + a;
    len = n - a;
}

inline bool eq_nocase(const char* s, size_t len, const char* lit) {
    size_t l = std::strlen(lit);
    if (len != l) return false;
    for (size_t i = 0u; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(lit[i]))) {
            return false;
        }
    }
    return true;
}

inline bool parse_bool_payload(const char* s, size_t len, bool& out_b) {
    if (eq_nocase(s, len, "ON") || eq_nocase(s, len, "1") || eq_nocase(s, len, "true")) {
        out_b = true;
        return true;
    }
    if (eq_nocase(s, len, "OFF") || eq_nocase(s, len, "0") || eq_nocase(s, len, "false")) {
        out_b = false;
        return true;
    }
    return false;
}

inline bool parse_button_payload(const char* s, size_t len) {
    return len == 0u ||
           eq_nocase(s, len, "PRESS") ||
           eq_nocase(s, len, "1") ||
           eq_nocase(s, len, "ON") ||
           eq_nocase(s, len, "true");
}

inline bool parse_finite_float(const char* s, size_t len, float& out_f) {
    if (len == 0u || len >= 32u) return false;
    char tmp[32];
    std::memcpy(tmp, s, len);
    tmp[len] = '\0';
    char* end = nullptr;
    float v = std::strtof(tmp, &end);
    if (end != tmp + len || !std::isfinite(v)) return false;
    out_f = v;
    return true;
}

struct JsonWriter {
    char* buf;
    size_t cap;
    size_t pos;
    bool first;
    bool ok;

    JsonWriter(char* b, size_t c)
        : buf(b), cap(c), pos(0u), first(true), ok(b != nullptr && c >= 2u) {
        if (ok) {
            buf[0] = '{';
            pos = 1u;
            buf[1] = '\0';
        } else if (b && c > 0u) {
            b[0] = '\0';
        }
    }

    void append_raw(const char* s, size_t n) {
        if (!ok) return;
        if (pos + n >= cap) {
            ok = false;
            return;
        }
        std::memcpy(buf + pos, s, n);
        pos += n;
        buf[pos] = '\0';
    }

    void append_cstr(const char* s) {
        if (s) append_raw(s, std::strlen(s));
    }

    void append_escaped(const char* s) {
        append_raw("\"", 1u);
        if (s) {
            for (size_t i = 0u; s[i] != '\0' && ok; ++i) {
                unsigned char uc = static_cast<unsigned char>(s[i]);
                if (uc == '"') {
                    append_raw("\\\"", 2u);
                } else if (uc == '\\') {
                    append_raw("\\\\", 2u);
                } else if (uc == '\n') {
                    append_raw("\\n", 2u);
                } else if (uc == '\r') {
                    append_raw("\\r", 2u);
                } else if (uc == '\t') {
                    append_raw("\\t", 2u);
                } else if (uc < 0x20u) {
                    char esc[7];
                    std::snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned>(uc));
                    append_raw(esc, 6u);
                } else {
                    char ch = static_cast<char>(uc);
                    append_raw(&ch, 1u);
                }
            }
        }
        append_raw("\"", 1u);
    }

    void begin_key(const char* key) {
        if (!ok) return;
        if (!first) append_raw(",", 1u);
        first = false;
        append_escaped(key);
        append_raw(":", 1u);
    }

    void kv_bool(const char* key, bool v) {
        begin_key(key);
        append_cstr(v ? "true" : "false");
    }

    void kv_u32(const char* key, uint32_t v) {
        begin_key(key);
        char num[16];
        int w = std::snprintf(num, sizeof(num), "%lu", static_cast<unsigned long>(v));
        if (w > 0) append_raw(num, static_cast<size_t>(w));
    }

    void kv_i32(const char* key, int32_t v) {
        begin_key(key);
        char num[16];
        int w = std::snprintf(num, sizeof(num), "%ld", static_cast<long>(v));
        if (w > 0) append_raw(num, static_cast<size_t>(w));
    }

    void kv_f1(const char* key, float v) {
        begin_key(key);
        char num[24];
        int w = std::snprintf(num, sizeof(num), "%.1f", static_cast<double>(safe_json_float(v)));
        if (w > 0 && static_cast<size_t>(w) < sizeof(num)) append_raw(num, static_cast<size_t>(w));
        else ok = false;
    }

    void kv_f2(const char* key, float v) {
        begin_key(key);
        char num[24];
        int w = std::snprintf(num, sizeof(num), "%.2f", static_cast<double>(safe_json_float(v)));
        if (w > 0 && static_cast<size_t>(w) < sizeof(num)) append_raw(num, static_cast<size_t>(w));
        else ok = false;
    }

    void kv_str(const char* key, const char* s) {
        begin_key(key);
        append_escaped(s);
    }

    void kv_raw(const char* key, const char* raw_json) {
        begin_key(key);
        append_cstr(raw_json);
    }

    size_t finish(size_t max_bytes = HA_MQTT_MAX_PAYLOAD) {
        append_raw("}", 1u);
        if (!ok || pos > max_bytes) {
            if (buf && cap > 0u) buf[0] = '\0';
            return 0u;
        }
        return pos;
    }
};

} // namespace ha_detail

inline void clamp_runtime_policy(RuntimePolicyOverride& pol) {
    pol.cvl_v = ha_detail::clamp_finite(pol.cvl_v, HA_POLICY_CVL_MIN_V, HA_POLICY_CVL_MAX_V, HA_POLICY_CVL_DEFAULT_V);
    pol.dvl_v = ha_detail::clamp_finite(pol.dvl_v, HA_POLICY_DVL_MIN_V, HA_POLICY_DVL_MAX_V, HA_POLICY_DVL_DEFAULT_V);
    pol.ccl_max_a = ha_detail::clamp_finite(pol.ccl_max_a, HA_POLICY_CCL_MIN_A, HA_POLICY_CCL_MAX_A, HA_POLICY_CCL_DEFAULT_A);
    pol.dcl_max_a = ha_detail::clamp_finite(pol.dcl_max_a, HA_POLICY_DCL_MIN_A, HA_POLICY_DCL_MAX_A, HA_POLICY_DCL_DEFAULT_A);
    pol.max_delta_v = ha_detail::clamp_finite(pol.max_delta_v, HA_POLICY_MAX_DV_MIN_V, HA_POLICY_MAX_DV_MAX_V, HA_POLICY_MAX_DV_DEFAULT_V);
    if (!(pol.cvl_v > pol.dvl_v)) {
        pol.cvl_v = HA_POLICY_CVL_DEFAULT_V;
        pol.dvl_v = HA_POLICY_DVL_DEFAULT_V;
    }
}

inline bool parse_ha_command(
    const char* suffix, const char* payload, bool maintenance_unlocked, HaActionCmd& out)
{
    out = HaActionCmd{};
    if (!suffix || !suffix[0] || !payload) {
        out.type = HA_ACT_INVALID;
        return false;
    }

    const char* ps = nullptr;
    size_t plen = 0u;
    ha_detail::trim_span(payload, ps, plen);

    // 1. Safe diagnostic commands (allowed regardless of maintenance_unlocked)
    if (std::strcmp(suffix, "rescan") == 0) {
        if (!ha_detail::parse_button_payload(ps, plen)) { out.type = HA_ACT_INVALID; return false; }
        out.type = HA_ACT_RESCAN;
        return true;
    }
    if (std::strcmp(suffix, "reset_diag") == 0) {
        if (!ha_detail::parse_button_payload(ps, plen)) { out.type = HA_ACT_INVALID; return false; }
        out.type = HA_ACT_RESET_DIAG;
        return true;
    }
    if (std::strcmp(suffix, "republish_discovery") == 0) {
        if (!ha_detail::parse_button_payload(ps, plen)) { out.type = HA_ACT_INVALID; return false; }
        out.type = HA_ACT_REPUBLISH_DISCOVERY;
        return true;
    }
    if (std::strcmp(suffix, "audit_eeprom") == 0) {
        if (plen == 0u || ha_detail::eq_nocase(ps, plen, "all") ||
            ha_detail::eq_nocase(ps, plen, "0") || ha_detail::eq_nocase(ps, plen, "PRESS")) {
            out.type = HA_ACT_AUDIT_EEPROM;
            out.u8_val = 0u;
            return true;
        }
        if (plen == 1u && ps[0] >= '1' && ps[0] <= '4') {
            out.type = HA_ACT_AUDIT_EEPROM;
            out.u8_val = static_cast<uint8_t>(ps[0] - '0');
            return true;
        }
        out.type = HA_ACT_INVALID;
        return false;
    }
    if (std::strcmp(suffix, "cell_log") == 0) {
        bool b = false;
        if (!ha_detail::parse_bool_payload(ps, plen, b)) { out.type = HA_ACT_INVALID; return false; }
        out.type = HA_ACT_SET_CELL_LOG;
        out.bool_val = b;
        return true;
    }
    if (std::strcmp(suffix, "maint_unlock") == 0) {
        bool b = false;
        if (!ha_detail::parse_bool_payload(ps, plen, b)) { out.type = HA_ACT_INVALID; return false; }
        out.type = HA_ACT_SET_MAINT_UNLOCK;
        out.bool_val = b;
        return true;
    }

    // 2. Critical commands (require maintenance_unlocked == true)
    HaActionCmd cand{};
    if (std::strcmp(suffix, "restart") == 0) {
        if (!ha_detail::parse_button_payload(ps, plen)) { out.type = HA_ACT_INVALID; return false; }
        cand.type = HA_ACT_RESTART;
    } else if (std::strcmp(suffix, "policy_reset") == 0) {
        if (!ha_detail::parse_button_payload(ps, plen)) { out.type = HA_ACT_INVALID; return false; }
        cand.type = HA_ACT_RESET_POLICY;
    } else if (std::strcmp(suffix, "dyn_discovery") == 0) {
        bool b = false;
        if (!ha_detail::parse_bool_payload(ps, plen, b)) { out.type = HA_ACT_INVALID; return false; }
        cand.type = HA_ACT_SET_DYN_DISCOVERY;
        cand.bool_val = b;
    } else if (std::strcmp(suffix, "bridge_mode") == 0) {
        if (ha_detail::eq_nocase(ps, plen, "ACTIVE") ||
            ha_detail::eq_nocase(ps, plen, "BRIDGE_ACTIVE") ||
            ha_detail::eq_nocase(ps, plen, "1")) {
            cand.type = HA_ACT_SET_BRIDGE_MODE;
            cand.bool_val = true;
            cand.u8_val = 1u;
        } else if (ha_detail::eq_nocase(ps, plen, "MONITOR_ONLY") ||
                   ha_detail::eq_nocase(ps, plen, "MONITOR") ||
                   ha_detail::eq_nocase(ps, plen, "0")) {
            cand.type = HA_ACT_SET_BRIDGE_MODE;
            cand.bool_val = false;
            cand.u8_val = 0u;
        } else {
            out.type = HA_ACT_INVALID;
            return false;
        }
    } else {
        struct NumRule {
            const char* name;
            HaActionType act;
            float lo;
            float hi;
            float def_val;
        };
        static const NumRule kNumRules[] = {
            {"policy_cvl",    HA_ACT_SET_CVL,    HA_POLICY_CVL_MIN_V,    HA_POLICY_CVL_MAX_V,    HA_POLICY_CVL_DEFAULT_V},
            {"policy_dvl",    HA_ACT_SET_DVL,    HA_POLICY_DVL_MIN_V,    HA_POLICY_DVL_MAX_V,    HA_POLICY_DVL_DEFAULT_V},
            {"policy_ccl",    HA_ACT_SET_CCL,    HA_POLICY_CCL_MIN_A,    HA_POLICY_CCL_MAX_A,    HA_POLICY_CCL_DEFAULT_A},
            {"policy_dcl",    HA_ACT_SET_DCL,    HA_POLICY_DCL_MIN_A,    HA_POLICY_DCL_MAX_A,    HA_POLICY_DCL_DEFAULT_A},
            {"policy_max_dv", HA_ACT_SET_MAX_DV, HA_POLICY_MAX_DV_MIN_V, HA_POLICY_MAX_DV_MAX_V, HA_POLICY_MAX_DV_DEFAULT_V},
        };
        bool matched = false;
        for (size_t i = 0u; i < sizeof(kNumRules) / sizeof(kNumRules[0]); ++i) {
            if (std::strcmp(suffix, kNumRules[i].name) == 0) {
                matched = true;
                float val = 0.0f;
                if (!ha_detail::parse_finite_float(ps, plen, val)) {
                    out.type = HA_ACT_INVALID;
                    return false;
                }
                cand.type = kNumRules[i].act;
                cand.float_val = ha_detail::clamp_finite(val, kNumRules[i].lo, kNumRules[i].hi, kNumRules[i].def_val);
                break;
            }
        }
        if (!matched) {
            out.type = HA_ACT_INVALID;
            return false;
        }
    }

    if (!maintenance_unlocked) {
        out.type = HA_ACT_DENIED_LOCKED;
        return false;
    }
    out = cand;
    return true;
}

inline size_t build_bank_state_json(
    const BankExt& bank, InhibitReason last_inh, bool dyn_disc, bool bridge_active,
    bool maint_unlock, uint32_t maint_rem_s, const RuntimePolicyOverride& pol,
    char* out, size_t cap)
{
    ha_detail::JsonWriter w(out, cap);
    float disp_i = bank.core.valid ? bank.core.bus_a : bank.raw_bus_a;
    float disp_soc = bank.core.valid ? bank.core.soc : bank.raw_soc;
    float power_w = ha_detail::safe_json_float(bank.bus_v) * ha_detail::safe_json_float(disp_i);
    uint16_t dv_mv = (bank.max_cell_mv >= bank.min_cell_mv && bank.min_cell_mv > 0u)
        ? static_cast<uint16_t>(bank.max_cell_mv - bank.min_cell_mv)
        : 0u;

    w.kv_bool("valid", bank.core.valid);
    w.kv_bool("has_telem", bank.has_telemetry);
    w.kv_u32("packs", bank.pack_count);
    w.kv_f2("v", bank.bus_v);
    w.kv_f2("i", disp_i);
    w.kv_f1("p", power_w);
    w.kv_f1("soc", disp_soc);
    w.kv_f1("raw_soc", bank.raw_soc);
    w.kv_f1("soh", bank.soh);
    w.kv_f1("cap_ah", bank.core.capacity_ah);
    w.kv_f1("rem_ah", bank.rem_cap_ah);
    w.kv_f2("cvl", bank.core.cvl);
    w.kv_f2("dvl", bank.core.dvl);
    w.kv_f1("ccl", bank.core.ccl);
    w.kv_f1("dcl", bank.core.dcl);
    w.kv_bool("chg", bank.core.charge);
    w.kv_bool("dis", bank.core.discharge);
    w.kv_f1("t_min", bank.min_temp_c);
    w.kv_f1("t_max", bank.max_temp_c);
    w.kv_f1("t_avg", bank.avg_temp_c);
    w.kv_u32("c_min_mv", bank.min_cell_mv);
    w.kv_u32("c_max_mv", bank.max_cell_mv);
    w.kv_u32("c_dv_mv", dv_mv);
    w.kv_u32("c_min_p", bank.min_cell_pack);
    w.kv_u32("c_min_i", bank.min_cell_idx);
    w.kv_u32("c_max_p", bank.max_cell_pack);
    w.kv_u32("c_max_i", bank.max_cell_idx);
    w.kv_u32("cyc", bank.cycles);
    w.kv_u32("warn", bank.warn_flags);
    w.kv_u32("prot", bank.prot_flags);
    w.kv_u32("fault", bank.fault_flags);
    w.kv_str("inh", inhibit_reason_name(last_inh));
    w.kv_bool("dyn_disc", dyn_disc);
    w.kv_str("bridge_mode", bridge_active ? "ACTIVE" : "MONITOR_ONLY");
    w.kv_bool("maint_unlock", maint_unlock);
    w.kv_u32("maint_rem_s", maint_rem_s);
    w.kv_f2("pol_cvl", pol.cvl_v);
    w.kv_f2("pol_dvl", pol.dvl_v);
    w.kv_f1("pol_ccl", pol.ccl_max_a);
    w.kv_f1("pol_dcl", pol.dcl_max_a);
    w.kv_f2("pol_dv", pol.max_delta_v);
    w.kv_bool("pol_mod", pol.modified);
    return w.finish();
}

inline size_t build_diag_state_json(
    const HaDiagSnapshot& d, const HaSystemHealth& sys, char* out, size_t cap)
{
    const char* inv_mode = (d.inv_63 > 0u && d.inv_62 > 0u)
        ? "PYL_FULL"
        : ((d.inv_61 > 0u && d.inv_63 == 0u) ? "SCAN_61H_ONLY" : "IDLE_OTHER");

    ha_detail::JsonWriter w(out, cap);
    w.kv_str("proto", ha_proto_name(d.proto));
    w.kv_str("inv_mode", inv_mode);
    w.kv_bool("cell_log", d.cell_log);
    w.kv_u32("bat_ok", d.bat_poll_ok);
    w.kv_u32("bat_err", d.bat_poll_fail);
    w.kv_u32("inv_rx", d.inv_req_count);
    w.kv_u32("inv_tx", d.inv_resp_count);
    w.kv_u32("inv_bytes", d.inv_raw_bytes);
    w.kv_u32("inh_cnt", d.inhibit_count);
    w.kv_str("last_inh", inhibit_reason_name(d.last_inhibit_reason));
    w.kv_u32("last_inh_age_ms", d.last_inhibit_age_ms);
    w.kv_f2("last_inh_dv", d.last_inhibit_dv);
    w.kv_u32("dv_retry_ok", d.dv_retry_ok);
    w.kv_u32("dv_retry_fail", d.dv_retry_fail);
    w.kv_f2("peak_dv", d.peak_dv);
    w.kv_u32("peak_age_ms", d.peak_age_ms);
    w.kv_u32("inv_60", d.inv_60);
    w.kv_u32("inv_61", d.inv_61);
    w.kv_u32("inv_62", d.inv_62);
    w.kv_u32("inv_63", d.inv_63);
    w.kv_u32("inv_oth", d.inv_other);
    w.kv_u32("mtx_miss", d.inv_mtx_miss);
    w.kv_str("tx61", d.tx61);
    w.kv_str("tx62", d.tx62);
    w.kv_str("tx63", d.tx63);
    w.kv_u32("tx61_age_ms", d.tx61_age_ms);
    w.kv_u32("tx62_age_ms", d.tx62_age_ms);
    w.kv_u32("tx63_age_ms", d.tx63_age_ms);
    w.kv_i32("rssi", sys.wifi_rssi_dbm);
    w.kv_u32("heap", sys.free_heap_bytes);
    w.kv_u32("min_heap", sys.min_free_heap_bytes);
    w.kv_u32("uptime_s", sys.uptime_s);
    w.kv_u32("mqtt_reconn", sys.mqtt_reconnects);
    w.kv_str("ip", sys.ip_addr);
    return w.finish();
}

inline size_t build_pack_state_json(
    uint8_t slot_1based, bool online, const PackState& p,
    uint32_t age_ms, uint8_t miss_cnt, char* out, size_t cap)
{
    uint16_t dv_mv = (p.ext.max_cell_mv >= p.ext.min_cell_mv && p.ext.min_cell_mv > 0u)
        ? static_cast<uint16_t>(p.ext.max_cell_mv - p.ext.min_cell_mv)
        : 0u;

    ha_detail::JsonWriter w(out, cap);
    w.kv_u32("slot", slot_1based);
    w.kv_u32("addr", p.addr);
    w.kv_bool("online", online);
    w.kv_str("proto", ha_proto_name(p.detected_proto));
    w.kv_u32("age_ms", age_ms);
    w.kv_u32("miss", miss_cnt);
    w.kv_f2("v", p.core.bus_v);
    w.kv_f2("i", p.core.bus_a);
    w.kv_f1("soc", p.core.soc);
    w.kv_f1("soh", p.ext.soh);
    w.kv_f1("cap_ah", p.core.capacity_ah);
    w.kv_f1("rem_ah", p.ext.rem_cap_ah);
    w.kv_u32("cyc", p.ext.cycles);
    w.kv_bool("crit", p.core.critical);
    w.kv_bool("chg", p.core.allow_charge);
    w.kv_bool("dis", p.core.allow_discharge);
    w.kv_f1("t_min", p.ext.min_temp_c);
    w.kv_f1("t_max", p.ext.max_temp_c);
    w.kv_f1("t_avg", p.ext.avg_temp_c);
    w.kv_u32("t_cnt", p.ext.temp_count);
    for (uint8_t k = 0u; k < 5u; ++k) {
        char tk[4];
        std::snprintf(tk, sizeof(tk), "t%u", static_cast<unsigned>(k + 1u));
        w.kv_f1(tk, p.ext.temps_c[k]);
    }
    w.kv_u32("c_cnt", p.ext.cell_count);
    w.kv_u32("c_min_mv", p.ext.min_cell_mv);
    w.kv_u32("c_max_mv", p.ext.max_cell_mv);
    w.kv_u32("c_dv_mv", dv_mv);
    w.kv_u32("c_min_i", p.ext.min_cell_idx);
    w.kv_u32("c_max_i", p.ext.max_cell_idx);
    w.kv_u32("warn", p.ext.warn_flags);
    w.kv_u32("prot", p.ext.prot_flags);
    w.kv_u32("fault", p.ext.fault_flags);
    w.kv_u32("stat", p.ext.status_flags);
    return w.finish();
}

inline size_t build_pack_cells_json(const PackState& p, char* out, size_t cap) {
    uint16_t dv_mv = (p.ext.max_cell_mv >= p.ext.min_cell_mv && p.ext.min_cell_mv > 0u)
        ? static_cast<uint16_t>(p.ext.max_cell_mv - p.ext.min_cell_mv)
        : 0u;
    uint8_t n = (p.ext.cell_count > 15u && p.ext.cell_count <= 16u) ? 16u : 15u;

    ha_detail::JsonWriter w(out, cap);
    w.kv_u32("cnt", p.ext.cell_count);
    w.kv_u32("min_mv", p.ext.min_cell_mv);
    w.kv_u32("max_mv", p.ext.max_cell_mv);
    w.kv_u32("dv_mv", dv_mv);
    w.kv_u32("min_i", p.ext.min_cell_idx);
    w.kv_u32("max_i", p.ext.max_cell_idx);
    for (uint8_t i = 0u; i < n; ++i) {
        char ck[6];
        std::snprintf(ck, sizeof(ck), "c%02u", static_cast<unsigned>(i + 1u));
        w.kv_u32(ck, p.ext.cell_mv[i]);
    }
    return w.finish();
}

inline size_t build_pack_eeprom_json(const PackEepromSnapshot& eep, char* out, size_t cap) {
    ha_detail::JsonWriter w(out, cap);
    w.kv_u32("addr", eep.addr);
    w.kv_bool("valid", eep.valid);
    w.kv_u32("aud_ms", eep.audited_ms);
    w.kv_bool("p_ok", eep.prot.valid);
    w.kv_bool("a_ok", eep.alarm.valid);
    w.kv_u32("tags", eep.alarm.tag_count);

    // 13 protection parameters (cmd 0x06..0x50)
    w.kv_u32("p_cov", eep.prot.cell_ov_prot_mv);
    w.kv_u32("p_covr", eep.prot.cell_ov_rec_mv);
    w.kv_u32("p_cuv", eep.prot.cell_uv_prot_mv);
    w.kv_u32("p_cuvr", eep.prot.cell_uv_rec_mv);
    w.kv_u32("p_pov", eep.prot.pack_ov_prot_cv);
    w.kv_u32("p_povr", eep.prot.pack_ov_rec_cv);
    w.kv_u32("p_puv", eep.prot.pack_uv_prot_cv);
    w.kv_u32("p_puvr", eep.prot.pack_uv_rec_cv);
    w.kv_u32("p_ccv", eep.prot.const_chg_cv);
    w.kv_u32("p_cci", eep.prot.const_chg_ci);
    w.kv_u32("p_slpv", eep.prot.cell_sleep_mv);
    w.kv_u32("p_uvo", eep.prot.uv_off_interval);
    w.kv_u32("p_slpt", eep.prot.sleep_time_min);

    // All 43 alarm tags (cmd 0x43, tags 0x00..0x2A)
    w.kv_u32("a_cov", eep.alarm.cell_ov_alarm_mv);
    w.kv_u32("a_covd", eep.alarm.cell_ov_delay_ms);
    w.kv_u32("a_covr", eep.alarm.cell_ov_alarm_rec_mv);
    w.kv_u32("a_cuv", eep.alarm.cell_uv_alarm_mv);
    w.kv_u32("a_cuvd", eep.alarm.cell_uv_delay_ms);
    w.kv_u32("a_cuvr", eep.alarm.cell_uv_alarm_rec_mv);
    w.kv_u32("a_pov", eep.alarm.pack_ov_alarm_cv);
    w.kv_u32("a_povd", eep.alarm.pack_ov_delay_ms);
    w.kv_u32("a_povr", eep.alarm.pack_ov_alarm_rec_cv);
    w.kv_u32("a_puv", eep.alarm.pack_uv_alarm_cv);
    w.kv_u32("a_puvd", eep.alarm.pack_uv_delay_ms);
    w.kv_u32("a_puvr", eep.alarm.pack_uv_alarm_rec_cv);
    w.kv_u32("a_coc", eep.alarm.chg_oc_ca);
    w.kv_u32("a_cocd", eep.alarm.chg_oc_delay_ms);
    w.kv_u32("a_cocr", eep.alarm.chg_oc_rec_ca);
    w.kv_u32("a_doc", eep.alarm.dsg_oc_ca);
    w.kv_u32("a_docd", eep.alarm.dsg_oc_delay_ms);
    w.kv_u32("a_docr", eep.alarm.dsg_oc_rec_ca);
    w.kv_i32("a_cot", eep.alarm.chg_ot_c);
    w.kv_u32("a_cotd", eep.alarm.chg_ot_delay_ms);
    w.kv_i32("a_cotr", eep.alarm.chg_ot_rec_c);
    w.kv_i32("a_cut", eep.alarm.chg_ut_c);
    w.kv_u32("a_cutd", eep.alarm.chg_ut_delay_ms);
    w.kv_i32("a_cutr", eep.alarm.chg_ut_rec_c);
    w.kv_i32("a_dot", eep.alarm.dsg_ot_c);
    w.kv_u32("a_dotd", eep.alarm.dsg_ot_delay_ms);
    w.kv_i32("a_dotr", eep.alarm.dsg_ot_rec_c);
    w.kv_i32("a_dut", eep.alarm.dsg_ut_c);
    w.kv_u32("a_dutd", eep.alarm.dsg_ut_delay_ms);
    w.kv_i32("a_dutr", eep.alarm.dsg_ut_rec_c);
    w.kv_i32("a_mot", eep.alarm.mos_ot_c);
    w.kv_u32("a_motd", eep.alarm.mos_ot_delay_ms);
    w.kv_i32("a_motr", eep.alarm.mos_ot_rec_c);
    w.kv_u32("a_lsoc", eep.alarm.low_soc_alarm_pct);
    w.kv_u32("a_lsocr", eep.alarm.low_soc_alarm_rec_pct);
    w.kv_u32("a_cdv", eep.alarm.cell_diff_alarm_mv);
    w.kv_u32("a_cdvr", eep.alarm.cell_diff_alarm_rec_mv);
    w.kv_i32("a_eot", eep.alarm.env_ot_c);
    w.kv_u32("a_eotd", eep.alarm.env_ot_delay_ms);
    w.kv_i32("a_eotr", eep.alarm.env_ot_rec_c);
    w.kv_i32("a_eut", eep.alarm.env_ut_c);
    w.kv_u32("a_eutd", eep.alarm.env_ut_delay_ms);
    w.kv_i32("a_eutr", eep.alarm.env_ut_rec_c);
    return w.finish();
}

// -------------------- MQTT Auto-Discovery Builder --------------------

enum HaDiscoveryType : uint8_t {
    HA_DISC_SENSOR = 0,
    HA_DISC_BINARY_SENSOR,
    HA_DISC_BUTTON,
    HA_DISC_SWITCH,
    HA_DISC_SELECT,
    HA_DISC_NUMBER
};

namespace ha_detail {

// Pure POD aggregate (no NSDMI) for C++11 & C++17 compatibility
struct HaDiscoveryDesc {
    HaDiscoveryType type;
    const char* obj_id;
    const char* name;
    const char* state_subtopic;
    const char* val_tpl;
    const char* unit;
    const char* dev_class;
    const char* state_class;
    const char* ent_cat;
    const char* cmd_suffix;
    const char* payload_press;
    const char* select_options_json;
    const char* num_min;
    const char* num_max;
    const char* num_step;
    bool has_json_attrs;
};

inline const char* ha_component_name(HaDiscoveryType t) {
    switch (t) {
        case HA_DISC_SENSOR:        return "sensor";
        case HA_DISC_BINARY_SENSOR: return "binary_sensor";
        case HA_DISC_BUTTON:        return "button";
        case HA_DISC_SWITCH:        return "switch";
        case HA_DISC_SELECT:        return "select";
        case HA_DISC_NUMBER:        return "number";
        default:                    return "sensor";
    }
}

static const HaDiscoveryDesc kGlobalDiscovery[] = {
    // Bank live telemetry
    {HA_DISC_SENSOR,        "bank_soc",            "Bank SOC",            "bank", "{{ value_json.soc }}",        "%",      "battery",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_raw_soc",        "Bank Raw SOC",        "bank", "{{ value_json.raw_soc }}",    "%",      "battery",     "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_voltage",        "Bank Voltage",        "bank", "{{ value_json.v }}",          "V",      "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_current",        "Bank Current",        "bank", "{{ value_json.i }}",          "A",      "current",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_power",          "Bank Power",          "bank", "{{ value_json.p }}",          "W",      "power",       "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_soh",            "Bank SOH",            "bank", "{{ value_json.soh }}",        "%",      nullptr,       "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_capacity_ah",    "Bank Capacity",       "bank", "{{ value_json.cap_ah }}",     "Ah",     nullptr,       "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_rem_cap_ah",     "Bank Remaining Ah",   "bank", "{{ value_json.rem_ah }}",     "Ah",     nullptr,       "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BINARY_SENSOR, "bank_valid",          "Bank Valid",          "bank", "{{ 'ON' if value_json.valid else 'OFF' }}", nullptr, nullptr, nullptr,    nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_inhibit_reason", "Bank Inhibit Reason", "bank", "{{ value_json.inh }}",        nullptr,  nullptr,       nullptr,            nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_pack_count",     "Bank Online Packs",   "bank", "{{ value_json.packs }}",      nullptr,  nullptr,       "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_cvl",            "Bank CVL",            "bank", "{{ value_json.cvl }}",        "V",      "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_dvl",            "Bank DVL",            "bank", "{{ value_json.dvl }}",        "V",      "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_ccl",            "Bank CCL",            "bank", "{{ value_json.ccl }}",        "A",      "current",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_dcl",            "Bank DCL",            "bank", "{{ value_json.dcl }}",        "A",      "current",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BINARY_SENSOR, "bank_allow_charge",   "Bank Allow Charge",   "bank", "{{ 'ON' if value_json.chg else 'OFF' }}", nullptr, nullptr, nullptr,      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BINARY_SENSOR, "bank_allow_discharge","Bank Allow Discharge","bank", "{{ 'ON' if value_json.dis else 'OFF' }}", nullptr, nullptr, nullptr,      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_temp_min",       "Bank Temp Min",       "bank", "{{ value_json.t_min }}",      "\xC2\xB0""C", "temperature", "measurement", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_temp_max",       "Bank Temp Max",       "bank", "{{ value_json.t_max }}",      "\xC2\xB0""C", "temperature", "measurement", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_temp_avg",       "Bank Temp Avg",       "bank", "{{ value_json.t_avg }}",      "\xC2\xB0""C", "temperature", "measurement", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_cell_min_mv",    "Bank Cell Min",       "bank", "{{ value_json.c_min_mv }}",   "mV",     "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_cell_max_mv",    "Bank Cell Max",       "bank", "{{ value_json.c_max_mv }}",   "mV",     "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_cell_dv_mv",     "Bank Cell Delta V",   "bank", "{{ value_json.c_dv_mv }}",    "mV",     "voltage",     "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_cycles",         "Bank Cycles",         "bank", "{{ value_json.cyc }}",        nullptr,  nullptr,       "total_increasing", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_warn_flags",     "Bank Warn Flags",     "bank", "{{ value_json.warn }}",       nullptr,  nullptr,       nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_prot_flags",     "Bank Prot Flags",     "bank", "{{ value_json.prot }}",       nullptr,  nullptr,       nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "bank_fault_flags",    "Bank Fault Flags",    "bank", "{{ value_json.fault }}",      nullptr,  nullptr,       nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "maint_unlock_rem_s",  "Maint Unlock Remaining","bank","{{ value_json.maint_rem_s }}","s",      "duration",    "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},

    // Diagnostics & ESP32 system health
    {HA_DISC_SENSOR, "diag_proto",         "Shoto Protocol",       "diag", "{{ value_json.proto }}",       nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_mode",      "Inverter Mode",        "diag", "{{ value_json.inv_mode }}",    nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_bat_ok",        "Battery Poll OK",      "diag", "{{ value_json.bat_ok }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_bat_err",       "Battery Poll Errors",  "diag", "{{ value_json.bat_err }}",     nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_rx",        "Inverter RX Count",    "diag", "{{ value_json.inv_rx }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_tx",        "Inverter TX Count",    "diag", "{{ value_json.inv_tx }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_60",        "Inverter 60H Count",   "diag", "{{ value_json.inv_60 }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_61",        "Inverter 61H Count",   "diag", "{{ value_json.inv_61 }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_62",        "Inverter 62H Count",   "diag", "{{ value_json.inv_62 }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_63",        "Inverter 63H Count",   "diag", "{{ value_json.inv_63 }}",      nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inv_mtx_miss",  "Inverter Mutex Miss",  "diag", "{{ value_json.mtx_miss }}",    nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_inhibit_count", "Inhibit Count",        "diag", "{{ value_json.inh_cnt }}",     nullptr, nullptr,           "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_peak_dv",       "Peak Pack Delta V",    "diag", "{{ value_json.peak_dv }}",     "V",     "voltage",         "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_peak_age_ms",   "Peak Poll Age",        "diag", "{{ value_json.peak_age_ms }}", "ms",    "duration",        "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_tx61",          "Last TX 61H Frame",    "diag", "{{ value_json.tx61 }}",        nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_tx62",          "Last TX 62H Frame",    "diag", "{{ value_json.tx62 }}",        nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "diag_tx63",          "Last TX 63H Frame",    "diag", "{{ value_json.tx63 }}",        nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "sys_wifi_rssi",      "ESP32 Wi-Fi RSSI",     "diag", "{{ value_json.rssi }}",        "dBm",   "signal_strength", "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "sys_free_heap",      "ESP32 Free Heap",      "diag", "{{ value_json.heap }}",        "B",     "data_size",       "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "sys_uptime_s",       "ESP32 Uptime",         "diag", "{{ value_json.uptime_s }}",    "s",     "duration",        "total_increasing", "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR, "sys_ip",             "ESP32 IP Address",     "diag", "{{ value_json.ip }}",          nullptr, nullptr,           nullptr,            "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},

    // Safe action buttons & switches
    {HA_DISC_BUTTON, "btn_rescan",              "Rescan Packs",         nullptr, nullptr, nullptr, nullptr, nullptr, "diagnostic", "rescan",              "PRESS", nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BUTTON, "btn_audit_eeprom_all",    "Audit EEPROM All",     nullptr, nullptr, nullptr, nullptr, nullptr, "diagnostic", "audit_eeprom",        "all",   nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BUTTON, "btn_reset_diag",          "Reset Diag Counters",  nullptr, nullptr, nullptr, nullptr, nullptr, "diagnostic", "reset_diag",          "PRESS", nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BUTTON, "btn_republish_discovery", "Republish Discovery",  nullptr, nullptr, nullptr, nullptr, nullptr, "diagnostic", "republish_discovery", "PRESS", nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SWITCH, "sw_cell_log",             "Auto Cell Log",        "diag",  "{{ 'ON' if value_json.cell_log else 'OFF' }}",     nullptr, nullptr, nullptr, "config", "cell_log",     nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SWITCH, "sw_maint_unlock",         "Maintenance Unlock",   "bank",  "{{ 'ON' if value_json.maint_unlock else 'OFF' }}", nullptr, nullptr, nullptr, "config", "maint_unlock", nullptr, nullptr, nullptr, nullptr, nullptr, false},

    // Guarded critical controls (require Maintenance Unlock)
    {HA_DISC_SWITCH, "sw_dyn_discovery",   "Dynamic Pack Discovery", "bank", "{{ 'ON' if value_json.dyn_disc else 'OFF' }}", nullptr, nullptr, nullptr, "config", "dyn_discovery", nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SELECT, "sel_bridge_mode",    "Inverter Bridge Mode",   "bank", "{{ value_json.bridge_mode }}", nullptr, nullptr, nullptr, "config", "bridge_mode", nullptr, "[\"ACTIVE\",\"MONITOR_ONLY\"]", nullptr, nullptr, nullptr, false},
    {HA_DISC_NUMBER, "num_policy_cvl",     "Policy CVL Max",         "bank", "{{ value_json.pol_cvl }}",     "V",     "voltage", nullptr, "config", "policy_cvl",    nullptr, nullptr, "51.0", "54.4", "0.1",  false},
    {HA_DISC_NUMBER, "num_policy_dvl",     "Policy DVL Min",         "bank", "{{ value_json.pol_dvl }}",     "V",     "voltage", nullptr, "config", "policy_dvl",    nullptr, nullptr, "44.0", "48.0", "0.1",  false},
    {HA_DISC_NUMBER, "num_policy_ccl",     "Policy CCL Cap",         "bank", "{{ value_json.pol_ccl }}",     "A",     "current", nullptr, "config", "policy_ccl",    nullptr, nullptr, "10.0", "90.0", "1.0",  false},
    {HA_DISC_NUMBER, "num_policy_dcl",     "Policy DCL Cap",         "bank", "{{ value_json.pol_dcl }}",     "A",     "current", nullptr, "config", "policy_dcl",    nullptr, nullptr, "10.0", "120.0","1.0",  false},
    {HA_DISC_NUMBER, "num_policy_max_dv",  "Policy Max Delta V",     "bank", "{{ value_json.pol_dv }}",      "V",     "voltage", nullptr, "config", "policy_max_dv", nullptr, nullptr, "0.5",  "1.5",  "0.05", false},
    {HA_DISC_BUTTON, "btn_policy_reset",   "Reset Policy Defaults",  nullptr, nullptr, nullptr, nullptr, nullptr, "config", "policy_reset", "PRESS", nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_BUTTON, "btn_restart",        "Restart ESP32",          nullptr, nullptr, nullptr, nullptr, nullptr, "config", "restart",      "PRESS", nullptr, nullptr, nullptr, nullptr, false}
};

static const HaDiscoveryDesc kPackStaticDesc[] = {
    {HA_DISC_BINARY_SENSOR, "online",            "Online",              "state",  "{{ 'ON' if value_json.online else 'OFF' }}", nullptr, "connectivity", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "voltage",           "Voltage",             "state",  "{{ value_json.v }}",        "V",  "voltage", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "current",           "Current",             "state",  "{{ value_json.i }}",        "A",  "current", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "soc",               "SOC",                 "state",  "{{ value_json.soc }}",      "%",  "battery", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "soh",               "SOH",                 "state",  "{{ value_json.soh }}",      "%",  nullptr,   "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "rem_cap_ah",        "Remaining Ah",        "state",  "{{ value_json.rem_ah }}",   "Ah", nullptr,   "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "cycles",            "Cycles",              "state",  "{{ value_json.cyc }}",      nullptr, nullptr,"total_increasing", nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "prot_flags",        "Prot Flags",          "state",  "{{ value_json.prot }}",     nullptr, nullptr, nullptr,           "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "fault_flags",       "Fault Flags",         "state",  "{{ value_json.fault }}",    nullptr, nullptr, nullptr,           "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "warn_flags",        "Warn Flags",          "state",  "{{ value_json.warn }}",     nullptr, nullptr, nullptr,           "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "miss_cnt",          "Poll Miss Count",     "state",  "{{ value_json.miss }}",     nullptr, nullptr, "measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "age_ms",            "Sample Age",          "state",  "{{ value_json.age_ms }}",   "ms", "duration","measurement",      "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "cell_min_mv",       "Cell Min",            "cells",  "{{ value_json.min_mv }}",   "mV", "voltage", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "cell_max_mv",       "Cell Max",            "cells",  "{{ value_json.max_mv }}",   "mV", "voltage", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false},
    {HA_DISC_SENSOR,        "cell_dv_mv",        "Cell Delta V",        "cells",  "{{ value_json.dv_mv }}",    "mV", "voltage", "measurement",      nullptr,      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, true},
    {HA_DISC_BINARY_SENSOR, "eep_valid",         "EEPROM Valid",        "eeprom", "{{ 'ON' if value_json.valid else 'OFF' }}", nullptr, nullptr, nullptr, "diagnostic", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, true},
    {HA_DISC_BUTTON,        "btn_audit_eeprom",  "Audit EEPROM",        nullptr,  nullptr,                     nullptr, nullptr, nullptr,           "diagnostic", "audit_eeprom", nullptr, nullptr, nullptr, nullptr, nullptr, false}
};

static constexpr size_t kGlobalDiscoveryCount = sizeof(kGlobalDiscovery) / sizeof(kGlobalDiscovery[0]);
static constexpr size_t kPackStaticCount = sizeof(kPackStaticDesc) / sizeof(kPackStaticDesc[0]);
static constexpr size_t kPackTempCount = 5u;
static constexpr size_t kPackCellCount = 15u;
static constexpr size_t kPerPackDiscoveryCount = kPackStaticCount + kPackTempCount + kPackCellCount;

} // namespace ha_detail

inline size_t ha_discovery_entity_count(uint8_t max_packs) {
    return ha_detail::kGlobalDiscoveryCount +
           static_cast<size_t>(max_packs) * ha_detail::kPerPackDiscoveryCount;
}

inline bool build_ha_discovery_payload(
    size_t idx, uint8_t max_packs,
    const char* dev_id, const char* site_name, const char* topic_pfx, const char* sw_ver,
    char* topic_out, size_t topic_cap, char* payload_out, size_t payload_cap)
{
    if (!dev_id || !dev_id[0] || !site_name || !site_name[0] ||
        !topic_pfx || !topic_pfx[0] || !sw_ver || !sw_ver[0] ||
        !topic_out || topic_cap == 0u || !payload_out || payload_cap == 0u) {
        return false;
    }
    if (idx >= ha_discovery_entity_count(max_packs)) {
        return false;
    }

    ha_detail::HaDiscoveryDesc desc{};
    char obj_buf[48] = "";
    char name_buf[48] = "";
    char state_sub_buf[32] = "";
    char tpl_buf[48] = "";
    char press_buf[8] = "";

    if (idx < ha_detail::kGlobalDiscoveryCount) {
        desc = ha_detail::kGlobalDiscovery[idx];
    } else {
        size_t rem = idx - ha_detail::kGlobalDiscoveryCount;
        size_t slot = (rem / ha_detail::kPerPackDiscoveryCount) + 1u;
        size_t sub = rem % ha_detail::kPerPackDiscoveryCount;
        if (sub < ha_detail::kPackStaticCount) {
            const ha_detail::HaDiscoveryDesc& base = ha_detail::kPackStaticDesc[sub];
            desc = base;
            std::snprintf(obj_buf, sizeof(obj_buf), "pack%u_%s",
                          static_cast<unsigned>(slot), base.obj_id);
            std::snprintf(name_buf, sizeof(name_buf), "Pack %u %s",
                          static_cast<unsigned>(slot), base.name);
            desc.obj_id = obj_buf;
            desc.name = name_buf;
            if (base.state_subtopic) {
                std::snprintf(state_sub_buf, sizeof(state_sub_buf), "pack%u/%s",
                              static_cast<unsigned>(slot), base.state_subtopic);
                desc.state_subtopic = state_sub_buf;
            }
            if (desc.type == HA_DISC_BUTTON && !desc.payload_press) {
                std::snprintf(press_buf, sizeof(press_buf), "%u", static_cast<unsigned>(slot));
                desc.payload_press = press_buf;
            }
        } else if (sub < ha_detail::kPackStaticCount + ha_detail::kPackTempCount) {
            size_t t = (sub - ha_detail::kPackStaticCount) + 1u;
            std::snprintf(obj_buf, sizeof(obj_buf), "pack%u_temp_%u",
                          static_cast<unsigned>(slot), static_cast<unsigned>(t));
            std::snprintf(name_buf, sizeof(name_buf), "Pack %u Temp %u",
                          static_cast<unsigned>(slot), static_cast<unsigned>(t));
            std::snprintf(state_sub_buf, sizeof(state_sub_buf), "pack%u/state",
                          static_cast<unsigned>(slot));
            std::snprintf(tpl_buf, sizeof(tpl_buf), "{{ value_json.t%u }}",
                          static_cast<unsigned>(t));
            desc.type = HA_DISC_SENSOR;
            desc.obj_id = obj_buf;
            desc.name = name_buf;
            desc.state_subtopic = state_sub_buf;
            desc.val_tpl = tpl_buf;
            desc.unit = "\xC2\xB0""C";
            desc.dev_class = "temperature";
            desc.state_class = "measurement";
        } else {
            size_t c = (sub - ha_detail::kPackStaticCount - ha_detail::kPackTempCount) + 1u;
            std::snprintf(obj_buf, sizeof(obj_buf), "pack%u_cell_%02u",
                          static_cast<unsigned>(slot), static_cast<unsigned>(c));
            std::snprintf(name_buf, sizeof(name_buf), "Pack %u Cell %02u",
                          static_cast<unsigned>(slot), static_cast<unsigned>(c));
            std::snprintf(state_sub_buf, sizeof(state_sub_buf), "pack%u/cells",
                          static_cast<unsigned>(slot));
            std::snprintf(tpl_buf, sizeof(tpl_buf), "{{ value_json.c%02u }}",
                          static_cast<unsigned>(c));
            desc.type = HA_DISC_SENSOR;
            desc.obj_id = obj_buf;
            desc.name = name_buf;
            desc.state_subtopic = state_sub_buf;
            desc.val_tpl = tpl_buf;
            desc.unit = "mV";
            desc.dev_class = "voltage";
            desc.state_class = "measurement";
        }
    }

    const char* comp = ha_detail::ha_component_name(desc.type);
    int tw = std::snprintf(topic_out, topic_cap, "homeassistant/%s/%s/%s/config",
                           comp, dev_id, desc.obj_id);
    if (tw <= 0 || static_cast<size_t>(tw) >= topic_cap) {
        topic_out[0] = '\0';
        return false;
    }

    char uid[160];
    char deid[176];
    char avail[160];
    int uw = std::snprintf(uid, sizeof(uid), "%s_%s", dev_id, desc.obj_id);
    int dw = std::snprintf(deid, sizeof(deid), "%s.%s_%s", comp, dev_id, desc.obj_id);
    int aw = std::snprintf(avail, sizeof(avail), "%s/status", topic_pfx);
    if (uw <= 0 || static_cast<size_t>(uw) >= sizeof(uid) ||
        dw <= 0 || static_cast<size_t>(dw) >= sizeof(deid) ||
        aw <= 0 || static_cast<size_t>(aw) >= sizeof(avail)) {
        topic_out[0] = '\0';
        return false;
    }

    ha_detail::JsonWriter w(payload_out, payload_cap);
    w.kv_str("name", desc.name);
    w.kv_str("unique_id", uid);
    w.kv_str("default_entity_id", deid);
    w.kv_str("availability_topic", avail);
    w.kv_str("payload_available", "online");
    w.kv_str("payload_not_available", "offline");

    if (desc.state_subtopic) {
        char st_topic[160];
        int sw = std::snprintf(st_topic, sizeof(st_topic), "%s/%s", topic_pfx, desc.state_subtopic);
        if (sw <= 0 || static_cast<size_t>(sw) >= sizeof(st_topic)) {
            topic_out[0] = '\0';
            return false;
        }
        w.kv_str("state_topic", st_topic);
        if (desc.has_json_attrs) {
            w.kv_str("json_attributes_topic", st_topic);
        }
    }
    if (desc.val_tpl) w.kv_str("value_template", desc.val_tpl);
    if (desc.unit) w.kv_str("unit_of_measurement", desc.unit);
    if (desc.dev_class) w.kv_str("device_class", desc.dev_class);
    if (desc.state_class) w.kv_str("state_class", desc.state_class);
    if (desc.ent_cat) w.kv_str("entity_category", desc.ent_cat);

    if (desc.cmd_suffix) {
        char cmd_topic[160];
        int cw = std::snprintf(cmd_topic, sizeof(cmd_topic), "%s/cmd/%s", topic_pfx, desc.cmd_suffix);
        if (cw <= 0 || static_cast<size_t>(cw) >= sizeof(cmd_topic)) {
            topic_out[0] = '\0';
            return false;
        }
        w.kv_str("command_topic", cmd_topic);
    }
    if (desc.type == HA_DISC_BUTTON && desc.payload_press) {
        w.kv_str("payload_press", desc.payload_press);
    } else if (desc.type == HA_DISC_SWITCH) {
        w.kv_str("payload_on", "ON");
        w.kv_str("payload_off", "OFF");
        w.kv_str("state_on", "ON");
        w.kv_str("state_off", "OFF");
    } else if (desc.type == HA_DISC_SELECT && desc.select_options_json) {
        w.kv_raw("options", desc.select_options_json);
    } else if (desc.type == HA_DISC_NUMBER) {
        if (desc.num_min) w.kv_raw("min", desc.num_min);
        if (desc.num_max) w.kv_raw("max", desc.num_max);
        if (desc.num_step) w.kv_raw("step", desc.num_step);
        w.kv_str("mode", "box");
    }

    // Device block
    w.begin_key("device");
    w.append_raw("{", 1u);
    w.first = true;
    w.begin_key("identifiers");
    w.append_raw("[", 1u);
    w.append_escaped(dev_id);
    w.append_raw("]", 1u);
    w.kv_str("name", site_name);
    w.kv_str("manufacturer", "Shoto/Samoto DIY");
    w.kv_str("model", "ESP32 Shoto-Samoto Bridge");
    w.kv_str("sw_version", sw_ver);
    w.append_raw("}", 1u);

    size_t plen = w.finish();
    if (plen == 0u) {
        topic_out[0] = '\0';
        return false;
    }
    return true;
}
