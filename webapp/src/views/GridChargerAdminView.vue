<template>
    <BasePage :title="$t('gridchargeradmin.ChargerSettings')" :isLoading="dataLoading">
        <BootstrapAlert
            v-model="showAlert"
            dismissible
            :variant="alertType"
            :auto-dismiss="alertType != 'success' ? 0 : 5000"
        >
            {{ alertMessage }}
        </BootstrapAlert>

        <form @submit="saveChargerConfig">
            <nav>
                <div class="nav nav-tabs" id="grid-charger-settings-tab" role="tablist">
                    <button
                        class="nav-link active"
                        id="grid-charger-configuration-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#grid-charger-configuration"
                        type="button"
                        role="tab"
                        aria-controls="grid-charger-configuration"
                        aria-selected="true"
                    >
                        {{ $t('gridchargeradmin.Configuration') }}
                    </button>
                    <button
                        v-if="showHuaweiSettings"
                        class="nav-link"
                        id="grid-charger-huawei-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#grid-charger-huawei"
                        type="button"
                        role="tab"
                        aria-controls="grid-charger-huawei"
                    >
                        {{ $t('gridchargeradmin.HuaweiSettings') }}
                    </button>
                    <button
                        v-if="showLimits"
                        class="nav-link"
                        id="grid-charger-limits-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#grid-charger-limits"
                        type="button"
                        role="tab"
                        aria-controls="grid-charger-limits"
                    >
                        {{ $t('gridchargeradmin.Limits') }}
                    </button>
                    <button
                        v-if="showBatterySoCLimits"
                        class="nav-link"
                        id="grid-charger-battery-soc-tab"
                        data-bs-toggle="tab"
                        data-bs-target="#grid-charger-battery-soc"
                        type="button"
                        role="tab"
                        aria-controls="grid-charger-battery-soc"
                    >
                        {{ $t('gridchargeradmin.BatterySoCLimits') }}
                    </button>
                </div>
            </nav>

            <div class="tab-content pt-3" id="grid-charger-settings-tabContent">
                <div
                    class="tab-pane fade show active"
                    id="grid-charger-configuration"
                    role="tabpanel"
                    aria-labelledby="grid-charger-configuration-tab"
                    tabindex="0"
                >
                    <CardElement :text="$t('gridchargeradmin.Configuration')" textVariant="text-bg-primary">
                        <InputElement
                            :label="$t('gridchargeradmin.EnableGridCharger')"
                            v-model="gridChargerConfigList.enabled"
                            type="checkbox"
                            wide
                        />

                        <template v-if="gridChargerConfigList.enabled">
                            <div class="row mb-3">
                                <label class="col-sm-4 col-form-label">
                                    {{ $t('gridchargeradmin.Provider') }}
                                </label>
                                <div class="col-sm-8">
                                    <select class="form-select" v-model="gridChargerConfigList.provider">
                                        <option
                                            v-for="provider in providerTypeList"
                                            :key="provider.key"
                                            :value="provider.key"
                                        >
                                            {{ $t(`gridchargeradmin.Provider` + provider.value) }}
                                        </option>
                                    </select>
                                </div>
                            </div>

                            <template v-if="gridChargerConfigList.provider === 0">
                                <div class="row mb-3">
                                    <label class="col-sm-4 col-form-label">
                                        {{ $t('gridchargeradmin.HardwareInterface') }}
                                    </label>
                                    <div class="col-sm-8">
                                        <select
                                            class="form-select"
                                            v-model="gridChargerConfigList.can.hardware_interface"
                                        >
                                            <option
                                                v-for="type in hardwareInterfaceList"
                                                :key="type.key"
                                                :value="type.key"
                                            >
                                                {{ $t('gridchargeradmin.HardwareInterface' + type.value) }}
                                            </option>
                                        </select>
                                    </div>
                                </div>

                                <div class="row mb-3" v-if="gridChargerConfigList.can.hardware_interface === 0">
                                    <label class="col-sm-4 col-form-label">
                                        {{ $t('gridchargeradmin.CanControllerFrequency') }}
                                    </label>
                                    <div class="col-sm-8">
                                        <select
                                            class="form-select"
                                            v-model="gridChargerConfigList.can.controller_frequency"
                                        >
                                            <option
                                                v-for="frequency in frequencyTypeList"
                                                :key="frequency.key"
                                                :value="frequency.value"
                                            >
                                                {{ frequency.key }} MHz
                                            </option>
                                        </select>
                                    </div>
                                </div>
                            </template>

                            <template v-if="gridChargerConfigList.provider === 1">
                                <InputElement
                                    :label="$t('gridchargeradmin.IpAddress')"
                                    v-model="gridChargerConfigList.trucki.ip_address"
                                    type="text"
                                    pattern="\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b"
                                    maxlength="15"
                                    wide
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.Password')"
                                    :tooltip="$t('gridchargeradmin.PasswordHint')"
                                    v-model="gridChargerConfigList.trucki.password"
                                    type="password"
                                    maxlength="64"
                                    wide
                                />
                            </template>

                            <InputElement
                                :label="$t('gridchargeradmin.EnableAutoPower')"
                                v-model="gridChargerConfigList.auto_power_enabled"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                v-if="gridChargerConfigList.auto_power_enabled"
                                :label="$t('gridchargeradmin.EnableBatterySoCLimits')"
                                v-model="gridChargerConfigList.auto_power_batterysoc_limits_enabled"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                v-if="gridChargerConfigList.auto_power_enabled && gridChargerConfigList.provider === 0"
                                :label="$t('gridchargeradmin.IgnoreBmsCurrent')"
                                :tooltip="$t('gridchargeradmin.IgnoreBmsCurrentHint')"
                                v-model="gridChargerConfigList.auto_power_ignore_bms_current"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                v-if="gridChargerConfigList.auto_power_enabled && gridChargerConfigList.provider === 0"
                                :label="$t('gridchargeradmin.BmsChargeCurrentMargin')"
                                :tooltip="$t('gridchargeradmin.BmsChargeCurrentMarginHint')"
                                v-model="gridChargerConfigList.auto_power_bms_charge_current_margin"
                                postfix="A"
                                type="number"
                                wide
                                required
                                step="0.1"
                                min="0"
                                max="5"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.EnableEmergencyCharge')"
                                :tooltip="$t('gridchargeradmin.EnableEmergencyChargeHint')"
                                v-model="gridChargerConfigList.emergency_charge_enabled"
                                type="checkbox"
                                wide
                            />
                        </template>
                    </CardElement>
                </div>

                <template v-if="showHuaweiSettings">
                    <div
                        class="tab-pane fade"
                        id="grid-charger-huawei"
                        role="tabpanel"
                        aria-labelledby="grid-charger-huawei-tab"
                        tabindex="0"
                    >
                        <CardElement :text="$t('gridchargeradmin.HuaweiSettings')" textVariant="text-bg-primary">
                            <InputElement
                                :label="$t('gridchargeradmin.OfflineVoltage')"
                                v-model="gridChargerConfigList.huawei.offline_voltage"
                                postfix="V"
                                type="number"
                                wide
                                step="0.01"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.OfflineCurrent')"
                                v-model="gridChargerConfigList.huawei.offline_current"
                                postfix="A"
                                type="number"
                                wide
                                step="0.1"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.InputCurrentLimit')"
                                v-model="gridChargerConfigList.huawei.input_current_limit"
                                postfix="A"
                                type="number"
                                wide
                                step="0.1"
                                :tooltip="$t('gridchargeradmin.InputCurrentLimitHint')"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.FanOnlineFullSpeed')"
                                v-model="gridChargerConfigList.huawei.fan_online_full_speed"
                                type="checkbox"
                                wide
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.FanOfflineFullSpeed')"
                                v-model="gridChargerConfigList.huawei.fan_offline_full_speed"
                                type="checkbox"
                                wide
                            />
                        </CardElement>
                    </div>
                </template>

                <template v-if="showLimits">
                    <div
                        class="tab-pane fade"
                        id="grid-charger-limits"
                        role="tabpanel"
                        aria-labelledby="grid-charger-limits-tab"
                        tabindex="0"
                    >
                        <CardElement :text="$t('gridchargeradmin.Limits')" textVariant="text-bg-primary">
                            <InputElement
                                :label="$t('gridchargeradmin.VoltageLimit')"
                                :tooltip="$t('gridchargeradmin.stopVoltageLimitHint')"
                                v-if="gridChargerConfigList.provider === 0"
                                v-model="gridChargerConfigList.voltage_limit"
                                postfix="V"
                                type="number"
                                wide
                                required
                                step="0.01"
                                min="42"
                                max="58.5"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.enableVoltageLimit')"
                                :tooltip="$t('gridchargeradmin.enableVoltageLimitHint')"
                                v-model="gridChargerConfigList.enable_voltage_limit"
                                v-if="gridChargerConfigList.auto_power_enabled && gridChargerConfigList.provider === 0"
                                postfix="V"
                                type="number"
                                wide
                                required
                                step="0.01"
                                min="42"
                                max="58.5"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.lowerPowerLimit')"
                                v-model="gridChargerConfigList.lower_power_limit"
                                v-if="gridChargerConfigList.auto_power_enabled && gridChargerConfigList.provider === 0"
                                postfix="W"
                                type="number"
                                wide
                                required
                                min="5"
                                max="4000"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.upperPowerLimit')"
                                :tooltip="$t('gridchargeradmin.upperPowerLimitHint')"
                                v-model="gridChargerConfigList.upper_power_limit"
                                v-if="gridChargerConfigList.provider === 0"
                                postfix="W"
                                type="number"
                                wide
                                required
                                min="100"
                                max="4000"
                            />
                        </CardElement>
                    </div>
                </template>

                <template v-if="showBatterySoCLimits">
                    <div
                        class="tab-pane fade"
                        id="grid-charger-battery-soc"
                        role="tabpanel"
                        aria-labelledby="grid-charger-battery-soc-tab"
                        tabindex="0"
                    >
                        <CardElement :text="$t('gridchargeradmin.BatterySoCLimits')" textVariant="text-bg-primary">
                            <InputElement
                                :label="$t('gridchargeradmin.StopBatterySoCThreshold')"
                                :tooltip="$t('gridchargeradmin.StopBatterySoCThresholdHint')"
                                v-model="gridChargerConfigList.stop_batterysoc_threshold"
                                postfix="%"
                                type="number"
                                wide
                                required
                                min="2"
                                max="101"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.ReenableBatterySoCThreshold')"
                                :tooltip="$t('gridchargeradmin.ReenableBatterySoCThresholdHint')"
                                v-model="gridChargerConfigList.reenable_batterysoc_threshold"
                                postfix="%"
                                type="number"
                                wide
                                required
                                min="0"
                                max="100"
                            />

                            <InputElement
                                :label="$t('gridchargeradmin.EnableSocPlanning')"
                                :tooltip="$t('gridchargeradmin.EnableSocPlanningHint')"
                                v-model="gridChargerConfigList.auto_power_soc_planning_enabled"
                                type="checkbox"
                                wide
                            />

                            <template v-if="gridChargerConfigList.auto_power_soc_planning_enabled">
                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningDayMinSoc')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningDayMinSocHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_day_min_soc"
                                    postfix="%"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="101"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningStartAfterSunrise')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningStartAfterSunriseHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_start_after_sunrise"
                                    postfix="min"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="720"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningIntermediateTargetSoc')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningIntermediateTargetSocHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_intermediate_target_soc"
                                    postfix="%"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="101"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningIntermediateBeforeSunset')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningIntermediateBeforeSunsetHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_intermediate_before_sunset"
                                    postfix="min"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="720"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningNightTargetSoc')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningNightTargetSocHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_night_target_soc"
                                    postfix="%"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="101"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningFinalRampStartBeforeSunset')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningFinalRampStartBeforeSunsetHint')"
                                    v-model="
                                        gridChargerConfigList.auto_power_soc_planning_final_ramp_start_before_sunset
                                    "
                                    postfix="min"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="720"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.SocPlanningFinishBeforeSunset')"
                                    :tooltip="$t('gridchargeradmin.SocPlanningFinishBeforeSunsetHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_finish_before_sunset"
                                    postfix="min"
                                    type="number"
                                    wide
                                    required
                                    min="0"
                                    max="720"
                                />

                                <InputElement
                                    :label="$t('gridchargeradmin.EnableSocPlanningPowerLimit')"
                                    :tooltip="$t('gridchargeradmin.EnableSocPlanningPowerLimitHint')"
                                    v-model="gridChargerConfigList.auto_power_soc_planning_power_limit_enabled"
                                    type="checkbox"
                                    wide
                                />

                                <template v-if="gridChargerConfigList.auto_power_soc_planning_power_limit_enabled">
                                    <InputElement
                                        :label="$t('gridchargeradmin.SocPlanningBatteryCapacity')"
                                        :tooltip="$t('gridchargeradmin.SocPlanningBatteryCapacityHint')"
                                        v-model="gridChargerConfigList.auto_power_soc_planning_battery_capacity"
                                        postfix="Wh"
                                        type="number"
                                        wide
                                        required
                                        min="1"
                                        max="100000"
                                    />

                                    <InputElement
                                        :label="$t('gridchargeradmin.SocPlanningMinimumPowerLimit')"
                                        :tooltip="$t('gridchargeradmin.SocPlanningMinimumPowerLimitHint')"
                                        v-model="gridChargerConfigList.auto_power_soc_planning_minimum_power_limit"
                                        postfix="W"
                                        type="number"
                                        wide
                                        min="0"
                                        max="4000"
                                    />

                                    <InputElement
                                        :label="$t('gridchargeradmin.SocPlanningCatchUpMultiplier')"
                                        :tooltip="$t('gridchargeradmin.SocPlanningCatchUpMultiplierHint')"
                                        v-model="gridChargerConfigList.auto_power_soc_planning_catch_up_multiplier"
                                        postfix="x"
                                        type="number"
                                        wide
                                        required
                                        min="0"
                                        max="10"
                                        step="0.1"
                                    />
                                </template>
                            </template>
                        </CardElement>
                    </div>
                </template>
            </div>

            <FormFooter @reload="getChargerConfig" />
        </form>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import CardElement from '@/components/CardElement.vue';
import FormFooter from '@/components/FormFooter.vue';
import InputElement from '@/components/InputElement.vue';
import type { GridChargerConfig } from '@/types/GridChargerConfig';
import { authHeader, handleResponse } from '@/utils/authentication';
import { defineComponent } from 'vue';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        FormFooter,
        InputElement,
    },
    data() {
        return {
            dataLoading: true,
            gridChargerConfigList: {} as GridChargerConfig,
            alertMessage: '',
            alertType: 'info',
            showAlert: false,
            providerTypeList: [
                { key: 0, value: 'Huawei' },
                { key: 1, value: 'Trucki' },
            ],
            frequencyTypeList: [
                { key: 8, value: 8000000 },
                { key: 16, value: 16000000 },
            ],
            hardwareInterfaceList: [
                { key: 0, value: 'MCP2515' },
                { key: 1, value: 'TWAI' },
            ],
        };
    },
    created() {
        this.getChargerConfig();
    },
    computed: {
        showHuaweiSettings() {
            return this.gridChargerConfigList.enabled && this.gridChargerConfigList.provider === 0;
        },
        showLimits() {
            return (
                this.gridChargerConfigList.enabled &&
                (this.gridChargerConfigList.auto_power_enabled ||
                    (this.gridChargerConfigList.emergency_charge_enabled && this.gridChargerConfigList.provider === 0))
            );
        },
        showBatterySoCLimits() {
            return (
                this.gridChargerConfigList.enabled &&
                this.gridChargerConfigList.auto_power_enabled &&
                this.gridChargerConfigList.auto_power_batterysoc_limits_enabled
            );
        },
    },
    methods: {
        getChargerConfig() {
            this.dataLoading = true;
            fetch('/api/gridcharger/config', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.gridChargerConfigList = data;
                    this.dataLoading = false;
                });
        },
        saveChargerConfig(e: Event) {
            e.preventDefault();

            const formData = new FormData();
            formData.append('data', JSON.stringify(this.gridChargerConfigList));

            fetch('/api/gridcharger/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.alertMessage = this.$t('onbatteryapiresponse.' + response.code, response.param);
                    this.alertType = response.type;
                    this.showAlert = true;
                    window.scrollTo(0, 0);
                });
        },
    },
});
</script>
