export interface GridChargerTruckiConfig {
    ip_address: string;
    password: string;
}

export interface GridChargerCanConfig {
    hardware_interface: number;
    controller_frequency: number;
}

export interface GridChargerHuaweiConfig {
    offline_voltage: number;
    offline_current: number;
    input_current_limit: number;
    fan_online_full_speed: boolean;
    fan_offline_full_speed: boolean;
}

export interface GridChargerConfig {
    enabled: boolean;
    provider: number;
    auto_power_enabled: boolean;
    auto_power_batterysoc_limits_enabled: boolean;
    voltage_limit: number;
    enable_voltage_limit: number;
    lower_power_limit: number;
    upper_power_limit: number;
    emergency_charge_enabled: boolean;
    stop_batterysoc_threshold: number;
    auto_power_soc_planning_enabled: boolean;
    auto_power_soc_planning_day_min_soc: number;
    auto_power_soc_planning_night_target_soc: number;
    auto_power_soc_planning_start_after_sunrise: number;
    auto_power_soc_planning_finish_before_sunset: number;
    auto_power_soc_planning_battery_capacity: number;
    auto_power_soc_planning_power_limit_enabled: boolean;
    auto_power_soc_planning_minimum_power_limit: number;
    auto_power_soc_planning_catch_up_multiplier: number;
    target_power_consumption: number;
    target_power_consumption_dynamic_enabled: boolean;
    target_power_consumption_dynamic_max: number;
    target_power_consumption_dynamic_multiplier: number;
    target_power_consumption_dynamic_window: number;
    power_limiter_managed: boolean;
    can: GridChargerCanConfig;
    huawei: GridChargerHuaweiConfig;
    trucki: GridChargerTruckiConfig;
}
