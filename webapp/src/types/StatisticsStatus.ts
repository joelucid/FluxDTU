export interface StatisticsSummary {
    sample_count: number;
    interval_count: number;
    solar_energy_wh: number;
    battery_charge_wh: number;
    battery_discharge_wh: number;
    grid_import_wh: number;
    grid_export_wh: number;
    grid_charger_energy_wh: number;
    flexible_load_energy_wh: number;
    flexible_load_energies_wh?: number[];
    target_deviation_wh: number;
    grid_balance_wh: number;
    max_grid_import_w: number;
    max_grid_export_w: number;
    max_battery_charge_w: number;
    max_battery_discharge_w: number;
    battery_boost_savings_25a_wh: number;
    battery_boost_savings_50a_wh: number;
    battery_boost_extra_wh: number;
    battery_boost_relax_limited_wh: number;
    battery_boost_seconds: number;
    avg_battery_soc?: number;
    min_battery_soc?: number;
    max_battery_soc?: number;
    min_battery_voltage?: number;
    max_battery_voltage?: number;
    min_battery_temperature?: number;
    max_battery_temperature?: number;
}

export interface StatisticsSample {
    t: number;
    battery_discharge_power_w: number;
    solar_power_w: number;
    battery_power_w: number;
    grid_power_w: number;
    grid_import_power_w: number;
    grid_export_power_w: number;
    grid_charger_power_w: number;
    battery_boost_savings_25a_power_w: number;
    battery_boost_savings_50a_power_w: number;
    battery_boost_extra_power_w: number;
    battery_boost_relax_limited_power_w: number;
    battery_boost_seconds: number;
    battery_boost_budget_used: number;
    flexible_load_power_w: number;
    flexible_load_powers_w?: number[];
    target_power_w: number;
    battery_soc?: number;
    battery_planned_soc?: number;
    inverter_temperatures?: Record<string, number>;
    panel_powers_w?: (number | null)[];
}

export interface StatisticsDay {
    t: number;
    solar_energy_wh: number;
    panel_energies_wh?: (number | null)[];
    battery_charge_wh: number;
    battery_discharge_wh: number;
    grid_import_wh: number;
    grid_export_wh: number;
    grid_charger_energy_wh: number;
    battery_boost_savings_25a_wh: number;
    battery_boost_savings_50a_wh: number;
    battery_boost_extra_wh: number;
    battery_boost_relax_limited_wh: number;
    battery_boost_seconds: number;
    flexible_load_energy_wh: number;
    flexible_load_energies_wh?: number[];
    avg_battery_soc?: number;
    min_battery_soc?: number;
    max_battery_soc?: number;
}

export interface StatisticsStatus {
    period: string;
    from: number;
    to: number;
    sample_interval_seconds: number;
    recent_retention_days: number;
    daily_retention_days: number;
    resolution_seconds: number;
    sample_capacity: number;
    recent_samples: number;
    recent_from?: number;
    recent_to?: number;
    summary: StatisticsSummary;
    battery_boost_estimator?: StatisticsBatteryBoostEstimator;
    flexible_loads?: StatisticsFlexibleLoad[];
    inverters?: StatisticsInverter[];
    panels?: StatisticsPanel[];
    sample_fields?: string[];
    samples?: StatisticsSample[];
    days?: StatisticsDay[];
}

export interface StatisticsFlexibleLoad {
    index: number;
    name: string;
    enabled: boolean;
    energy_wh: number;
}

export interface StatisticsInverter {
    index: number;
    serial: string;
    name: string;
    enabled: boolean;
    order: number;
}

export interface StatisticsPanel {
    index: number;
    inverter_index: number;
    channel: number;
    channel_number: number;
    serial: string;
    inverter: string;
    name: string;
    label: string;
    enabled: boolean;
    order: number;
    max_power: number;
    energy_wh: number;
}

export interface StatisticsBatteryBoostEstimator {
    normal_current_a: number;
    max_current_a: number;
    boost_budget_seconds: number;
    cooldown_seconds: number;
    inverter_efficiency: number;
    ac_limit_w: number;
}
