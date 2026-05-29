<template>
    <BasePage
        class="live-view-palette"
        :title="$t('home.LiveData')"
        :isLoading="dataLoading"
        :isWideScreen="true"
        :showWebSocket="true"
        :isWebsocketConnected="isWebsocketConnected"
        @reload="reloadData"
    >
        <HintView :hints="liveData.hints" />
        <InverterTotalInfo
            :totalData="liveData.total"
            :hasInverters="hasInverters"
            :solarChargerData="liveData.solarcharger"
            :totalBattData="liveData.battery"
            :powerMeterData="liveData.power_meter"
            :powerLimiterData="liveData.powerlimiter"
            :gridChargerData="liveData.gridcharger"
        />
        <FlexibleLoadInfo :loads="visibleFlexibleLoads" @enabled-change="onFlexibleLoadEnabledChange" />
        <div v-if="hasInverters" class="row gy-3 mt-0">
            <div class="tab-content col-sm-12 col-md-12">
                <div class="card">
                    <div class="card-header">
                        <div class="p-1 flex-grow-1">
                            <div class="d-flex flex-wrap">
                                <div style="padding-right: 2em">{{ $t('menu.InverterSettings') }}</div>
                            </div>
                        </div>
                    </div>

                    <div class="card-body">
                        <div class="row gy-3">
                            <div
                                class="col-sm-3 col-md-2"
                                :style="[inverterData.length == 1 ? { display: 'none' } : {}]"
                            >
                                <div
                                    class="nav nav-pills row-cols-sm-1 gap-3"
                                    id="v-pills-tab"
                                    role="tablist"
                                    aria-orientation="vertical"
                                >
                                    <button
                                        v-for="inverter in inverterData"
                                        :key="inverter.serial"
                                        class="nav-link border border-primary text-break"
                                        :id="'v-pills-' + inverter.serial + '-tab'"
                                        data-bs-toggle="pill"
                                        :data-bs-target="'#v-pills-' + inverter.serial"
                                        type="button"
                                        role="tab"
                                        aria-controls="'v-pills-' + inverter.serial"
                                        aria-selected="true"
                                    >
                                        <div class="d-flex align-items-center">
                                            <div class="me-2">
                                                <span
                                                    v-if="inverter.AC"
                                                    class="badge"
                                                    :class="{
                                                        'text-bg-secondary': !inverter.poll_enabled,
                                                        'text-bg-danger': inverter.poll_enabled && !inverter.reachable,
                                                        'text-bg-warning':
                                                            inverter.poll_enabled &&
                                                            inverter.reachable &&
                                                            !inverter.producing,
                                                        'text-bg-success':
                                                            inverter.poll_enabled &&
                                                            inverter.reachable &&
                                                            inverter.producing,
                                                    }"
                                                >
                                                    {{ $n(inverter.AC[0]?.Power?.v || 0, 'decimalNoDigits') }}
                                                    {{ inverter.AC[0]?.Power?.u }}
                                                </span>
                                                <span v-else class="badge text-bg-light">-</span>
                                            </div>
                                            <div class="ms-auto me-auto">
                                                {{ inverter.name }}
                                            </div>
                                        </div>
                                    </button>
                                </div>
                            </div>

                            <div
                                class="tab-content"
                                id="v-pills-tabContent"
                                :class="{
                                    'col-sm-9 col-md-10': inverterData.length > 1,
                                    'col-sm-12 col-md-12': inverterData.length == 1,
                                }"
                            >
                                <div
                                    v-for="inverter in inverterData"
                                    :key="inverter.serial"
                                    class="tab-pane fade show"
                                    :id="'v-pills-' + inverter.serial"
                                    role="tabpanel"
                                    :aria-labelledby="'v-pills-' + inverter.serial + '-tab'"
                                    tabindex="0"
                                >
                                    <div class="card">
                                        <div
                                            class="card-header d-flex justify-content-between align-items-center"
                                            :class="{
                                                'text-bg-tertiary': !inverter.poll_enabled,
                                                'text-bg-danger': inverter.poll_enabled && !inverter.reachable,
                                                'text-bg-warning':
                                                    inverter.poll_enabled && inverter.reachable && !inverter.producing,
                                                'text-bg-success':
                                                    inverter.poll_enabled && inverter.reachable && inverter.producing,
                                            }"
                                        >
                                            <div class="p-1 flex-grow-1">
                                                <div class="d-flex flex-wrap">
                                                    <div style="padding-right: 2em">
                                                        {{ inverter.name }}
                                                    </div>
                                                    <div style="padding-right: 2em">
                                                        {{ $t('home.SerialNumber') }}{{ inverter.serial }}
                                                    </div>
                                                    <div style="padding-right: 2em">
                                                        {{ $t('home.CurrentLimit') }}:
                                                        <template v-if="inverter.limit_absolute > -1">
                                                            {{ $n(inverter.limit_absolute, 'decimalNoDigits') }} W | </template
                                                        >{{ $n(inverter.limit_relative / 100, 'percentOneDigit') }}
                                                    </div>
                                                    <div style="padding-right: 2em">
                                                        <DataAgeDisplay :data-age-ms="inverter.data_age_ms" />
                                                    </div>
                                                </div>
                                            </div>
                                            <div class="btn-toolbar p-2" role="toolbar">
                                                <div class="btn-group me-2" role="group">
                                                    <button
                                                        :disabled="!isLogged"
                                                        type="button"
                                                        class="btn btn-sm btn-danger"
                                                        @click="onShowLimitSettings(inverter.serial)"
                                                        v-tooltip
                                                        :title="$t('home.ShowSetInverterLimit')"
                                                    >
                                                        <BIconSpeedometer style="font-size: 24px" />
                                                    </button>
                                                </div>

                                                <div class="btn-group me-2" role="group">
                                                    <button
                                                        :disabled="!isLogged"
                                                        type="button"
                                                        class="btn btn-sm btn-danger"
                                                        @click="onShowPowerSettings(inverter.serial)"
                                                        v-tooltip
                                                        :title="$t('home.TurnOnOff')"
                                                    >
                                                        <BIconPower style="font-size: 24px" />
                                                    </button>
                                                </div>

                                                <div class="btn-group me-2" role="group">
                                                    <button
                                                        type="button"
                                                        class="btn btn-sm btn-info"
                                                        @click="onShowDevInfo(inverter.serial)"
                                                        v-tooltip
                                                        :title="$t('home.ShowInverterInfo')"
                                                    >
                                                        <BIconCpu style="font-size: 24px" />
                                                    </button>
                                                </div>

                                                <div class="btn-group me-2" role="group">
                                                    <button
                                                        type="button"
                                                        class="btn btn-sm btn-info"
                                                        @click="onShowGridProfile(inverter.serial)"
                                                        v-tooltip
                                                        :title="$t('home.ShowGridProfile')"
                                                    >
                                                        <BIconOutlet style="font-size: 24px" />
                                                    </button>
                                                </div>

                                                <div class="btn-group" role="group">
                                                    <button
                                                        v-if="inverter.events >= 0"
                                                        type="button"
                                                        class="btn btn-sm btn-secondary position-relative"
                                                        @click="onShowEventlog(inverter.serial)"
                                                        v-tooltip
                                                        :title="$t('home.ShowEventlog')"
                                                    >
                                                        <BIconJournalText style="font-size: 24px" />
                                                        <span
                                                            class="position-absolute top-0 start-100 translate-middle badge rounded-pill text-bg-danger"
                                                        >
                                                            {{ inverter.events }}
                                                            <span class="visually-hidden">{{
                                                                $t('home.UnreadMessages')
                                                            }}</span>
                                                        </span>
                                                    </button>
                                                </div>
                                            </div>
                                        </div>
                                        <div class="card-body">
                                            <div class="row flex-row-reverse flex-wrap-reverse g-3">
                                                <template
                                                    v-for="chanType in [
                                                        { obj: inverter.INV, name: 'INV' },
                                                        { obj: inverter.AC, name: 'AC' },
                                                        { obj: inverter.DC, name: 'DC' },
                                                    ].reverse()"
                                                >
                                                    <template v-if="chanType.obj != null">
                                                        <template
                                                            v-for="channel in Object.keys(chanType.obj)
                                                                .sort()
                                                                .reverse()
                                                                .map((x) => +x)"
                                                            :key="channel"
                                                        >
                                                            <template
                                                                v-if="
                                                                    chanType.name != 'DC' ||
                                                                    (chanType.name == 'DC' &&
                                                                        getSumIrridiation(inverter) == 0) ||
                                                                    (chanType.name == 'DC' &&
                                                                        getSumIrridiation(inverter) > 0 &&
                                                                        chanType.obj[channel]?.Irradiation?.max) ||
                                                                    0 > 0
                                                                "
                                                            >
                                                                <div class="col" v-if="chanType.obj[channel]">
                                                                    <InverterChannelInfo
                                                                        :channelData="chanType.obj[channel]"
                                                                        :channelType="chanType.name"
                                                                        :channelNumber="channel"
                                                                    />
                                                                </div>
                                                            </template>
                                                        </template>
                                                    </template>
                                                </template>
                                            </div>

                                            <BootstrapAlert class="m-3" :show="!inverter.hasOwnProperty('INV')">
                                                <div class="d-flex justify-content-center align-items-center">
                                                    <div class="spinner-border m-1" role="status">
                                                        <span class="visually-hidden">{{
                                                            $t('home.LoadingInverter')
                                                        }}</span>
                                                    </div>
                                                    <span>{{ $t('home.LoadingInverter') }}</span>
                                                </div>
                                            </BootstrapAlert>

                                            <div class="accordion mt-5" id="accordionRadioStats">
                                                <div class="accordion-item accordion-table">
                                                    <h2 class="accordion-header">
                                                        <button
                                                            class="accordion-button collapsed"
                                                            type="button"
                                                            data-bs-toggle="collapse"
                                                            data-bs-target="#collapseStats"
                                                            aria-expanded="true"
                                                            aria-controls="collapseStats"
                                                        >
                                                            <BIconBroadcast />&nbsp;{{ $t('home.RadioStats') }}
                                                        </button>
                                                    </h2>
                                                    <div
                                                        id="collapseStats"
                                                        class="accordion-collapse collapse"
                                                        data-bs-parent="#accordionRadioStats"
                                                    >
                                                        <div class="accordion-body">
                                                            <table class="table table-striped table-hover">
                                                                <tbody>
                                                                    <tr>
                                                                        <td>{{ $t('home.TxRequest') }}</td>
                                                                        <td>
                                                                            {{ $n(inverter.radio_stats.tx_request) }}
                                                                        </td>
                                                                        <td></td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>{{ $t('home.RxSuccess') }}</td>
                                                                        <td>
                                                                            {{ $n(inverter.radio_stats.rx_success) }}
                                                                        </td>
                                                                        <td>
                                                                            {{
                                                                                ratio(
                                                                                    inverter.radio_stats.rx_success,
                                                                                    inverter.radio_stats.tx_request
                                                                                )
                                                                            }}
                                                                        </td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>{{ $t('home.RxFailNothing') }}</td>
                                                                        <td>
                                                                            {{
                                                                                $n(inverter.radio_stats.rx_fail_nothing)
                                                                            }}
                                                                        </td>
                                                                        <td>
                                                                            {{
                                                                                ratio(
                                                                                    inverter.radio_stats
                                                                                        .rx_fail_nothing,
                                                                                    inverter.radio_stats.tx_request
                                                                                )
                                                                            }}
                                                                        </td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>{{ $t('home.RxFailPartial') }}</td>
                                                                        <td>
                                                                            {{
                                                                                $n(inverter.radio_stats.rx_fail_partial)
                                                                            }}
                                                                        </td>
                                                                        <td>
                                                                            {{
                                                                                ratio(
                                                                                    inverter.radio_stats
                                                                                        .rx_fail_partial,
                                                                                    inverter.radio_stats.tx_request
                                                                                )
                                                                            }}
                                                                        </td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>{{ $t('home.RxFailCorrupt') }}</td>
                                                                        <td>
                                                                            {{
                                                                                $n(inverter.radio_stats.rx_fail_corrupt)
                                                                            }}
                                                                        </td>
                                                                        <td>
                                                                            {{
                                                                                ratio(
                                                                                    inverter.radio_stats
                                                                                        .rx_fail_corrupt,
                                                                                    inverter.radio_stats.tx_request
                                                                                )
                                                                            }}
                                                                        </td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>{{ $t('home.TxReRequest') }}</td>
                                                                        <td>
                                                                            {{ $n(inverter.radio_stats.tx_re_request) }}
                                                                        </td>
                                                                        <td></td>
                                                                    </tr>
                                                                    <tr>
                                                                        <td>
                                                                            {{ $t('home.Rssi') }}
                                                                            <BIconInfoCircle
                                                                                v-tooltip
                                                                                :title="$t('home.RssiHint')"
                                                                            />
                                                                        </td>
                                                                        <td>
                                                                            {{
                                                                                $t('home.dBm', {
                                                                                    dbm: $n(inverter.radio_stats.rssi),
                                                                                })
                                                                            }}
                                                                        </td>
                                                                        <td></td>
                                                                    </tr>
                                                                </tbody>
                                                            </table>
                                                            <div class="d-flex">
                                                                <button
                                                                    :disabled="!isLogged || performRadioStatsReset"
                                                                    type="button"
                                                                    class="btn btn-danger ms-auto me-3 mt-3"
                                                                    @click="onResetRadioStats(inverter.serial)"
                                                                >
                                                                    <template v-if="!performRadioStatsReset">
                                                                        <BIconArrowCounterclockwise />&nbsp;{{
                                                                            $t('home.StatsReset')
                                                                        }}
                                                                    </template>
                                                                    <template v-else>
                                                                        <span
                                                                            class="spinner-border spinner-border-sm"
                                                                            aria-hidden="true"
                                                                        ></span>
                                                                        <span role="status"
                                                                            >&nbsp;{{ $t('home.StatsResetting') }}</span
                                                                        >
                                                                    </template>
                                                                </button>
                                                            </div>
                                                        </div>
                                                    </div>
                                                </div>
                                            </div>
                                        </div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        <SolarChargerView v-if="liveData.solarcharger.enabled" />
        <BatteryView v-if="liveData.battery.enabled" />
        <GridChargerView v-if="liveData.gridcharger.enabled" />
    </BasePage>

    <ModalDialog class="live-view-palette" modalId="eventView" :title="$t('home.EventLog')" :loading="eventLogLoading">
        <EventLog :eventLogList="eventLogList" />
    </ModalDialog>

    <ModalDialog
        class="live-view-palette"
        modalId="devInfoView"
        :title="$t('home.InverterInfo')"
        :loading="devInfoLoading"
    >
        <DevInfo :devInfoList="devInfoList" />
    </ModalDialog>

    <ModalDialog
        class="live-view-palette"
        modalId="gridProfileView"
        :title="$t('home.GridProfile')"
        :loading="gridProfileLoading"
    >
        <GridProfile :gridProfileList="gridProfileList" :gridProfileRawList="gridProfileRawList" />
    </ModalDialog>

    <ModalDialog
        class="live-view-palette"
        modalId="limitSettingView"
        :title="$t('home.LimitSettings')"
        :loading="limitSettingLoading"
    >
        <BootstrapAlert v-model="showAlertLimit" :variant="alertTypeLimit">
            {{ alertMessageLimit }}
        </BootstrapAlert>

        <div class="row mb-3">
            <label for="inputCurrentLimit" class="col-sm-3 col-form-label">{{ $t('home.CurrentLimit') }} </label>
            <div class="col-sm-4">
                <div class="input-group">
                    <input
                        type="text"
                        class="form-control"
                        id="inputCurrentLimit"
                        aria-describedby="currentLimitType"
                        v-model="currentLimitRelative"
                        disabled
                    />
                    <span class="input-group-text" id="currentLimitType">%</span>
                </div>
            </div>

            <div class="col-sm-4" v-if="currentLimitList.max_power > 0">
                <div class="input-group">
                    <input
                        type="text"
                        class="form-control"
                        id="inputCurrentLimitAbsolute"
                        aria-describedby="currentLimitTypeAbsolute"
                        v-model="currentLimitAbsolute"
                        disabled
                    />
                    <span class="input-group-text" id="currentLimitTypeAbsolute">W</span>
                </div>
            </div>
        </div>

        <div class="row mb-3 align-items-center">
            <label for="inputLastLimitSet" class="col-sm-3 col-form-label">
                {{ $t('home.LastLimitSetStatus') }}
            </label>
            <div class="col-sm-9">
                <span
                    class="badge"
                    :class="{
                        'text-bg-danger': currentLimitList.limit_set_status == 'Failure',
                        'text-bg-warning': currentLimitList.limit_set_status == 'Pending',
                        'text-bg-success': currentLimitList.limit_set_status == 'Ok',
                        'text-bg-secondary': currentLimitList.limit_set_status == 'Unknown',
                    }"
                >
                    {{ $t('home.' + currentLimitList.limit_set_status) }}
                </span>
            </div>
        </div>

        <div class="row mb-3">
            <label for="inputTargetLimit" class="col-sm-3 col-form-label">{{ $t('home.SetLimit') }}</label>
            <div class="col-sm-9">
                <div class="input-group">
                    <input
                        type="number"
                        name="inputTargetLimit"
                        class="form-control"
                        id="inputTargetLimit"
                        :min="targetLimitMin"
                        :max="targetLimitMax"
                        v-model="targetLimitList.limit_value"
                    />
                    <button
                        class="btn btn-primary dropdown-toggle"
                        type="button"
                        data-bs-toggle="dropdown"
                        aria-expanded="false"
                    >
                        {{ targetLimitTypeText }}
                    </button>
                    <ul class="dropdown-menu dropdown-menu-end">
                        <li>
                            <a class="dropdown-item" @click="onSelectType(true)" href="#">{{ $t('home.Relative') }}</a>
                        </li>
                        <li>
                            <a class="dropdown-item" @click="onSelectType(false)" href="#">{{ $t('home.Absolute') }}</a>
                        </li>
                    </ul>
                </div>
                <div
                    v-if="!targetLimitRelative"
                    class="alert alert-secondary mt-3"
                    role="alert"
                    v-html="$t('home.LimitHint')"
                ></div>
            </div>
        </div>

        <template #footer>
            <button type="button" class="btn btn-danger" @click="onSetLimitSettings(true)">
                {{ $t('home.SetPersistent') }}
            </button>

            <button type="button" class="btn btn-danger" @click="onSetLimitSettings(false)">
                {{ $t('home.SetNonPersistent') }}
            </button>
        </template>
    </ModalDialog>

    <ModalDialog
        class="live-view-palette"
        modalId="powerSettingView"
        :title="$t('home.PowerSettings')"
        :loading="powerSettingLoading"
    >
        <BootstrapAlert v-model="showAlertPower" :variant="alertTypePower">
            {{ alertMessagePower }}
        </BootstrapAlert>

        <div class="row mb-3 align-items-center">
            <label for="inputLastPowerSet" class="col col-form-label">{{ $t('home.LastPowerSetStatus') }}</label>
            <div class="col">
                <span
                    class="badge"
                    :class="{
                        'text-bg-danger': successCommandPower == 'Failure',
                        'text-bg-warning': successCommandPower == 'Pending',
                        'text-bg-success': successCommandPower == 'Ok',
                        'text-bg-secondary': successCommandPower == 'Unknown',
                    }"
                >
                    {{ $t('home.' + successCommandPower) }}
                </span>
            </div>
        </div>

        <div class="d-grid gap-2 col-6 mx-auto">
            <button type="button" class="btn btn-success" @click="onSetPowerSettings(true)">
                <BIconToggleOn class="fs-4" />&nbsp;{{ $t('home.TurnOn') }}
            </button>
            <button type="button" class="btn btn-danger" @click="onSetPowerSettings(false)">
                <BIconToggleOff class="fs-4" />&nbsp;{{ $t('home.TurnOff') }}
            </button>
            <button type="button" class="btn btn-warning" @click="onSetPowerSettings(true, true)">
                <BIconArrowCounterclockwise class="fs-4" />&nbsp;{{ $t('home.Restart') }}
            </button>
        </div>
    </ModalDialog>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import DataAgeDisplay from '@/components/DataAgeDisplay.vue';
import DevInfo from '@/components/DevInfo.vue';
import EventLog from '@/components/EventLog.vue';
import FlexibleLoadInfo from '@/components/FlexibleLoadInfo.vue';
import GridProfile from '@/components/GridProfile.vue';
import HintView from '@/components/HintView.vue';
import InverterChannelInfo from '@/components/InverterChannelInfo.vue';
import InverterTotalInfo from '@/components/InverterTotalInfo.vue';
import { LimitType } from '@/types/LimitConfig';
import ModalDialog from '@/components/ModalDialog.vue';
import SolarChargerView from '@/components/SolarChargerView.vue';
import GridChargerView from '@/components/GridChargerView.vue';
import BatteryView from '@/components/BatteryView.vue';
import type { DevInfoStatus } from '@/types/DevInfoStatus';
import type { EventlogItems } from '@/types/EventlogStatus';
import type { GridProfileRawdata } from '@/types/GridProfileRawdata';
import type { GridProfileStatus } from '@/types/GridProfileStatus';
import type { LimitConfig } from '@/types/LimitConfig';
import type { LimitStatus } from '@/types/LimitStatus';
import type { FlexibleLoad, Inverter, LiveData } from '@/types/LiveDataStatus';
import { authHeader, authUrl, handleResponse, isLoggedIn } from '@/utils/authentication';
import * as bootstrap from 'bootstrap';
import {
    BIconArrowCounterclockwise,
    BIconBroadcast,
    BIconCpu,
    BIconInfoCircle,
    BIconJournalText,
    BIconOutlet,
    BIconPower,
    BIconSpeedometer,
    BIconToggleOff,
    BIconToggleOn,
} from 'bootstrap-icons-vue';
import { defineComponent } from 'vue';
import WebSocketService from '@/utils/websocketService';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        DataAgeDisplay,
        DevInfo,
        EventLog,
        FlexibleLoadInfo,
        GridProfile,
        HintView,
        InverterChannelInfo,
        InverterTotalInfo,
        ModalDialog,
        BIconArrowCounterclockwise,
        BIconBroadcast,
        BIconCpu,
        BIconInfoCircle,
        BIconJournalText,
        BIconOutlet,
        BIconPower,
        BIconSpeedometer,
        BIconToggleOff,
        BIconToggleOn,
        SolarChargerView,
        GridChargerView,
        BatteryView,
    },
    data() {
        return {
            isLogged: isLoggedIn(),

            socket: {} as WebSocketService,
            heartInterval: 0,
            dataAgeTimers: {} as Record<string, number>,
            dataLoading: true,
            liveData: {} as LiveData,
            isFirstFetchAfterConnect: true,
            eventLogView: {} as bootstrap.Modal,
            eventLogList: {} as EventlogItems,
            eventLogLoading: true,
            devInfoView: {} as bootstrap.Modal,
            devInfoList: {} as DevInfoStatus,
            devInfoLoading: true,
            gridProfileView: {} as bootstrap.Modal,
            gridProfileList: {} as GridProfileStatus,
            gridProfileRawList: {} as GridProfileRawdata,
            gridProfileLoading: true,

            limitSettingView: {} as bootstrap.Modal,
            limitSettingLoading: true,

            currentLimitList: {} as LimitStatus,
            targetLimitList: {} as LimitConfig,

            targetLimitMin: 0,
            targetLimitMax: 100,
            targetLimitTypeText: this.$t('home.Relative'),
            targetLimitRelative: true,

            alertMessageLimit: '',
            alertTypeLimit: 'info',
            showAlertLimit: false,
            performRadioStatsReset: false,

            powerSettingView: {} as bootstrap.Modal,
            powerSettingSerial: '',
            powerSettingLoading: true,
            alertMessagePower: '',
            alertTypePower: 'info',
            showAlertPower: false,
            successCommandPower: '',

            isWebsocketConnected: false,
        };
    },
    created() {
        this.getInitialData();
        this.initSocket();
        this.$emitter.on('logged-in', () => {
            this.isLogged = this.isLoggedIn();
        });
        this.$emitter.on('logged-out', () => {
            this.isLogged = this.isLoggedIn();
        });
    },
    mounted() {
        this.eventLogView = new bootstrap.Modal('#eventView');
        this.devInfoView = new bootstrap.Modal('#devInfoView');
        this.gridProfileView = new bootstrap.Modal('#gridProfileView');
        this.limitSettingView = new bootstrap.Modal('#limitSettingView');
        this.powerSettingView = new bootstrap.Modal('#powerSettingView');
    },
    unmounted() {
        this.socket?.close();
    },
    updated() {
        console.log('Updated');
        // Select first tab
        if (this.isFirstFetchAfterConnect) {
            console.log('isFirstFetchAfterConnect');

            this.$nextTick(() => {
                console.log('nextTick');
                const firstTabEl = document.querySelector('#v-pills-tab:first-child button');
                if (firstTabEl != null) {
                    this.isFirstFetchAfterConnect = false;
                    console.log('Show');
                    const firstTab = new bootstrap.Tab(firstTabEl);
                    firstTab.show();
                }
            });
        }
    },
    computed: {
        currentLimitAbsolute(): string {
            if (this.currentLimitList.max_power > 0) {
                return this.$n(
                    (this.currentLimitList.limit_relative * this.currentLimitList.max_power) / 100,
                    'decimalNoDigits'
                );
            }
            return '0';
        },
        currentLimitRelative(): string {
            return this.$n(this.currentLimitList.limit_relative, 'decimalOneDigit');
        },
        inverterData(): Inverter[] {
            return this.liveData.inverters.slice().sort((a: Inverter, b: Inverter) => {
                return a.order - b.order;
            });
        },
        hasInverters(): boolean {
            return this.liveData?.inverters?.length > 0 || false;
        },
        visibleFlexibleLoads(): FlexibleLoad[] {
            const powerLimiterData = this.liveData?.powerlimiter;
            if (!powerLimiterData?.enabled) {
                return [];
            }

            const loads =
                powerLimiterData.flexibleLoads ||
                (powerLimiterData.flexibleLoad ? [powerLimiterData.flexibleLoad] : []);
            return loads
                .filter((load: FlexibleLoad) => load.enabled || load.configured)
                .sort((a: FlexibleLoad, b: FlexibleLoad) => {
                    const priorityA = a.priority ?? a.index + 1;
                    const priorityB = b.priority ?? b.index + 1;
                    return priorityA === priorityB ? a.index - b.index : priorityA - priorityB;
                });
        },
    },
    methods: {
        isLoggedIn,
        getInitialData(triggerLoading: boolean = true) {
            if (triggerLoading) {
                this.dataLoading = true;
            }
            fetch('/api/livedata/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.liveData = data;
                    if (triggerLoading) {
                        this.dataLoading = false;
                    }
                });
        },
        reloadData() {
            this.socket?.close();

            this.getInitialData(false);
            this.initSocket();
        },
        onFlexibleLoadEnabledChange(index: number, enabled: boolean) {
            const flexibleLoads = this.liveData.powerlimiter?.flexibleLoads;
            const flexibleLoad =
                flexibleLoads?.find((load) => load.index === index) ||
                (index === 0 ? this.liveData.powerlimiter?.flexibleLoad : undefined);
            if (!flexibleLoad) {
                return;
            }

            flexibleLoad.enabled = enabled;
            if (!enabled) {
                flexibleLoad.state = 'disabled';
                flexibleLoad.startBlockReason = 'disabled';
                flexibleLoad.startReason = 'none';
            }

            this.getInitialData(false);
        },
        handleMessage(event: MessageEvent) {
            if (!event.data || event.data === '{}') {
                this.socket?.close(); // force reconnect
                this.initSocket();
                return;
            }

            const newData = JSON.parse(event.data);

            if (typeof newData.solarcharger !== 'undefined') {
                Object.assign(this.liveData.solarcharger, newData.solarcharger);
            }
            if (typeof newData.gridcharger !== 'undefined') {
                Object.assign(this.liveData.gridcharger, newData.gridcharger);
            }
            if (typeof newData.battery !== 'undefined') {
                Object.assign(this.liveData.battery, newData.battery);
            }
            if (typeof newData.power_meter !== 'undefined') {
                Object.assign(this.liveData.power_meter, newData.power_meter);
            }
            if (typeof newData.powerlimiter !== 'undefined') {
                Object.assign(this.liveData.powerlimiter, newData.powerlimiter);
            }

            if (typeof newData.total === 'undefined') {
                return;
            }

            Object.assign(this.liveData.total, newData.total);
            Object.assign(this.liveData.hints, newData.hints);

            const idx = this.liveData.inverters.findIndex((i) => i.serial === newData.inverters[0].serial);

            if (idx == -1) {
                Object.assign(this.liveData.inverters, newData.inverters);
                this.liveData.inverters.forEach((inv) => this.resetDataAging(inv));
            } else if (this.liveData.inverters[idx]) {
                Object.assign(this.liveData.inverters[idx], newData.inverters[0]);
                this.resetDataAging(this.liveData.inverters[idx]);
            }
        },
        initSocket() {
            console.log('Starting connection to WebSocket Server');

            const { protocol, host } = location;
            const authString = authUrl();
            const webSocketUrl = `${protocol === 'https:' ? 'wss' : 'ws'}://${authString}${host}/livedata`;

            this.socket = new WebSocketService(webSocketUrl, {
                onMessage: this.handleMessage,
                onOpen: () => {
                    console.log('WebSocket connected');
                    this.isWebsocketConnected = true;
                },
                onClose: () => {
                    console.log('WebSocket closed');
                    this.isWebsocketConnected = false;
                },
            });

            // Listen to window events , When the window closes , Take the initiative to disconnect websocket Connect
            window.onbeforeunload = () => {
                this.socket?.close();
            };

            this.socket?.connect();
        },
        resetDataAging(inv: Inverter) {
            if (this.dataAgeTimers[inv.serial] !== undefined) {
                clearTimeout(this.dataAgeTimers[inv.serial]);
            }

            const nextMs = 1000 - (inv.data_age_ms % 1000);
            this.dataAgeTimers[inv.serial] = setTimeout(() => {
                this.doDataAging(inv.serial);
            }, nextMs);
        },
        doDataAging(serial: string) {
            const inv = this.liveData?.inverters?.find((inv) => inv.serial === serial);
            if (inv === undefined) {
                return;
            }

            inv.data_age_ms += 1000;

            this.dataAgeTimers[serial] = setTimeout(() => {
                this.doDataAging(serial);
            }, 1000);
        },
        onShowEventlog(serial: string) {
            this.eventLogLoading = true;
            fetch('/api/eventlog/status?inv=' + serial + '&locale=' + this.$i18n.locale, {
                headers: authHeader(),
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.eventLogList = data;
                    this.eventLogLoading = false;
                });

            this.eventLogView.show();
        },
        onShowDevInfo(serial: string) {
            this.devInfoLoading = true;
            fetch('/api/devinfo/status?inv=' + serial, { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.devInfoList = data;
                    this.devInfoList.serial = serial;
                    this.devInfoLoading = false;
                });

            this.devInfoView.show();
        },
        onShowGridProfile(serial: string) {
            this.gridProfileLoading = true;
            fetch('/api/gridprofile/status?inv=' + serial, { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.gridProfileList = data;

                    fetch('/api/gridprofile/rawdata?inv=' + serial, { headers: authHeader() })
                        .then((response) => handleResponse(response, this.$emitter, this.$router))
                        .then((data) => {
                            this.gridProfileRawList = data;
                            this.gridProfileLoading = false;
                        });
                });

            this.gridProfileView.show();
        },
        onShowLimitSettings(serial: string) {
            this.showAlertLimit = false;
            this.targetLimitList.serial = '';
            this.targetLimitList.limit_value = 0;
            this.onSelectType(true);

            this.limitSettingLoading = true;
            fetch('/api/limit/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.currentLimitList = data[serial];
                    this.targetLimitList.serial = serial;
                    this.limitSettingLoading = false;
                });

            this.limitSettingView.show();
        },
        onResetRadioStats(serial: string) {
            this.performRadioStatsReset = true;
            fetch('/api/inverter/stats_reset?inv=' + serial, { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then(() => {
                    this.performRadioStatsReset = false;
                });
        },
        onSetLimitSettings(setPersistent: boolean) {
            if (setPersistent) {
                if (this.targetLimitRelative) {
                    this.targetLimitList.limit_type = LimitType.RelativPersistent;
                } else {
                    this.targetLimitList.limit_type = LimitType.AbsolutPersistent;
                }
            } else {
                if (this.targetLimitRelative) {
                    this.targetLimitList.limit_type = LimitType.RelativNonPersistent;
                } else {
                    this.targetLimitList.limit_type = LimitType.AbsolutNonPersistent;
                }
            }
            const formData = new FormData();
            formData.append('data', JSON.stringify(this.targetLimitList));

            console.log(this.targetLimitList);

            fetch('/api/limit/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    if (response.type == 'success') {
                        this.limitSettingView.hide();
                    } else {
                        this.alertMessageLimit = this.$t('apiresponse.' + response.code, response.param);
                        this.alertTypeLimit = response.type;
                        this.showAlertLimit = true;
                    }
                });
        },
        onSelectType(isRelative: boolean) {
            if (isRelative) {
                this.targetLimitTypeText = this.$t('home.Relative');
                this.targetLimitMin = 0;
                this.targetLimitMax = 100;
            } else {
                this.targetLimitTypeText = this.$t('home.Absolute');
                this.targetLimitMin = 0;
                this.targetLimitMax = this.currentLimitList.max_power > 0 ? this.currentLimitList.max_power : 2250;
            }
            this.targetLimitRelative = isRelative;
        },

        onShowPowerSettings(serial: string) {
            this.showAlertPower = false;
            this.powerSettingSerial = '';
            this.powerSettingLoading = true;
            fetch('/api/power/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.successCommandPower = data[serial].power_set_status;
                    this.powerSettingSerial = serial;
                    this.powerSettingLoading = false;
                });
            this.powerSettingView.show();
        },

        onSetPowerSettings(turnOn: boolean, restart = false) {
            const data = restart
                ? {
                      serial: this.powerSettingSerial,
                      restart: true,
                  }
                : {
                      serial: this.powerSettingSerial,
                      power: turnOn,
                  };

            const formData = new FormData();
            formData.append('data', JSON.stringify(data));

            console.log(data);

            fetch('/api/power/config', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    if (response.type == 'success') {
                        this.powerSettingView.hide();
                    } else {
                        this.alertMessagePower = this.$t('apiresponse.' + response.code, response.param);
                        this.alertTypePower = response.type;
                        this.showAlertPower = true;
                    }
                });
        },
        getSumIrridiation(inv: Inverter): number {
            let total = 0;
            Object.keys(inv.DC).forEach((key) => {
                total += inv.DC[key as unknown as number]?.Irradiation?.max || 0;
            });
            return total;
        },
        ratio(val_small: number, val_large: number): string {
            if (val_large == 0) {
                return '-';
            }
            return this.$n(val_small / val_large, 'percent');
        },
    },
});
</script>

<style scoped>
.btn-group {
    border-radius: var(--bs-border-radius);
    margin-top: 0.25rem;
}

.live-view-palette {
    background-color: #e9ecef;
    min-height: calc(100vh - 4.5rem);
    --live-view-primary-bg: #d9eaff;
    --live-view-primary-border: #9fc7f5;
    --live-view-primary-text: #08406f;
    --live-view-info-bg: #d9f0f6;
    --live-view-info-border: #a6dbe7;
    --live-view-info-text: #075464;
    --live-view-success-bg: #d8f0df;
    --live-view-success-border: #a6d8b8;
    --live-view-success-text: #14532d;
    --live-view-warning-bg: #fff1c2;
    --live-view-warning-border: #f0cf73;
    --live-view-warning-text: #5c4300;
    --live-view-danger-bg: #f8d6da;
    --live-view-danger-border: #e7aab2;
    --live-view-danger-text: #7a1d2a;
    --live-view-secondary-bg: #e9ecef;
    --live-view-secondary-border: #c8ced3;
    --live-view-secondary-text: #394047;
}

.live-view-palette :deep(.text-bg-primary) {
    color: var(--live-view-primary-text) !important;
    background-color: var(--live-view-primary-bg) !important;
}

.live-view-palette :deep(.text-bg-info) {
    color: var(--live-view-info-text) !important;
    background-color: var(--live-view-info-bg) !important;
}

.live-view-palette :deep(.text-bg-success) {
    color: var(--live-view-success-text) !important;
    background-color: var(--live-view-success-bg) !important;
}

.live-view-palette :deep(.text-bg-warning) {
    color: var(--live-view-warning-text) !important;
    background-color: var(--live-view-warning-bg) !important;
}

.live-view-palette :deep(.text-bg-danger) {
    color: var(--live-view-danger-text) !important;
    background-color: var(--live-view-danger-bg) !important;
}

.live-view-palette :deep(.text-bg-secondary) {
    color: var(--live-view-secondary-text) !important;
    background-color: var(--live-view-secondary-bg) !important;
}

.live-view-palette :deep(.border-primary) {
    border-color: var(--live-view-primary-border) !important;
}

.live-view-palette :deep(.border-info) {
    border-color: var(--live-view-info-border) !important;
}

.live-view-palette :deep(.border-success) {
    border-color: var(--live-view-success-border) !important;
}

.live-view-palette :deep(.border-warning) {
    border-color: var(--live-view-warning-border) !important;
}

.live-view-palette :deep(.border-danger) {
    border-color: var(--live-view-danger-border) !important;
}

.live-view-palette :deep(.border-secondary) {
    border-color: var(--live-view-secondary-border) !important;
}

.live-view-palette :deep(.nav-pills) {
    --bs-nav-pills-link-active-bg: var(--live-view-primary-bg);
    --bs-nav-pills-link-active-color: var(--live-view-primary-text);
}

.live-view-palette :deep(.btn-primary) {
    --bs-btn-color: var(--live-view-primary-text);
    --bs-btn-bg: var(--live-view-primary-bg);
    --bs-btn-border-color: var(--live-view-primary-border);
    --bs-btn-hover-color: var(--live-view-primary-text);
    --bs-btn-hover-bg: #c4ddfb;
    --bs-btn-hover-border-color: #8bb8ea;
    --bs-btn-active-color: var(--live-view-primary-text);
    --bs-btn-active-bg: #b4d4f8;
    --bs-btn-active-border-color: #7dadde;
    --bs-btn-disabled-color: var(--live-view-primary-text);
    --bs-btn-disabled-bg: var(--live-view-primary-bg);
    --bs-btn-disabled-border-color: var(--live-view-primary-border);
}

.live-view-palette :deep(.btn-info) {
    --bs-btn-color: var(--live-view-info-text);
    --bs-btn-bg: var(--live-view-info-bg);
    --bs-btn-border-color: var(--live-view-info-border);
    --bs-btn-hover-color: var(--live-view-info-text);
    --bs-btn-hover-bg: #c4e7f0;
    --bs-btn-hover-border-color: #91cedd;
    --bs-btn-active-color: var(--live-view-info-text);
    --bs-btn-active-bg: #b5dfeb;
    --bs-btn-active-border-color: #82c4d3;
}

.live-view-palette :deep(.btn-success) {
    --bs-btn-color: var(--live-view-success-text);
    --bs-btn-bg: var(--live-view-success-bg);
    --bs-btn-border-color: var(--live-view-success-border);
    --bs-btn-hover-color: var(--live-view-success-text);
    --bs-btn-hover-bg: #c4e5ce;
    --bs-btn-hover-border-color: #91c7a6;
    --bs-btn-active-color: var(--live-view-success-text);
    --bs-btn-active-bg: #b6ddc3;
    --bs-btn-active-border-color: #82bd99;
}

.live-view-palette :deep(.btn-warning) {
    --bs-btn-color: var(--live-view-warning-text);
    --bs-btn-bg: var(--live-view-warning-bg);
    --bs-btn-border-color: var(--live-view-warning-border);
    --bs-btn-hover-color: var(--live-view-warning-text);
    --bs-btn-hover-bg: #ffe9a6;
    --bs-btn-hover-border-color: #e4bf5e;
    --bs-btn-active-color: var(--live-view-warning-text);
    --bs-btn-active-bg: #ffe294;
    --bs-btn-active-border-color: #d9b44f;
}

.live-view-palette :deep(.btn-danger) {
    --bs-btn-color: var(--live-view-danger-text);
    --bs-btn-bg: var(--live-view-danger-bg);
    --bs-btn-border-color: var(--live-view-danger-border);
    --bs-btn-hover-color: var(--live-view-danger-text);
    --bs-btn-hover-bg: #f0c2c8;
    --bs-btn-hover-border-color: #dc98a2;
    --bs-btn-active-color: var(--live-view-danger-text);
    --bs-btn-active-bg: #eab4bc;
    --bs-btn-active-border-color: #cf8994;
}

.live-view-palette :deep(.btn-secondary) {
    --bs-btn-color: var(--live-view-secondary-text);
    --bs-btn-bg: var(--live-view-secondary-bg);
    --bs-btn-border-color: var(--live-view-secondary-border);
    --bs-btn-hover-color: var(--live-view-secondary-text);
    --bs-btn-hover-bg: #dde2e6;
    --bs-btn-hover-border-color: #b7c0c8;
    --bs-btn-active-color: var(--live-view-secondary-text);
    --bs-btn-active-bg: #d2d9df;
    --bs-btn-active-border-color: #acb6bf;
}
</style>
