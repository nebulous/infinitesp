"""Auto-generated entity surface.

Users declare the hub and one climate block per zone; the per-zone entities
and the system-wide entities listed here spawn automatically in codegen. An
entity the user declared explicitly (same domain platform, same type, same
zone for zone-scoped types) suppresses its auto twin, so existing fully
explicit configs compile unchanged. Opt-out flags only remove.

Mechanism: each spawn builds a minimal config dict, runs it through the
platform's own CONFIG_SCHEMA (defaults filled, id auto-declared, metadata
injected from the TYPES tables — the exact path explicit declarations take),
then calls the platform's to_code. Entities are therefore indistinguishable
from yaml-declared ones in generated code and the native API.
"""
import itertools

from esphome import config_validation as cv
from esphome.const import CONF_NAME
from esphome.core import CORE

from . import sensor as _sensor_platform

# CONF_ZONE is not in esphome.const; every platform schema keys on the string.
CONF_ZONE = "zone"

# ---------------------------------------------------------------------------
# Matrix. Names reproduce the reference yaml's names so explicit and auto
# configs generate identical entity sets.
#
# Manual-only types (never spawned; no matrix rows):
#   raw_register, zc_zone_temperature, zc_lat, zc_hpt (user inputs),
#   active_fault (deprecated), odu_indoor_ambient, idu_med_heat_cycles/hours
#   (not in the reference config's set; added here only with evidence).
# ---------------------------------------------------------------------------

# Per-zone entities. label None = custom naming function.
PER_ZONE_ENTITIES = [
    {"domain": "sensor", "type": "temperature", "label": "Temperature", "flag": "temperature"},
    {"domain": "sensor", "type": "humidity", "label": "Humidity", "flag": "humidity"},
    {"domain": "binary_sensor", "type": "occupancy", "label": "Occupancy", "flag": "occupancy"},
    {"domain": "text_sensor", "type": "zone_name", "label": "Name", "flag": "zone_name"},
    {"domain": "text_sensor", "type": "hold_state", "label": "Hold State", "flag": "hold_state"},
    {"domain": "text_sensor", "type": "comfort_profile", "label": None, "flag": "comfort_profile"},
    {"domain": "select", "type": "fan_mode", "label": "Fan Mode", "flag": "fan_mode"},
    {"domain": "cover", "type": None, "label": "Damper", "flag": "damper"},
    {"domain": "datetime", "type": None, "label": "Hold Until", "flag": "hold_until"},
    {"domain": "number", "type": None, "label": "Hold Minutes", "flag": "hold_minutes"},
]

# System-wide, always spawned.
SYSTEM_CORE = [
    ("binary_sensor", "bus_status", "Bus Status"),
    ("binary_sensor", "electric_heat", "Electric Heat"),
    ("binary_sensor", "compressor_running", "Compressor Running"),
    ("sensor", "outdoor_temperature", "Outdoor Temperature"),
    ("sensor", "blower_rpm", "Blower RPM"),
    ("sensor", "airflow_cfm", "Airflow CFM"),
    ("sensor", "vacation_min_temp", "Vacation Min Temp"),
    ("sensor", "vacation_max_temp", "Vacation Max Temp"),
    ("sensor", "odu_outdoor_temp", "ODU Outdoor Temp"),
    ("sensor", "odu_coil_temp", "ODU Coil Temp"),
    ("sensor", "odu_stage", "ODU Stage"),
    ("sensor", "odu_commanded_stage", "ODU Commanded Stage"),
    ("sensor", "odu_mode", "ODU Operating Mode"),
    ("sensor", "odu_line_voltage", "ODU Line Voltage"),
    ("select", "system_mode", "System Mode"),
    ("text_sensor", "fault_history", "Fault History"),
    ("sensor", "fault_timestamp", "Fault Timestamp"),
]

# System-wide, diagnostic entity category; gated by the hub's auto_diagnostics.
SYSTEM_DIAGNOSTIC = [
    ("sensor", "idu_low_heat_cycles", "IDU Low Heat Cycles"),
    ("sensor", "idu_low_heat_hours", "IDU Low Heat Hours"),
    ("sensor", "idu_high_heat_cycles", "IDU High Heat Cycles"),
    ("sensor", "idu_high_heat_hours", "IDU High Heat Hours"),
    ("sensor", "idu_blower_cycles", "IDU Blower Cycles"),
    ("sensor", "idu_blower_hours", "IDU Blower Hours"),
    ("sensor", "idu_poweron_cycles", "IDU Power-On Cycles"),
    ("sensor", "idu_poweron_hours", "IDU Power-On Hours"),
    ("sensor", "odu_heat_cycles", "ODU Heat Cycles"),
    ("sensor", "odu_heat_hours", "ODU Heat Hours"),
    ("sensor", "odu_cool_cycles", "ODU Cool Cycles"),
    ("sensor", "odu_cool_hours", "ODU Cool Hours"),
    ("sensor", "odu_defrost_cycles", "ODU Defrost Cycles"),
    ("sensor", "odu_defrost_hours", "ODU Defrost Hours"),
    ("sensor", "odu_poweron_cycles", "ODU Power-On Cycles"),
    ("sensor", "odu_poweron_hours", "ODU Power-On Hours"),
    ("text_sensor", "tstat_ssid", "Thermostat WiFi SSID"),
    ("text_sensor", "tstat_wifi_mac", "Thermostat WiFi MAC"),
    ("text_sensor", "tstat_hostname", "Thermostat Hostname"),
    ("text_sensor", "tstat_cloud_host", "Thermostat Cloud Host"),
    ("text_sensor", "tstat_proxy_server", "Thermostat Proxy Server"),
    ("text_sensor", "tstat_dealer_name", "Dealer Name"),
    ("text_sensor", "tstat_dealer_brand", "Dealer Brand"),
    ("text_sensor", "tstat_dealer_url", "Dealer URL"),
    ("text_sensor", "manufacture_date", "Thermostat Manufacture Date"),
    ("text_sensor", "version", "InfinitESP Version"),
]

# System-wide, equipment-conditional (variable-speed ODU registers; the 3E
# two-stage family does not serve them). Spawned disabled_by_default so they
# do not clutter HA on installs whose equipment lacks them.
SYSTEM_CONDITIONAL = [
    ("sensor", "compressor_rpm", "Compressor RPM"),
    ("sensor", "target_compressor_rpm", "Compressor Target RPM"),
    ("sensor", "odu_requested_cfm", "ODU Requested CFM"),
    ("sensor", "odu_expansion_valve", "ODU Expansion Valve"),
    ("sensor", "odu_float_1", "ODU Float 1"),
    ("sensor", "odu_float_2", "ODU Float 2"),
    ("sensor", "odu_float_3", "ODU Float 3"),
    ("sensor", "odu_float_4", "ODU Float 4"),
    ("sensor", "odu_float_5", "ODU Float 5"),
    ("sensor", "odu_float_6", "ODU Float 6"),
    ("sensor", "odu_discharge_temp", "ODU Discharge Temp"),
    ("sensor", "odu_suction_temp", "ODU Suction Temp"),
    ("sensor", "odu_suction_superheat", "ODU Suction Superheat"),
]

# Zone-scoped types per domain: suppression matches on (type, zone). Every
# other type is system-wide for suppression purposes: any explicit block of
# that type suppresses the auto spawn.
_ZONE_SCOPED = {
    "sensor": {"temperature", "humidity"},
    "binary_sensor": {"occupancy"},
    "text_sensor": {"zone_name", "hold_state", "comfort_profile"},
    "select": {"fan_mode"},
    "cover": {None},
    "datetime": {None},
    "number": {None},
}

# Some stock schemas inject a `type` that is the entity KIND, not our flavor
# key (datetime.time_schema defaults CONF_TYPE to "TIME"). Normalize those to
# None so suppression matches our typeless convention. Deprecated sensor-type
# aliases (single source: sensor platform) normalize to their replacement so an
# explicit old-name declaration suppresses the replacement's auto twin instead
# of double-spawning it.
_TYPE_NORMALIZE = {
    "sensor": dict(_sensor_platform.DEPRECATED_SENSOR_TYPES),
}

_modules = {}
_uniq = itertools.count()


def _platform_module(domain):
    mod = _modules.get(domain)
    if mod is None:
        import importlib

        mod = importlib.import_module(f".{domain}", __package__)
        _modules[domain] = mod
    return mod


def _explicit(domain):
    """Explicit infinitesp declarations in CORE.config: returns
    (zone_scoped_keys, system_types) for suppression checks."""
    zone_keys = set()
    system_types = set()
    for blk in CORE.config.get(domain) or []:
        if not isinstance(blk, dict) or blk.get("platform") != "infinitesp":
            continue
        stype = blk.get("type")
        stype = _TYPE_NORMALIZE.get(domain, {}).get(str(stype).lower() if stype else None, stype)
        # Multi-device variants (manufacture_date with device_address for the
        # IDU/ODU) are distinct entities: only a declaration of the DEFAULT
        # variant (no device_address) suppresses the auto spawn.
        if stype == "manufacture_date" and "device_address" in blk:
            continue
        if stype in _ZONE_SCOPED.get(domain, set()):
            zone_keys.add((stype, blk.get(CONF_ZONE, 0)))
        else:
            system_types.add(stype)
    return zone_keys, system_types


def _suppressed(domain, stype, zone):
    zone_keys, system_types = _explicit(domain)
    if stype in _ZONE_SCOPED.get(domain, set()):
        return (stype, zone) in zone_keys
    return stype in system_types


async def _spawn(domain, stype, zone, cfg):
    mod = _platform_module(domain)
    # Explicit unique id: anonymous ID(None) declarations all hash equal, so
    # the second one would collide in the codegen variable registry.
    tag = (stype or domain).replace("_", "")
    cfg["id"] = f"infinitesp_auto_{tag}_{zone}_{next(_uniq)}"
    cfg = mod.CONFIG_SCHEMA(cfg)
    # Component-backed entities (the datetime time entity registers a loop)
    # need register_component, which validates against CORE.component_ids —
    # a set populated only during yaml validation. Spawned ids are created
    # after that pass, so register them here. Generic: keyed on the declared
    # class actually inheriting Component.
    id_obj = cfg.get("id")
    if hasattr(id_obj, "type") and getattr(id_obj.type, "inherits_from", None):
        from esphome import codegen as _cg

        if id_obj.type.inherits_from(_cg.Component):
            CORE.component_ids.add(id_obj.id)
    await mod.to_code(cfg)


def zone_prefix(climate_name, zone):
    """entity name prefix: the climate block's name minus a trailing
    'Climate', else 'Zone N'."""
    if climate_name:
        for suffix in (" Climate", " climate"):
            if climate_name.endswith(suffix):
                return climate_name[: -len(suffix)]
        return climate_name
    return f"Zone {zone}"


def entity_name(comp, zone, prefix):
    if comp["type"] == "comfort_profile":
        # Reference naming: zone 1 keeps the unnumbered name.
        return "Comfort Profiles" if zone == 1 else f"Comfort Profiles Zone {zone}"
    return f"{prefix} {comp['label']}"


def _first_time_id():
    for blk in CORE.config.get("time") or []:
        if isinstance(blk, dict) and "id" in blk:
            return blk["id"]
    return None


async def spawn_zone_entities(climate_config):
    """Called from the climate platform's to_code: one entity set per zone."""
    zone = climate_config[CONF_ZONE]
    prefix = zone_prefix(climate_config.get(CONF_NAME), zone)
    hub_id = climate_config["infinitesp_id"]
    for comp in PER_ZONE_ENTITIES:
        if not climate_config.get(comp["flag"], True):
            continue
        if _suppressed(comp["domain"], comp["type"], zone):
            continue
        cfg = {
            CONF_NAME: entity_name(comp, zone, prefix),
            "infinitesp_id": hub_id,
        }
        if comp["type"] is not None:
            cfg["type"] = comp["type"]
        if comp["domain"] in ("sensor", "binary_sensor", "text_sensor", "select", "cover", "datetime", "number"):
            cfg[CONF_ZONE] = zone
        if comp["domain"] == "cover":
            cfg["device_class"] = "damper"
        await _spawn(comp["domain"], comp["type"], zone, cfg)


async def spawn_system_entities(hub_config):
    """Called from the hub's to_code: system-wide entity set."""
    hub_id = hub_config["id"]
    groups = [(SYSTEM_CORE, False, False)]
    if hub_config.get("auto_diagnostics", True):
        groups.append((SYSTEM_DIAGNOSTIC, True, False))
    groups.append((SYSTEM_CONDITIONAL, False, True))

    time_id = _first_time_id()
    for entries, diagnostic, conditional in groups:
        for domain, stype, name in entries:
            if _suppressed(domain, stype, 0):
                continue
            cfg = {
                CONF_NAME: name,
                "infinitesp_id": hub_id,
            }
            if stype is not None:
                cfg["type"] = stype
            if domain in ("sensor", "binary_sensor", "text_sensor", "select"):
                cfg[CONF_ZONE] = 1  # schema default; system types ignore it
            if diagnostic:
                cfg["entity_category"] = "diagnostic"
            if conditional:
                cfg["disabled_by_default"] = True
            if stype == "fault_timestamp" and time_id is not None:
                cfg["time_id"] = time_id
            await _spawn(domain, stype, 0, cfg)


ZONE_ENTITY_FLAGS = {c["flag"]: cv.boolean for c in PER_ZONE_ENTITIES}
