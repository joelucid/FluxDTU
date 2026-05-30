<template>
    <BasePage :title="$t('powerlimiteradmin.PowerLimiterSettings')" :isLoading="dataLoading">
        <BootstrapAlert
            v-model="showAlert"
            dismissible
            :variant="alertType"
            :auto-dismiss="alertType != 'success' ? 0 : 5000"
        >
            {{ alertMessage }}
        </BootstrapAlert>

        <BootstrapAlert v-model="configAlert" variant="warning">
            {{ $t('powerlimiteradmin.ConfigAlertMessage') }}
        </BootstrapAlert>

        <CardElement
            :text="$t('powerlimiteradmin.ConfigHints')"
            textVariant="text-bg-primary"
            v-if="getConfigHints().length"
        >
            <div class="row">
                <div class="col-sm-12">
                    {{ $t('powerlimiteradmin.ConfigHintsIntro') }}
                    <ul class="mb-0">
                        <li v-for="(hint, idx) in getConfigHints()" :key="idx">
                            <b v-if="hint.severity === 'requirement'"
                                >{{ $t('powerlimiteradmin.ConfigHintRequirement') }}:</b
                            >
                            <b v-if="hint.severity === 'optional'">{{ $t('powerlimiteradmin.ConfigHintOptional') }}:</b>
                            {{ $t('powerlimiteradmin.ConfigHint' + hint.subject) }}
                        </li>
                    </ul>
                </div>
            </div>
        </CardElement>

        <form @submit="savePowerLimiterConfig" v-if="!configAlert">
            <nav :class="{ 'mt-5': getConfigHints().length > 0 }">
                <div class="nav nav-tabs" id="power-limiter-settings-tab" role="tablist">
                    <button
                        class="nav-link active"
                        id="power-limiter-general-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#power-limiter-general"
                        type="button"
                        role="tab"
                        aria-controls="power-limiter-general"
                        aria-selected="true"
                    >
                        {{ $t('powerlimiteradmin.SettingsTabGeneral') }}
                    </button>
                    <button
                        v-if="isEnabled"
                        class="nav-link"
                        id="power-limiter-inverters-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#power-limiter-inverters"
                        type="button"
                        role="tab"
                        aria-controls="power-limiter-inverters"
                    >
                        {{ $t('powerlimiteradmin.SettingsTabInverters') }}
                    </button>
                    <button
                        v-if="isEnabled && governingBatteryPoweredInverters"
                        class="nav-link"
                        id="power-limiter-battery-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#power-limiter-battery"
                        type="button"
                        role="tab"
                        aria-controls="power-limiter-battery"
                    >
                        {{ $t('powerlimiteradmin.SettingsTabBattery') }}
                    </button>
                    <button
                        v-if="isEnabled && (canUseVoltageThresholds || canUseSoCThresholds)"
                        class="nav-link"
                        id="power-limiter-thresholds-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#power-limiter-thresholds"
                        type="button"
                        role="tab"
                        aria-controls="power-limiter-thresholds"
                    >
                        {{ $t('powerlimiteradmin.SettingsTabThresholds') }}
                    </button>
                    <button
                        v-if="isEnabled"
                        class="nav-link"
                        id="power-limiter-flexible-loads-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#power-limiter-flexible-loads"
                        type="button"
                        role="tab"
                        aria-controls="power-limiter-flexible-loads"
                    >
                        {{ $t('powerlimiteradmin.SettingsTabFlexibleLoads') }}
                    </button>
                </div>
            </nav>

            <div class="tab-content pt-3" id="power-limiter-settings-tabContent">
                <div
                    class="tab-pane fade show active"
                    id="power-limiter-general"
                    role="tabpanel"
                    aria-labelledby="power-limiter-general-tab"
                    tabindex="0"
                >
                    <CardElement :text="$t('powerlimiteradmin.General')" textVariant="text-bg-primary">
                        <InputElement
                            :label="$t('powerlimiteradmin.Enable')"
                            v-model="powerLimiterConfigList.enabled"
                            type="checkbox"
                            wide
                        />

                        <template v-if="powerLimiterConfigList.enabled">
                            <InputElement
                                v-for="(inv, idx) in powerLimiterConfigList.inverters"
                                :key="idx"
                                :label="$t('powerlimiteradmin.GovernInverter', { name: inverterName(inv.serial) })"
                                v-model="inv.is_governed"
                                type="checkbox"
                                wide
                            />
                        </template>

                        <template v-if="isEnabled">
                            <InputElement
                                v-if="hasPowerMeter"
                                :label="$t('powerlimiteradmin.TargetPowerConsumption')"
                                :tooltip="$t('powerlimiteradmin.TargetPowerConsumptionHint')"
                                v-model="powerLimiterConfigList.target_power_consumption"
                                postfix="W"
                                type="number"
                                :disabled="powerLimiterConfigList.target_power_consumption_follow_storage_target"
                                wide
                            />

                            <InputElement
                                v-if="hasPowerMeter"
                                :label="$t('powerlimiteradmin.TargetPowerConsumptionFollowStorageTarget')"
                                :tooltip="$t('powerlimiteradmin.TargetPowerConsumptionFollowStorageTargetHint')"
                                v-model="powerLimiterConfigList.target_power_consumption_follow_storage_target"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                v-if="
                                    hasPowerMeter &&
                                    powerLimiterConfigList.target_power_consumption_follow_storage_target
                                "
                                :label="$t('powerlimiteradmin.TargetPowerConsumptionStorageOffset')"
                                :tooltip="$t('powerlimiteradmin.TargetPowerConsumptionStorageOffsetHint')"
                                v-model="powerLimiterConfigList.target_power_consumption_storage_offset"
                                postfix="W"
                                type="number"
                                min="0"
                                max="32767"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.BaseLoadLimit')"
                                :tooltip="$t('powerlimiteradmin.BaseLoadLimitHint')"
                                v-model="powerLimiterConfigList.base_load_limit"
                                placeholder="200"
                                postfix="W"
                                type="number"
                                min="0"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.TargetPowerConsumptionHysteresis')"
                                :tooltip="$t('powerlimiteradmin.TargetPowerConsumptionHysteresisHint')"
                                v-model="powerLimiterConfigList.target_power_consumption_hysteresis"
                                postfix="W"
                                type="number"
                                min="1"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.TotalUpperPowerLimit')"
                                :tooltip="$t('powerlimiteradmin.TotalUpperPowerLimitHint')"
                                v-model="powerLimiterConfigList.total_upper_power_limit"
                                postfix="W"
                                type="number"
                                min="1"
                                wide
                            />
                        </template>

                        <template
                            v-if="
                                isEnabled && (governingBatteryPoweredInverters || governingSmartBufferPoweredInverters)
                            "
                        >
                            <div class="row mb-3">
                                <label for="inverter_restart" class="col-sm-4 col-form-label">
                                    {{ $t('powerlimiteradmin.InverterRestartHour') }}
                                    <BIconInfoCircle v-tooltip :title="$t('powerlimiteradmin.InverterRestartHint')" />
                                </label>
                                <div class="col-sm-8">
                                    <select
                                        id="inverter_restart"
                                        class="form-select"
                                        v-model="powerLimiterConfigList.inverter_restart_hour"
                                    >
                                        <option v-for="hour in range(24)" :key="hour" :value="hour">
                                            {{ hour > 9 ? hour : '0' + hour }}:00
                                        </option>
                                    </select>
                                </div>
                            </div>
                        </template>
                    </CardElement>
                </div>

                <template v-if="isEnabled">
                    <div
                        class="tab-pane fade"
                        id="power-limiter-flexible-loads"
                        role="tabpanel"
                        aria-labelledby="power-limiter-flexible-loads-tab"
                        tabindex="0"
                    >
                        <CardElement :text="$t('powerlimiteradmin.FlexibleLoadSettings')" textVariant="text-bg-primary">
                            <div
                                v-for="(load, loadIdx) in powerLimiterConfigList.flexible_loads"
                                :key="loadIdx"
                                :class="{ 'border-top pt-3 mt-3': loadIdx > 0 }"
                            >
                                <h5 class="mb-3">
                                    {{
                                        load.name ||
                                        $t('powerlimiteradmin.FlexibleLoadFallbackName', { index: loadIdx + 1 })
                                    }}
                                </h5>

                                <InputElement
                                    :label="$t('powerlimiteradmin.FlexibleLoadName')"
                                    :tooltip="$t('powerlimiteradmin.FlexibleLoadNameHint')"
                                    v-model="load.name"
                                    type="text"
                                    maxlength="31"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.EnableFlexibleLoad')"
                                    :tooltip="$t('powerlimiteradmin.EnableFlexibleLoadHint')"
                                    v-model="load.enabled"
                                    type="checkbox"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.FlexibleLoadPriority')"
                                    :tooltip="$t('powerlimiteradmin.FlexibleLoadPriorityHint')"
                                    v-model="load.priority"
                                    type="number"
                                    min="1"
                                    max="4"
                                    wide
                                />

                                <h6 class="border-bottom pb-2 mb-3">
                                    {{ $t('powerlimiteradmin.FlexibleLoadSectionMeasurement') }}
                                </h6>

                                <InputElement
                                    :label="$t('powerlimiteradmin.FlexibleLoadMeasurementPowerTopic')"
                                    :tooltip="$t('powerlimiteradmin.FlexibleLoadMeasurementPowerTopicHint')"
                                    v-model="load.power_topic"
                                    type="text"
                                    maxlength="256"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.FlexibleLoadMeasurementJsonPath')"
                                    :tooltip="$t('powerlimiteradmin.FlexibleLoadMeasurementJsonPathHint')"
                                    v-model="load.power_json_path"
                                    type="text"
                                    maxlength="256"
                                    wide
                                />

                                <div class="row mb-3">
                                    <label class="col-sm-4 col-form-label">
                                        {{ $t('powerlimiteradmin.FlexibleLoadMeasurementUnit') }}
                                    </label>
                                    <div class="col-sm-8">
                                        <select class="form-select" v-model="load.power_unit">
                                            <option v-for="unit in powerUnitList" :key="unit.key" :value="unit.key">
                                                {{ unit.value }}
                                            </option>
                                        </select>
                                    </div>
                                </div>

                                <InputElement
                                    :label="$t('powerlimiteradmin.FlexibleLoadMeasurementSignInverted')"
                                    :tooltip="$t('powerlimiteradmin.FlexibleLoadMeasurementSignInvertedHint')"
                                    v-model="load.power_sign_inverted"
                                    type="checkbox"
                                    wide
                                />

                                <template v-if="load.enabled">
                                    <h6 class="border-bottom pb-2 mt-4 mb-3">
                                        {{ $t('powerlimiteradmin.FlexibleLoadSectionSwitching') }}
                                    </h6>

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMqttTopic')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadMqttTopicHint')"
                                        v-model="load.mqtt_topic"
                                        type="text"
                                        maxlength="256"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMqttOnPayload')"
                                        v-model="load.mqtt_on_payload"
                                        type="text"
                                        maxlength="64"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMqttOffPayload')"
                                        v-model="load.mqtt_off_payload"
                                        type="text"
                                        maxlength="64"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMqttRetain')"
                                        v-model="load.mqtt_retain"
                                        type="checkbox"
                                        wide
                                    />

                                    <h6 class="border-bottom pb-2 mt-4 mb-3">
                                        {{ $t('powerlimiteradmin.FlexibleLoadSectionStartConditions') }}
                                    </h6>

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStartBatterySoc')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStartBatterySocHint')"
                                        v-model="load.start_battery_soc_threshold"
                                        postfix="%"
                                        type="number"
                                        min="0"
                                        max="100"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStartOnBmsChargeCurrentLimit')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStartOnBmsChargeCurrentLimitHint')"
                                        v-model="load.start_on_bms_charge_current_limit"
                                        type="checkbox"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadBmsChargeCurrentLimitMargin')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadBmsChargeCurrentLimitMarginHint')"
                                        v-model="load.bms_charge_current_limit_margin"
                                        postfix="A"
                                        type="number"
                                        min="0"
                                        step="0.1"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStartPowerDemand')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStartPowerDemandHint')"
                                        v-model="load.start_power_demand"
                                        postfix="W"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStartDelay')"
                                        v-model="load.start_delay"
                                        postfix="s"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStartupGrace')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStartupGraceHint')"
                                        v-model="load.startup_grace"
                                        postfix="s"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <h6 class="border-bottom pb-2 mt-4 mb-3">
                                        {{ $t('powerlimiteradmin.FlexibleLoadSectionStopConditions') }}
                                    </h6>

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMinRuntime')"
                                        v-model="load.min_runtime"
                                        postfix="s"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMinOffTime')"
                                        v-model="load.min_offtime"
                                        postfix="s"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStopPowerMargin')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStopPowerMarginHint')"
                                        v-model="load.stop_power_margin"
                                        postfix="W"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStopOnGridChargerLimit')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadStopOnGridChargerLimitHint')"
                                        v-model="load.stop_on_grid_charger_limit"
                                        type="checkbox"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadStopDelay')"
                                        v-model="load.stop_delay"
                                        postfix="s"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadBatterySupportPowerThreshold')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadBatterySupportPowerThresholdHint')"
                                        v-model="load.battery_support_power_threshold"
                                        postfix="W"
                                        type="number"
                                        min="0"
                                        wide
                                    />

                                    <InputElement
                                        :label="$t('powerlimiteradmin.FlexibleLoadMaxBatterySupportEnergy')"
                                        :tooltip="$t('powerlimiteradmin.FlexibleLoadMaxBatterySupportEnergyHint')"
                                        v-model="load.max_battery_support_energy"
                                        postfix="Wh"
                                        type="number"
                                        min="0"
                                        wide
                                    />
                                </template>
                            </div>
                        </CardElement>
                    </div>

                    <div
                        class="tab-pane fade"
                        id="power-limiter-inverters"
                        role="tabpanel"
                        aria-labelledby="power-limiter-inverters-tab"
                        tabindex="0"
                    >
                        <template v-for="(inv, idx) in governedInverters" :key="idx">
                            <CardElement
                                :text="inverterLabel(inv.serial)"
                                textVariant="text-bg-primary"
                                :add-space="idx > 0"
                            >
                                <InputElement
                                    v-if="hasPowerMeter"
                                    :label="$t('powerlimiteradmin.InverterIsBehindPowerMeter')"
                                    v-model="inv.is_behind_power_meter"
                                    :tooltip="$t('powerlimiteradmin.InverterIsBehindPowerMeterHint')"
                                    type="checkbox"
                                    wide
                                />

                                <div class="row mb-3">
                                    <label class="col-sm-4 col-form-label">
                                        {{ $t('powerlimiteradmin.PowerSource') }}
                                    </label>
                                    <div class="col-sm-8">
                                        <select class="form-select" v-model="inv.power_source">
                                            <option
                                                v-for="provider in powerSourceList"
                                                :key="provider.key"
                                                :value="provider.key"
                                            >
                                                {{ $t(`powerlimiteradmin.PowerSource` + provider.value) }}
                                            </option>
                                        </select>
                                    </div>
                                </div>

                                <InputElement
                                    :label="$t('powerlimiteradmin.AllowStandby')"
                                    :tooltip="$t('powerlimiteradmin.AllowStandbyHint')"
                                    v-if="inv.power_source == 2 || inv.power_source == 0"
                                    v-model="inv.allow_standby"
                                    type="checkbox"
                                    wide
                                />

                                <InputElement
                                    v-if="inv.power_source != 0 && inverterSupportsOverscaling(inv.serial)"
                                    :label="$t('powerlimiteradmin.UseOverscaling')"
                                    :tooltip="$t('powerlimiteradmin.UseOverscalingHint')"
                                    v-model="inv.use_overscaling_to_compensate_shading"
                                    type="checkbox"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.LowerPowerLimit')"
                                    :tooltip="$t('powerlimiteradmin.LowerPowerLimitHint')"
                                    v-model="inv.lower_power_limit"
                                    postfix="W"
                                    type="number"
                                    wide
                                />

                                <div class="row">
                                    <div class="col-sm-4"></div>
                                    <div class="col-sm-8">
                                        <div v-if="lowerLimitWarning(inv)" class="alert alert-warning" role="alert">
                                            {{
                                                $t('powerlimiteradmin.LowerPowerLimitWarning', {
                                                    min: getLowerLimitMinimum(inv),
                                                })
                                            }}
                                        </div>
                                    </div>
                                </div>

                                <InputElement
                                    :label="$t('powerlimiteradmin.UpperPowerLimit')"
                                    v-model="inv.upper_power_limit"
                                    :tooltip="$t('powerlimiteradmin.UpperPowerLimitHint')"
                                    postfix="W"
                                    type="number"
                                    wide
                                />
                            </CardElement>
                        </template>
                    </div>

                    <div
                        v-if="governingBatteryPoweredInverters"
                        class="tab-pane fade"
                        id="power-limiter-battery"
                        role="tabpanel"
                        aria-labelledby="power-limiter-battery-tab"
                        tabindex="0"
                    >
                        <CardElement :text="$t('powerlimiteradmin.DcPowerBusSettings')" textVariant="text-bg-primary">
                            <InputElement
                                v-if="canUseSolarPassthrough"
                                :label="$t('powerlimiteradmin.EnableSolarPassthrough')"
                                :tooltip="$t('powerlimiteradmin.SolarPassthroughInfo')"
                                v-model="powerLimiterConfigList.solar_passthrough_enabled"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.BatteryDischargeAtNight')"
                                :tooltip="$t('powerlimiteradmin.BatteryDischargeAtNightHint')"
                                v-model="powerLimiterConfigList.battery_always_use_at_night"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                v-if="hasPowerMeter"
                                :label="$t('powerlimiteradmin.BatteryTargetPowerConsumption')"
                                :tooltip="$t('powerlimiteradmin.BatteryTargetPowerConsumptionHint')"
                                v-model="powerLimiterConfigList.battery_target_power_consumption"
                                postfix="W"
                                type="number"
                                wide
                            />

                            <InputElement
                                v-if="hasPowerMeter"
                                :label="$t('powerlimiteradmin.BatteryStandbyPowerMargin')"
                                :tooltip="$t('powerlimiteradmin.BatteryStandbyPowerMarginHint')"
                                v-model="powerLimiterConfigList.battery_standby_power_margin"
                                postfix="W"
                                type="number"
                                min="0"
                                max="32767"
                                wide
                            />

                            <InputElement
                                v-if="hasPowerMeter"
                                :label="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicEnabled')"
                                :tooltip="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicEnabledHint')"
                                v-model="powerLimiterConfigList.battery_target_power_consumption_dynamic_enabled"
                                type="checkbox"
                                wide
                            />

                            <template
                                v-if="
                                    hasPowerMeter &&
                                    powerLimiterConfigList.battery_target_power_consumption_dynamic_enabled
                                "
                            >
                                <InputElement
                                    :label="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicMax')"
                                    :tooltip="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicMaxHint')"
                                    v-model="powerLimiterConfigList.battery_target_power_consumption_dynamic_max"
                                    postfix="W"
                                    type="number"
                                    min="0"
                                    max="32767"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicMultiplier')"
                                    :tooltip="
                                        $t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicMultiplierHint')
                                    "
                                    v-model="powerLimiterConfigList.battery_target_power_consumption_dynamic_multiplier"
                                    postfix="x"
                                    type="number"
                                    min="0"
                                    step="0.1"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicWindow')"
                                    :tooltip="$t('powerlimiteradmin.BatteryTargetPowerConsumptionDynamicWindowHint')"
                                    v-model="powerLimiterConfigList.battery_target_power_consumption_dynamic_window"
                                    postfix="s"
                                    type="number"
                                    min="10"
                                    wide
                                />
                            </template>

                            <div class="row mb-3">
                                <label for="inverter_serial_for_dc_voltage" class="col-sm-4 col-form-label">
                                    {{ $t('powerlimiteradmin.InverterForDcVoltage') }}
                                </label>
                                <div class="col-sm-8">
                                    <select
                                        id="inverter_serial_for_dc_voltage"
                                        class="form-select"
                                        v-model="powerLimiterConfigList.inverter_serial_for_dc_voltage"
                                    >
                                        <option value="" disabled hidden selected>
                                            {{ $t('powerlimiteradmin.SelectInverter') }}
                                        </option>
                                        <option
                                            v-for="inv in governedBatteryPoweredInverters"
                                            :key="inv.serial"
                                            :value="inv.serial"
                                        >
                                            {{ inverterLabel(inv.serial) }}
                                        </option>
                                    </select>
                                </div>
                            </div>

                            <div class="row mb-3" v-if="needsChannelSelection()">
                                <label for="inverter_channel" class="col-sm-4 col-form-label">
                                    {{ $t('powerlimiteradmin.InverterChannelId') }}
                                </label>
                                <div class="col-sm-8">
                                    <select
                                        id="inverter_channel"
                                        class="form-select"
                                        v-model="powerLimiterConfigList.inverter_channel_id_for_dc_voltage"
                                    >
                                        <option
                                            v-for="channel in range(
                                                getInverterInfo(powerLimiterConfigList.inverter_serial_for_dc_voltage)
                                                    .channels
                                            )"
                                            :key="channel"
                                            :value="channel"
                                        >
                                            {{ channel + 1 }}
                                        </option>
                                    </select>
                                </div>
                            </div>

                            <InputElement
                                :label="$t('powerlimiteradmin.ConductionLosses')"
                                :tooltip="$t('powerlimiteradmin.ConductionLossesInfo')"
                                v-model="powerLimiterConfigList.conduction_losses"
                                min="0"
                                max="10"
                                postfix="%"
                                type="number"
                                wide
                            />
                        </CardElement>
                    </div>

                    <div
                        v-if="canUseVoltageThresholds || canUseSoCThresholds"
                        class="tab-pane fade"
                        id="power-limiter-thresholds"
                        role="tabpanel"
                        aria-labelledby="power-limiter-thresholds-tab"
                        tabindex="0"
                    >
                        <CardElement
                            v-if="canUseVoltageThresholds"
                            :text="$t('powerlimiteradmin.VoltageThresholds')"
                            textVariant="text-bg-primary"
                        >
                            <InputElement
                                v-if="hasBatteryInterface"
                                :label="$t('powerlimiteradmin.IgnoreSoc')"
                                :tooltip="$t('powerlimiteradmin.IgnoreSocHint')"
                                v-model="powerLimiterConfigList.ignore_soc"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.StartThreshold')"
                                v-model="powerLimiterConfigList.voltage_start_threshold"
                                placeholder="50"
                                min="16"
                                max="66"
                                postfix="V"
                                type="number"
                                step="0.01"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.StopThreshold')"
                                v-model="powerLimiterConfigList.voltage_stop_threshold"
                                placeholder="49"
                                min="16"
                                max="66"
                                postfix="V"
                                type="number"
                                step="0.01"
                                wide
                            />

                            <template v-if="isSolarPassthroughEnabled">
                                <InputElement
                                    :label="$t('powerlimiteradmin.FullSolarPassthroughStartThreshold')"
                                    :tooltip="$t('powerlimiteradmin.FullSolarPassthroughStartThresholdHint')"
                                    v-model="powerLimiterConfigList.full_solar_passthrough_start_voltage"
                                    placeholder="49"
                                    min="16"
                                    max="66"
                                    postfix="V"
                                    type="number"
                                    step="0.01"
                                    wide
                                />

                                <InputElement
                                    :label="$t('powerlimiteradmin.VoltageSolarPassthroughStopThreshold')"
                                    v-model="powerLimiterConfigList.full_solar_passthrough_stop_voltage"
                                    placeholder="49"
                                    min="16"
                                    max="66"
                                    postfix="V"
                                    type="number"
                                    step="0.01"
                                    wide
                                />
                            </template>

                            <InputElement
                                :label="$t('powerlimiteradmin.VoltageLoadCorrectionFactor')"
                                v-model="powerLimiterConfigList.voltage_load_correction_factor"
                                placeholder="0.0001"
                                postfix="1/A"
                                type="number"
                                step="0.0001"
                                wide
                            />

                            <div
                                class="alert alert-secondary"
                                role="alert"
                                v-html="$t('powerlimiteradmin.VoltageLoadCorrectionInfo')"
                            ></div>
                        </CardElement>

                        <CardElement
                            :text="$t('powerlimiteradmin.SocThresholds')"
                            textVariant="text-bg-primary"
                            add-space
                            v-if="canUseSoCThresholds"
                        >
                            <InputElement
                                :label="$t('powerlimiteradmin.StartThreshold')"
                                v-model="powerLimiterConfigList.battery_soc_start_threshold"
                                placeholder="80"
                                min="0"
                                max="100"
                                postfix="%"
                                type="number"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.StopThreshold')"
                                v-model="powerLimiterConfigList.battery_soc_stop_threshold"
                                placeholder="20"
                                min="0"
                                max="100"
                                postfix="%"
                                type="number"
                                wide
                            />

                            <InputElement
                                :label="$t('powerlimiteradmin.FullSolarPassthroughStartThreshold')"
                                :tooltip="$t('powerlimiteradmin.FullSolarPassthroughStartThresholdHint')"
                                v-model="powerLimiterConfigList.full_solar_passthrough_soc"
                                v-if="isSolarPassthroughEnabled"
                                placeholder="80"
                                min="0"
                                max="100"
                                postfix="%"
                                type="number"
                                wide
                            />

                            <div
                                class="alert alert-secondary"
                                role="alert"
                                v-html="$t('powerlimiteradmin.BatterySocInfo')"
                            ></div>
                        </CardElement>
                    </div>
                </template>
            </div>

            <FormFooter @reload="getMetaData" />
        </form>
    </BasePage>
</template>

<script lang="ts">
import { defineComponent } from 'vue';
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import { handleResponse, authHeader } from '@/utils/authentication';
import CardElement from '@/components/CardElement.vue';
import FormFooter from '@/components/FormFooter.vue';
import InputElement from '@/components/InputElement.vue';
import { BIconInfoCircle } from 'bootstrap-icons-vue';
import type {
    PowerLimiterConfig,
    PowerLimiterInverterConfig,
    PowerLimiterFlexibleLoadConfig,
    PowerLimiterMetaData,
    PowerLimiterInverterInfo,
} from '@/types/PowerLimiterConfig';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        FormFooter,
        InputElement,
        BIconInfoCircle,
    },
    data() {
        return {
            dataLoading: true,
            powerLimiterConfigList: {} as PowerLimiterConfig,
            powerLimiterMetaData: {} as PowerLimiterMetaData,
            alertMessage: '',
            alertType: 'info',
            showAlert: false,
            configAlert: false,
            powerSourceList: [
                { key: 0, value: 'Battery' },
                { key: 1, value: 'Solar' },
                { key: 2, value: 'SmartBuffer' },
            ],
            powerUnitList: [
                { key: 1, value: 'mW' },
                { key: 0, value: 'W' },
                { key: 2, value: 'kW' },
            ],
        };
    },
    created() {
        this.getMetaData();
    },
    watch: {
        governedInverters() {
            if (
                !this.governedInverters.some(
                    (inv: PowerLimiterInverterConfig) =>
                        inv.serial == this.powerLimiterConfigList.inverter_serial_for_dc_voltage
                )
            ) {
                // marks serial as invalid, selects placeholder option
                this.powerLimiterConfigList.inverter_serial_for_dc_voltage = '';
            }
        },
    },
    computed: {
        governedInverters(): PowerLimiterInverterConfig[] {
            const inverters = this.powerLimiterConfigList?.inverters || [];
            return inverters.filter((inv: PowerLimiterInverterConfig) => inv.is_governed) || [];
        },
        governedBatteryPoweredInverters(): PowerLimiterInverterConfig[] {
            return this.governedInverters.filter((inv: PowerLimiterInverterConfig) => inv.power_source == 0);
        },
        governingBatteryPoweredInverters(): boolean {
            return this.governedBatteryPoweredInverters.length > 0;
        },
        governingSmartBufferPoweredInverters(): boolean {
            return this.governedInverters.filter((inv: PowerLimiterInverterConfig) => inv.power_source == 2).length > 0;
        },
        isEnabled(): boolean {
            const cfg = this.powerLimiterConfigList;
            return cfg.enabled && this.governedInverters.length > 0;
        },
        isSolarPassthroughEnabled(): boolean {
            return (
                this.powerLimiterMetaData.charge_controller_enabled &&
                this.powerLimiterConfigList.solar_passthrough_enabled
            );
        },
        hasPowerMeter(): boolean {
            return this.powerLimiterMetaData.power_meter_enabled;
        },
        canUseSolarPassthrough(): boolean {
            const meta = this.powerLimiterMetaData;
            return meta.charge_controller_enabled && this.governingBatteryPoweredInverters;
        },
        canUseVoltageThresholds(): boolean {
            return this.governingBatteryPoweredInverters;
        },
        canUseSoCThresholds(): boolean {
            const cfg = this.powerLimiterConfigList;
            const meta = this.powerLimiterMetaData;
            return meta.battery_enabled && this.governingBatteryPoweredInverters && !cfg.ignore_soc;
        },
        hasBatteryInterface(): boolean {
            const meta = this.powerLimiterMetaData;
            return meta.battery_enabled && this.governingBatteryPoweredInverters;
        },
    },
    methods: {
        getInverterInfo(serial: string): PowerLimiterInverterInfo {
            return (
                this.powerLimiterMetaData.inverters?.find((inv: PowerLimiterInverterInfo) => inv.serial === serial) ||
                ({} as PowerLimiterInverterInfo)
            );
        },
        getConfigHints(): { severity: string; subject: string }[] {
            const meta = this.powerLimiterMetaData;
            const hints = [];

            if (meta.power_meter_enabled !== true) {
                hints.push({ severity: 'optional', subject: 'PowerMeterDisabled' });
            }

            if (typeof meta.inverters === 'undefined' || meta.inverters.length == 0) {
                hints.push({ severity: 'requirement', subject: 'NoInverter' });
                this.configAlert = true;
            } else {
                let showCommDayHint = false;
                let showCommNightHint = false;
                for (const inv of this.powerLimiterMetaData.inverters) {
                    if (
                        !this.powerLimiterConfigList.inverters.some(
                            (i: PowerLimiterInverterConfig) => i.serial == inv.serial
                        )
                    ) {
                        continue;
                    }

                    const governed = this.governedInverters.some(
                        (cfgInv: PowerLimiterInverterConfig) => cfgInv.serial === inv.serial
                    );

                    if (!governed) {
                        continue;
                    }

                    const commDayEnabled = inv.poll_enable && inv.command_enable;
                    const commNightEnabled = inv.poll_enable_night && inv.command_enable_night;
                    const solarPowered = this.governedInverters.some(
                        (cfgInv: PowerLimiterInverterConfig) =>
                            cfgInv.serial === inv.serial && cfgInv.power_source === 1
                    );

                    if (!commDayEnabled) {
                        showCommDayHint = true;
                    }
                    if (!solarPowered && !commNightEnabled) {
                        showCommNightHint = true;
                    }
                }
                if (showCommDayHint) {
                    hints.push({
                        severity: 'requirement',
                        subject: 'InverterCommunicationDay',
                    });
                }
                if (showCommNightHint) {
                    hints.push({
                        severity: 'requirement',
                        subject: 'InverterCommunicationNight',
                    });
                }
            }

            if (this.governingBatteryPoweredInverters) {
                if (!meta.charge_controller_enabled) {
                    hints.push({ severity: 'optional', subject: 'NoChargeController' });
                }

                if (!meta.battery_enabled) {
                    hints.push({ severity: 'optional', subject: 'NoBatteryInterface' });
                }
            }

            if (
                (this.powerLimiterConfigList.flexible_loads || []).some(
                    (load: PowerLimiterFlexibleLoadConfig) => load.enabled
                )
            ) {
                if (!meta.mqtt_enabled) {
                    hints.push({ severity: 'requirement', subject: 'FlexibleLoadNoMqtt' });
                }
                if (!meta.power_meter_enabled) {
                    hints.push({ severity: 'requirement', subject: 'FlexibleLoadNoPowerMeter' });
                }
                if (!meta.battery_enabled) {
                    hints.push({ severity: 'requirement', subject: 'FlexibleLoadNoBatteryInterface' });
                }
            }

            if (
                (this.powerLimiterConfigList.flexible_loads || []).some(
                    (load: PowerLimiterFlexibleLoadConfig) => load.power_topic
                ) &&
                !meta.mqtt_enabled
            ) {
                hints.push({ severity: 'requirement', subject: 'FlexibleLoadMeasurementNoMqtt' });
            }

            return hints;
        },
        range(end: number) {
            return Array.from(Array(end).keys());
        },
        inverterName(serial: string) {
            if (serial === undefined) {
                return 'undefined';
            }
            const meta = this.powerLimiterMetaData;
            if (meta === undefined) {
                return 'metadata pending';
            }
            const inv = this.getInverterInfo(serial);
            if (inv === undefined) {
                return 'not found';
            }
            return inv.name;
        },
        inverterLabel(serial: string) {
            if (serial === undefined) {
                return 'undefined';
            }
            const meta = this.powerLimiterMetaData;
            if (meta === undefined) {
                return 'metadata pending';
            }
            const inv = this.getInverterInfo(serial);
            if (inv === undefined) {
                return 'not found';
            }
            return inv.name + ' (' + inv.type + ')';
        },
        inverterSupportsOverscaling(serial: string) {
            if (serial === undefined) {
                return false;
            }
            const meta = this.powerLimiterMetaData;
            if (meta === undefined) {
                return false;
            }
            const inv = this.getInverterInfo(serial);
            if (inv === undefined) {
                return false;
            }
            return inv.pdl_supported === false;
        },
        needsChannelSelection() {
            const cfg = this.powerLimiterConfigList;

            const reset = function () {
                cfg.inverter_channel_id_for_dc_voltage = 0;
                return false;
            };

            if (!this.governingBatteryPoweredInverters) {
                return reset();
            }

            if (cfg.inverter_serial_for_dc_voltage === '') {
                return reset();
            }

            const inverter = this.getInverterInfo(cfg.inverter_serial_for_dc_voltage);
            if (cfg.inverter_channel_id_for_dc_voltage >= inverter.channels) {
                cfg.inverter_channel_id_for_dc_voltage = 0;
            }

            return inverter.channels > 1;
        },
        getLowerLimitMinimum(inv: PowerLimiterInverterConfig) {
            // we can trust that the inverter info is actually available,
            // as we only use this function in a context with a valid serial.
            return this.getInverterInfo(inv.serial).channels * 13;
        },
        lowerLimitWarning(inv: PowerLimiterInverterConfig) {
            return inv.lower_power_limit < this.getLowerLimitMinimum(inv);
        },
        getMetaData() {
            this.dataLoading = true;
            fetch('/api/powerlimiter/metadata', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.powerLimiterMetaData = data;
                    this.getConfigData();
                });
        },
        getConfigData() {
            fetch('/api/powerlimiter/config', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    data.inverters = this.tidyUpInverterConfigs(data.inverters);
                    data.flexible_loads = this.tidyUpFlexibleLoadConfigs(data.flexible_loads || data.flexible_load);
                    data.flexible_load = data.flexible_loads[0];
                    this.powerLimiterConfigList = data;
                    this.dataLoading = false;
                });
        },
        tidyUpFlexibleLoadConfigs(
            configs?: PowerLimiterFlexibleLoadConfig[] | PowerLimiterFlexibleLoadConfig
        ): PowerLimiterFlexibleLoadConfig[] {
            const existing = Array.isArray(configs) ? configs : configs ? [configs] : [];
            const defaults = [
                { name: 'Flexible Load 1', priority: 1, mqtt_topic: '', mqtt_retain: false },
                {
                    name: 'Freezer Battery',
                    priority: 2,
                    mqtt_topic: 'freezer_battery/pv_available/set',
                    mqtt_retain: true,
                },
                { name: 'Flexible Load 3', priority: 3, mqtt_topic: '', mqtt_retain: false },
                { name: 'Flexible Load 4', priority: 4, mqtt_topic: '', mqtt_retain: false },
            ];

            return defaults.map((defaultsForLoad, index) =>
                this.tidyUpFlexibleLoadConfig({
                    ...defaultsForLoad,
                    ...(existing[index] || {}),
                } as PowerLimiterFlexibleLoadConfig)
            );
        },
        tidyUpFlexibleLoadConfig(config?: PowerLimiterFlexibleLoadConfig): PowerLimiterFlexibleLoadConfig {
            return {
                name: 'Flexible Load',
                enabled: false,
                priority: 1,
                mqtt_topic: '',
                mqtt_on_payload: 'ON',
                mqtt_off_payload: 'OFF',
                mqtt_retain: false,
                power_topic: '',
                power_json_path: '',
                power_unit: 0,
                power_sign_inverted: false,
                start_battery_soc_threshold: 95,
                start_on_bms_charge_current_limit: true,
                bms_charge_current_limit_margin: 0.5,
                start_power_demand: config?.start_power_demand ?? config?.start_power_tolerance ?? 50,
                start_delay: 60,
                startup_grace: 120,
                min_runtime: 600,
                min_offtime: 300,
                stop_power_margin: 50,
                stop_on_grid_charger_limit: false,
                stop_delay: 60,
                battery_support_power_threshold: 50,
                max_battery_support_energy: 25,
                ...(config || {}),
            };
        },
        tidyUpInverterConfigs(inverters: PowerLimiterInverterConfig[]): PowerLimiterInverterConfig[] {
            const metaInverters = this.powerLimiterMetaData?.inverters || [];

            // remove power limiter inverter config if no such inverter exists
            inverters = inverters.filter((cfgInv: PowerLimiterInverterConfig) =>
                metaInverters.some((metaInv) => metaInv.serial === cfgInv.serial)
            );

            // add default power limiter inverter config for new inverters
            for (const metaInv of metaInverters) {
                const known = inverters.some((cfgInv: PowerLimiterInverterConfig) => cfgInv.serial === metaInv.serial);
                if (known) {
                    continue;
                }

                const newInv = {} as PowerLimiterInverterConfig;
                newInv.serial = metaInv.serial;
                newInv.is_governed = false;
                newInv.is_behind_power_meter = true;
                newInv.lower_power_limit = this.getLowerLimitMinimum(newInv);
                newInv.upper_power_limit = Math.max(metaInv.max_power, 300);
                newInv.power_source = 0; // battery
                newInv.use_overscaling_to_compensate_shading = false;
                newInv.allow_standby = true;
                inverters.push(newInv);
            }

            inverters = inverters.sort((a, b) => {
                const orderA = this.getInverterInfo(a.serial).order;
                const orderB = this.getInverterInfo(b.serial).order;
                return orderA < orderB ? -1 : 1;
            });

            return inverters;
        },
        savePowerLimiterConfig(e: Event) {
            e.preventDefault();

            if (this.governingBatteryPoweredInverters && !this.powerLimiterConfigList.inverter_serial_for_dc_voltage) {
                this.alertMessage = this.$t('powerlimiteradmin.SelectDcVoltageInverterWarning');
                this.alertType = 'danger';
                this.showAlert = true;
                window.scrollTo(0, 0);
                return;
            }

            const formData = new FormData();
            formData.append('data', JSON.stringify(this.powerLimiterConfigList));

            fetch('/api/powerlimiter/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.alertMessage = response.message;
                    this.alertType = response.type;
                    this.showAlert = true;
                    window.scrollTo(0, 0);
                });
        },
    },
});
</script>
