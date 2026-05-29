export interface PowerLimiterInverterInfo {
    serial: string;
    pos: number;
    order: number;
    name: string;
    poll_enable: boolean;
    poll_enable_night: boolean;
    command_enable: boolean;
    command_enable_night: boolean;
    max_power: number;
    type: string;
    channels: number;
    pdl_supported: boolean;
}

// meta-data not directly part of the DPL settings,
// to control visibility of DPL settings
export interface PowerLimiterMetaData {
    power_meter_enabled: boolean;
    battery_enabled: boolean;
    charge_controller_enabled: boolean;
    mqtt_enabled: boolean;
    inverters: PowerLimiterInverterInfo[];
}

export interface PowerLimiterInverterConfig {
    serial: string;
    is_governed: boolean;
    is_behind_power_meter: boolean;
    power_source: number;
    use_overscaling_to_compensate_shading: boolean;
    allow_standby: boolean;
    lower_power_limit: number;
    upper_power_limit: number;
}

export interface PowerLimiterFlexibleLoadConfig {
    name: string;
    enabled: boolean;
    priority: number;
    mqtt_topic: string;
    mqtt_on_payload: string;
    mqtt_off_payload: string;
    mqtt_retain: boolean;
    power_topic: string;
    power_json_path: string;
    power_unit: number;
    power_sign_inverted: boolean;
    start_battery_soc_threshold: number;
    start_on_bms_charge_current_limit: boolean;
    bms_charge_current_limit_margin: number;
    start_power_demand: number;
    start_power_tolerance?: number;
    start_delay: number;
    startup_grace: number;
    min_runtime: number;
    min_offtime: number;
    stop_power_margin: number;
    stop_delay: number;
    stop_on_grid_charger_limit: boolean;
    battery_support_power_threshold: number;
    max_battery_support_energy: number;
}

export interface PowerLimiterConfig {
    enabled: boolean;
    solar_passthrough_enabled: boolean;
    conduction_losses: number;
    battery_always_use_at_night: boolean;
    target_power_consumption: number;
    target_power_consumption_follow_storage_target: boolean;
    target_power_consumption_storage_offset: number;
    battery_target_power_consumption: number;
    battery_standby_power_margin: number;
    battery_target_power_consumption_dynamic_enabled: boolean;
    battery_target_power_consumption_dynamic_max: number;
    battery_target_power_consumption_dynamic_multiplier: number;
    battery_target_power_consumption_dynamic_window: number;
    target_power_consumption_hysteresis: number;
    base_load_limit: number;
    ignore_soc: boolean;
    battery_soc_start_threshold: number;
    battery_soc_stop_threshold: number;
    voltage_start_threshold: number;
    voltage_stop_threshold: number;
    voltage_load_correction_factor: number;
    inverter_restart_hour: number;
    full_solar_passthrough_soc: number;
    full_solar_passthrough_start_voltage: number;
    full_solar_passthrough_stop_voltage: number;
    inverter_serial_for_dc_voltage: string;
    inverter_channel_id_for_dc_voltage: number;
    restart_hour: number;
    total_upper_power_limit: number;
    flexible_load?: PowerLimiterFlexibleLoadConfig;
    flexible_loads: PowerLimiterFlexibleLoadConfig[];
    inverters: PowerLimiterInverterConfig[];
}
