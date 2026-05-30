<template>
    <BasePage
        class="statistics-page"
        :title="$t('statistics.Statistics')"
        :isLoading="dataLoading"
        :isWideScreen="true"
        showReload
        @reload="reloadData"
    >
        <div class="statistics-toolbar mb-2">
            <div class="d-flex flex-wrap gap-2" role="group">
                <button
                    v-for="period in periods"
                    :key="period.value"
                    type="button"
                    class="btn"
                    :class="selectedPeriod === period.value ? 'btn-primary' : 'btn-outline-primary'"
                    @click="setPeriod(period.value)"
                >
                    {{ $t(period.label) }}
                </button>
            </div>

            <div class="range-controls">
                <button
                    type="button"
                    class="btn btn-outline-secondary btn-sm icon-button"
                    :title="$t('statistics.PreviousRange')"
                    :aria-label="$t('statistics.PreviousRange')"
                    @click="shiftRange(-1)"
                >
                    <BIconChevronLeft />
                </button>
                <div class="input-group input-group-sm range-date-input">
                    <span class="input-group-text">
                        <BIconCalendar3 />
                        <span class="ms-1">{{ $t('statistics.Start') }}</span>
                    </span>
                    <input
                        v-model="rangeStartInput"
                        type="date"
                        class="form-control"
                        :min="rangeStartMinDate"
                        :max="rangeStartMaxDate"
                        @change="setRangeStartFromInput"
                    />
                </div>
                <button
                    type="button"
                    class="btn btn-outline-secondary btn-sm icon-button"
                    :title="$t('statistics.NextRange')"
                    :aria-label="$t('statistics.NextRange')"
                    :disabled="isLatestRange"
                    @click="shiftRange(1)"
                >
                    <BIconChevronRight />
                </button>
                <button
                    type="button"
                    class="btn btn-outline-secondary btn-sm"
                    :disabled="isLatestRange"
                    @click="resetToLatestRange"
                >
                    {{ $t('statistics.Latest') }}
                </button>
            </div>
        </div>
        <div v-if="status" class="statistics-range-label text-muted small mb-3">
            {{ visibleRangeLabel }}
        </div>

        <div class="row g-3 mb-3">
            <div class="col-6 col-xl" v-for="card in kpiCards" :key="card.label">
                <div class="card statistic-card h-100">
                    <div class="card-body">
                        <div class="text-muted small">{{ card.label }}</div>
                        <div class="fs-4 fw-semibold">{{ card.value }}</div>
                    </div>
                </div>
            </div>
        </div>

        <div class="card chart-panel-card">
            <div class="card-header pb-0">
                <ul class="nav nav-tabs card-header-tabs flex-nowrap overflow-auto" role="tablist">
                    <li v-for="tab in chartTabs" :key="tab.value" class="nav-item" role="presentation">
                        <button
                            type="button"
                            class="nav-link text-nowrap"
                            :class="{ active: selectedChartTab === tab.value }"
                            :id="`statistics-${tab.value}-tab`"
                            role="tab"
                            :aria-controls="`statistics-${tab.value}-panel`"
                            :aria-selected="selectedChartTab === tab.value"
                            @click="setChartTab(tab.value)"
                        >
                            {{ tab.label }}
                        </button>
                    </li>
                </ul>
            </div>
            <div class="card-body">
                <div
                    v-if="selectedChartTab === 'flow'"
                    class="chart-tab-panel"
                    id="statistics-flow-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-flow-tab"
                >
                    <div
                        class="chart-host"
                        :class="{ 'chart-host-dragging': rangeDrag.active }"
                        @pointerdown="startRangeDrag"
                        @pointermove="moveRangeDrag"
                        @pointerup="endRangeDrag"
                        @pointercancel="cancelRangeDragPointer"
                    >
                        <canvas ref="flowChart"></canvas>
                    </div>
                </div>

                <div
                    v-if="selectedChartTab === 'panels'"
                    class="chart-tab-panel chart-tab-panel-with-controls"
                    id="statistics-panels-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-panels-tab"
                >
                    <div class="panel-chart-controls">
                        <div
                            class="btn-group btn-group-sm panel-mode-toggle"
                            role="group"
                            :aria-label="$t('statistics.Panels')"
                        >
                            <input
                                id="statistics-panel-mode-inverters"
                                v-model="panelDisplayMode"
                                class="btn-check"
                                type="radio"
                                value="inverters"
                                autocomplete="off"
                                @change="renderActiveChart"
                            />
                            <label class="btn btn-outline-primary" for="statistics-panel-mode-inverters">
                                {{ $t('statistics.PanelModeInverters') }}
                            </label>
                            <input
                                id="statistics-panel-mode-strings"
                                v-model="panelDisplayMode"
                                class="btn-check"
                                type="radio"
                                value="strings"
                                autocomplete="off"
                                @change="renderActiveChart"
                            />
                            <label class="btn btn-outline-primary" for="statistics-panel-mode-strings">
                                {{ $t('statistics.PanelModeStrings') }}
                            </label>
                        </div>
                        <div class="panel-inverter-toggle-group" role="group" :aria-label="$t('statistics.Inverter')">
                            <template v-for="option in panelInverterOptions" :key="option.value">
                                <input
                                    :id="`statistics-panel-inverter-${option.value}`"
                                    v-model="selectedPanelInverters"
                                    class="btn-check"
                                    type="checkbox"
                                    :value="option.value"
                                    autocomplete="off"
                                    @change="onPanelSelectionChanged"
                                />
                                <label
                                    class="btn btn-sm btn-outline-secondary panel-inverter-toggle"
                                    :for="`statistics-panel-inverter-${option.value}`"
                                >
                                    {{ option.label }}
                                </label>
                            </template>
                        </div>
                    </div>
                    <div
                        class="chart-host"
                        :class="{ 'chart-host-dragging': rangeDrag.active }"
                        @pointerdown="startRangeDrag"
                        @pointermove="moveRangeDrag"
                        @pointerup="endRangeDrag"
                        @pointercancel="cancelRangeDragPointer"
                    >
                        <canvas ref="panelChart"></canvas>
                    </div>
                </div>

                <div
                    v-if="selectedChartTab === 'loads'"
                    class="chart-tab-panel"
                    id="statistics-loads-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-loads-tab"
                >
                    <div
                        class="chart-host"
                        :class="{ 'chart-host-dragging': rangeDrag.active }"
                        @pointerdown="startRangeDrag"
                        @pointermove="moveRangeDrag"
                        @pointerup="endRangeDrag"
                        @pointercancel="cancelRangeDragPointer"
                    >
                        <canvas ref="loadChart"></canvas>
                    </div>
                </div>

                <div
                    v-if="selectedChartTab === 'temperatures'"
                    class="chart-tab-panel"
                    id="statistics-temperatures-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-temperatures-tab"
                >
                    <div
                        class="chart-host"
                        :class="{ 'chart-host-dragging': rangeDrag.active }"
                        @pointerdown="startRangeDrag"
                        @pointermove="moveRangeDrag"
                        @pointerup="endRangeDrag"
                        @pointercancel="cancelRangeDragPointer"
                    >
                        <canvas ref="temperatureChart"></canvas>
                    </div>
                </div>

                <div
                    v-if="selectedChartTab === 'boost'"
                    class="chart-tab-panel"
                    id="statistics-boost-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-boost-tab"
                >
                    <div class="row g-3 align-items-stretch h-100">
                        <div class="col-12 col-xl-9">
                            <div
                                class="chart-host"
                                :class="{ 'chart-host-dragging': rangeDrag.active }"
                                @pointerdown="startRangeDrag"
                                @pointermove="moveRangeDrag"
                                @pointerup="endRangeDrag"
                                @pointercancel="cancelRangeDragPointer"
                            >
                                <canvas ref="boostChart"></canvas>
                            </div>
                        </div>
                        <div class="col-12 col-xl-3">
                            <div class="battery-metrics h-100">
                                <div class="metric-line">
                                    <span>{{ $t('statistics.GridImport') }}</span>
                                    <strong>{{ formatWh(status?.summary.grid_import_wh || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.BoostSavings25A') }}</span>
                                    <strong>{{ formatWh(status?.summary.battery_boost_savings_25a_wh || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.BoostSavings50A') }}</span>
                                    <strong>{{ formatWh(status?.summary.battery_boost_savings_50a_wh || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.BoostExtra') }}</span>
                                    <strong>{{ formatWh(status?.summary.battery_boost_extra_wh || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.BoostRelaxLimited') }}</span>
                                    <strong>{{ formatWh(status?.summary.battery_boost_relax_limited_wh || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.BoostTime') }}</span>
                                    <strong>{{ formatDuration(status?.summary.battery_boost_seconds || 0) }}</strong>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <div
                    v-if="selectedChartTab === 'battery'"
                    class="chart-tab-panel"
                    id="statistics-battery-panel"
                    role="tabpanel"
                    aria-labelledby="statistics-battery-tab"
                >
                    <div class="row g-3 align-items-stretch h-100">
                        <div class="col-12 col-xl-9">
                            <div
                                class="chart-host chart-host-battery"
                                :class="{ 'chart-host-dragging': rangeDrag.active }"
                                @pointerdown="startRangeDrag"
                                @pointermove="moveRangeDrag"
                                @pointerup="endRangeDrag"
                                @pointercancel="cancelRangeDragPointer"
                            >
                                <canvas ref="batteryChart"></canvas>
                            </div>
                        </div>
                        <div class="col-12 col-xl-3">
                            <div class="battery-metrics h-100">
                                <div class="metric-line">
                                    <span>{{ $t('statistics.AvgSoc') }}</span>
                                    <strong>{{ formatOptionalPercent(status?.summary.avg_battery_soc) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.MinSoc') }}</span>
                                    <strong>{{ formatOptionalPercent(status?.summary.min_battery_soc) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.MaxSoc') }}</span>
                                    <strong>{{ formatOptionalPercent(status?.summary.max_battery_soc) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.ChargePeak') }}</span>
                                    <strong>{{ formatWatt(status?.summary.max_battery_charge_w || 0) }}</strong>
                                </div>
                                <div class="metric-line">
                                    <span>{{ $t('statistics.DischargePeak') }}</span>
                                    <strong>{{ formatWatt(status?.summary.max_battery_discharge_w || 0) }}</strong>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </BasePage>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import { BIconCalendar3, BIconChevronLeft, BIconChevronRight } from 'bootstrap-icons-vue';
import type {
    StatisticsDay,
    StatisticsFlexibleLoad,
    StatisticsInverter,
    StatisticsPanel,
    StatisticsSample,
    StatisticsStatus,
    StatisticsSummary,
} from '@/types/StatisticsStatus';
import { authHeader, handleResponse } from '@/utils/authentication';
import {
    BarController,
    BarElement,
    CategoryScale,
    Chart as ChartJS,
    Filler,
    Legend,
    LineController,
    LineElement,
    LinearScale,
    PointElement,
    Tooltip,
    type ChartConfiguration,
    type ChartDataset,
    type Plugin,
    type TooltipItem,
} from 'chart.js';
import { defineComponent } from 'vue';

type StatisticsDragChart = ChartJS & { $statisticsRangeDragOffsetPx?: number };
const clippedCharts = new WeakSet<object>();
const statisticsRangeDragPlugin: Plugin = {
    id: 'statisticsRangeDrag',
    beforeDatasetsDraw(chart) {
        const offset = (chart as StatisticsDragChart).$statisticsRangeDragOffsetPx || 0;
        const { ctx, chartArea } = chart;

        ctx.save();
        ctx.beginPath();
        ctx.rect(chartArea.left, chartArea.top, chartArea.right - chartArea.left, chartArea.bottom - chartArea.top);
        ctx.clip();
        if (Math.abs(offset) >= 0.1) {
            ctx.translate(offset, 0);
        }
        clippedCharts.add(chart);
    },
    afterDatasetsDraw(chart) {
        if (!clippedCharts.has(chart)) {
            return;
        }

        chart.ctx.restore();
        clippedCharts.delete(chart);
    },
};

ChartJS.register(
    BarController,
    BarElement,
    CategoryScale,
    Filler,
    Legend,
    LineController,
    LineElement,
    LinearScale,
    PointElement,
    Tooltip,
    statisticsRangeDragPlugin
);

interface PeriodOption {
    value: string;
    label: string;
}

type ChartTab = 'flow' | 'panels' | 'loads' | 'temperatures' | 'boost' | 'battery';
type PanelDisplayMode = 'inverters' | 'strings';

interface ChartTabOption {
    value: ChartTab;
    label: string;
}

interface FlowStackPoint {
    t: number;
    consumption: number;
    gridImportShare: number;
    gridExportShare: number;
    batteryShare: number;
    solarShare: number;
    acChargerShare: number;
}

type EnergyStackPoint = FlowStackPoint;

interface LoadPoint {
    t: number;
    flexibleValues: number[];
    other: number;
}

interface PanelPoint {
    t: number;
    values: (number | null)[];
}

interface PanelSeries {
    key: string;
    label: string;
    positions: number[];
}

interface BoostPoint {
    t: number;
    gridImport: number;
    savings25A: number;
    boostExtra: number;
    remainingImport: number;
    relaxLimited: number;
}

interface ManagedChart {
    destroy(): void;
    draw(): void;
    resize(): void;
}

export default defineComponent({
    components: {
        BasePage,
        BIconCalendar3,
        BIconChevronLeft,
        BIconChevronRight,
    },
    data() {
        return {
            dataLoading: true,
            selectedPeriod: 'today',
            customRangeStart: null as number | null,
            rangeStartInput: '',
            rangeDrag: {
                active: false,
                pointerId: null as number | null,
                startX: 0,
                currentX: 0,
                width: 0,
                baseStart: 0,
                lastAppliedStart: null as number | null,
            },
            status: null as StatisticsStatus | null,
            statusCache: {} as Record<string, StatisticsStatus>,
            pendingStatusRequests: {} as Record<string, Promise<StatisticsStatus>>,
            requestSerial: 0,
            prefetchTimer: null as number | null,
            prefetchInFlight: false as boolean,
            rangeDragFetchTimer: null as number | null,
            rangeDragFetchInFlight: false,
            rangeDragQueuedStart: null as number | null,
            rangeDragFinalizeAfterApply: false as boolean,
            rangeDragEnding: false as boolean,
            rangeDragElement: null as HTMLElement | null,
            rangeDragDrawFrame: null as number | null,
            rangeDragLastMoveAt: 0,
            rangeDragStatusApplyTimer: null as number | null,
            rangeDragPendingStatus: null as { status: StatisticsStatus; rangeStart: number } | null,
            rangeDragSession: 0,
            flowChart: null as ManagedChart | null,
            panelChart: null as ManagedChart | null,
            loadChart: null as ManagedChart | null,
            temperatureChart: null as ManagedChart | null,
            boostChart: null as ManagedChart | null,
            batteryChart: null as ManagedChart | null,
            selectedChartTab: 'flow' as ChartTab,
            panelDisplayMode: 'inverters' as PanelDisplayMode,
            selectedPanelInverters: [] as string[],
            panelFilterInitialized: false,
            periods: [
                { value: 'today', label: 'statistics.Today' },
                { value: '24h', label: 'statistics.Last24Hours' },
                { value: '7d', label: 'statistics.Last7Days' },
                { value: '30d', label: 'statistics.Last30Days' },
                { value: '365d', label: 'statistics.Last365Days' },
            ] as PeriodOption[],
        };
    },
    created() {
        this.getData();
    },
    mounted() {
        this.renderCharts();
    },
    beforeUnmount() {
        this.cancelRangeDrag();
        if (this.prefetchTimer !== null) {
            window.clearTimeout(this.prefetchTimer);
            this.prefetchTimer = null;
        }
        if (this.rangeDragFetchTimer !== null) {
            window.clearTimeout(this.rangeDragFetchTimer);
            this.rangeDragFetchTimer = null;
        }
        if (this.rangeDragStatusApplyTimer !== null) {
            window.clearTimeout(this.rangeDragStatusApplyTimer);
            this.rangeDragStatusApplyTimer = null;
        }
        if (this.rangeDragDrawFrame !== null) {
            window.cancelAnimationFrame(this.rangeDragDrawFrame);
            this.rangeDragDrawFrame = null;
        }
        this.rangeDragQueuedStart = null;
        this.rangeDragPendingStatus = null;
        this.destroyCharts();
    },
    computed: {
        summary(): StatisticsSummary | null {
            return this.status?.summary || null;
        },
        isDailyPeriod(): boolean {
            return this.selectedPeriod === '30d' || this.selectedPeriod === '365d';
        },
        periodDurationSeconds(): number {
            if (this.selectedPeriod === '7d') {
                return 7 * 24 * 60 * 60;
            }
            if (this.selectedPeriod === '30d') {
                return 30 * 24 * 60 * 60;
            }
            if (this.selectedPeriod === '365d') {
                return 365 * 24 * 60 * 60;
            }
            return 24 * 60 * 60;
        },
        rangeStartMinDate(): string | undefined {
            return this.status?.recent_from ? this.dateInputFromTimestamp(this.status.recent_from) : undefined;
        },
        rangeStartMaxDate(): string {
            return this.dateInputFromTimestamp(this.latestSelectableRangeStart());
        },
        isLatestRange(): boolean {
            return this.customRangeStart === null || this.customRangeStart >= this.latestSelectableRangeStart();
        },
        visibleRangeLabel(): string {
            if (!this.status) {
                return '';
            }
            return `${this.formatTooltipTime(this.status.from)} - ${this.formatTooltipTime(this.status.to)}`;
        },
        chartTitle(): string {
            return this.isDailyPeriod ? this.$t('statistics.DailyEnergy') : this.$t('statistics.PowerHistory');
        },
        chartTabs(): ChartTabOption[] {
            const tabs: ChartTabOption[] = [{ value: 'flow', label: this.chartTitle }];
            if (this.hasLoadData) {
                tabs.push({ value: 'loads', label: this.$t('statistics.Loads') });
            }
            if (this.hasPanelData) {
                tabs.push({ value: 'panels', label: this.$t('statistics.Panels') });
            }
            tabs.push({ value: 'battery', label: this.$t('statistics.Battery') });
            if (this.hasTemperatureData) {
                tabs.push({ value: 'temperatures', label: this.$t('statistics.Temperatures') });
            }
            tabs.push({ value: 'boost', label: this.$t('statistics.Boost') });
            return tabs;
        },
        samples(): StatisticsSample[] {
            return this.status?.samples || [];
        },
        plotSamples(): StatisticsSample[] {
            if (this.isDailyPeriod) {
                return this.samples;
            }

            const status = this.status;
            if (!status) {
                return [];
            }

            const byTimestamp = new Map<number, StatisticsSample>();
            const minTimestamp = Math.max(0, status.from - this.rangeBufferSpanSeconds());
            const maxTimestamp = status.to + this.rangeBufferSpanSeconds();

            const addSamples = (candidate: StatisticsStatus, includeVisibleRange: boolean) => {
                (candidate.samples || []).forEach((sample) => {
                    if (sample.t < minTimestamp || sample.t > maxTimestamp) {
                        return;
                    }
                    if (!includeVisibleRange && sample.t >= status.from && sample.t <= status.to) {
                        return;
                    }
                    byTimestamp.set(sample.t, sample);
                });
            };

            addSamples(status, true);
            this.plotStatuses().forEach((candidate) => {
                if (candidate === status) {
                    return;
                }
                addSamples(candidate, false);
            });

            return Array.from(byTimestamp.values()).sort((a, b) => a.t - b.t);
        },
        temperatureSamples(): StatisticsSample[] {
            return this.samples.filter(
                (sample) => sample.inverter_temperatures && Object.keys(sample.inverter_temperatures).length > 0
            );
        },
        plotTemperatureSamples(): StatisticsSample[] {
            return this.plotSamples.filter(
                (sample) => sample.inverter_temperatures && Object.keys(sample.inverter_temperatures).length > 0
            );
        },
        days(): StatisticsDay[] {
            return this.status?.days || [];
        },
        flexibleLoads(): StatisticsFlexibleLoad[] {
            return this.status?.flexible_loads || [];
        },
        inverters(): StatisticsInverter[] {
            return (this.status?.inverters || []).slice().sort((a, b) => a.order - b.order);
        },
        panels(): StatisticsPanel[] {
            return this.status?.panels || [];
        },
        panelInverterOptions(): { value: string; label: string }[] {
            const options: { value: string; label: string }[] = [];
            const seen = new Set<string>();
            this.panels.forEach((panel) => {
                const value = String(panel.inverter_index);
                if (seen.has(value)) {
                    return;
                }
                seen.add(value);
                options.push({ value, label: panel.inverter || panel.serial });
            });
            return options;
        },
        selectedPanelInverterSet(): Set<string> {
            return new Set(this.selectedPanelInverters);
        },
        panelSeries(): PanelSeries[] {
            if (this.panelDisplayMode === 'inverters') {
                return this.panelInverterOptions
                    .filter((option) => this.selectedPanelInverterSet.has(option.value))
                    .map((option) => ({
                        key: `inverter-${option.value}`,
                        label: option.label,
                        positions: this.panels
                            .map((panel, position) => ({ panel, position }))
                            .filter(({ panel }) => String(panel.inverter_index) === option.value)
                            .map(({ position }) => position),
                    }))
                    .filter((series) => series.positions.length > 0);
            }

            const showInverterName = this.selectedPanelInverters.length !== 1;
            return this.panels
                .map((panel, position) => ({ panel, position }))
                .filter(({ panel }) => this.selectedPanelInverterSet.has(String(panel.inverter_index)))
                .map(({ panel, position }) => {
                    const fallbackLabel = this.$t('statistics.InputWithNumber', { num: panel.channel_number });
                    return {
                        key: `panel-${panel.index}`,
                        label: showInverterName
                            ? panel.label || `${panel.inverter || panel.serial} / ${fallbackLabel}`
                            : panel.name || fallbackLabel,
                        positions: [position],
                    };
                });
        },
        hasPanelData(): boolean {
            return this.panels.length > 0;
        },
        hasLoadData(): boolean {
            return this.loadPoints.some((point) => point.other > 0 || point.flexibleValues.some((value) => value > 0));
        },
        hasTemperatureData(): boolean {
            return !this.isDailyPeriod && this.inverters.length > 0;
        },
        summaryConsumptionWh(): number {
            const summary = this.summary;
            if (!summary) {
                return 0;
            }
            return (
                summary.solar_energy_wh +
                summary.battery_discharge_wh -
                summary.grid_charger_energy_wh +
                summary.grid_import_wh -
                summary.grid_export_wh
            );
        },
        selfGeneratedConsumptionPercent(): number | undefined {
            const summary = this.summary;
            const consumption = this.summaryConsumptionWh;
            if (!summary || consumption <= 0) {
                return undefined;
            }

            const selfGeneratedWh = Math.max(0, consumption - Math.max(0, summary.grid_import_wh));
            return Math.min(100, (selfGeneratedWh / consumption) * 100);
        },
        kpiCards() {
            const summary = this.summary;
            const consumption = this.summaryConsumptionWh;
            const gridBalance = (summary?.grid_import_wh || 0) - (summary?.grid_export_wh || 0);
            const batteryBalance = (summary?.battery_discharge_wh || 0) - (summary?.grid_charger_energy_wh || 0);
            return [
                { label: this.$t('statistics.Consumption'), value: this.formatWh(consumption) },
                {
                    label: this.$t('statistics.SelfGeneratedConsumption'),
                    value: this.formatOptionalPercent(this.selfGeneratedConsumptionPercent),
                },
                { label: this.$t('statistics.SolarEnergy'), value: this.formatWh(summary?.solar_energy_wh || 0) },
                { label: this.$t('statistics.GridImport'), value: this.formatWh(summary?.grid_import_wh || 0) },
                { label: this.$t('statistics.GridBalance'), value: this.formatSignedWh(gridBalance) },
                { label: this.$t('statistics.BatteryBalance'), value: this.formatSignedWh(batteryBalance) },
                {
                    label: this.$t('statistics.GridCharger'),
                    value: this.formatWh(summary?.grid_charger_energy_wh || 0),
                },
                {
                    label: this.$t('statistics.FlexibleLoadEnergy'),
                    value: this.formatWh(summary?.flexible_load_energy_wh || 0),
                },
            ];
        },
        flowPoints(): FlowStackPoint[] {
            if (this.isDailyPeriod) {
                return this.days.map((day) =>
                    this.createEnergyStackPoint(
                        day.t,
                        day.solar_energy_wh,
                        day.battery_discharge_wh,
                        day.grid_import_wh,
                        day.grid_export_wh,
                        day.grid_charger_energy_wh
                    )
                );
            }

            const samples = this.samples.slice();
            while (samples.length > 0) {
                const lastSample = samples[samples.length - 1];
                if (!lastSample || !this.isEmptyPowerSample(lastSample)) {
                    break;
                }
                samples.pop();
            }
            return samples.map((sample) => this.createPowerStackPoint(sample));
        },
        plotFlowPoints(): FlowStackPoint[] {
            if (this.isDailyPeriod) {
                return this.flowPoints;
            }

            const samples = this.plotSamples.slice();
            while (samples.length > 0) {
                const lastSample = samples[samples.length - 1];
                if (!lastSample || !this.isEmptyPowerSample(lastSample)) {
                    break;
                }
                samples.pop();
            }
            return samples.map((sample) => this.createPowerStackPoint(sample));
        },
        loadPoints(): LoadPoint[] {
            if (this.isDailyPeriod) {
                return this.days.map((day) => {
                    const flexibleValues = day.flexible_load_energies_wh || [];
                    const totalLoad = Math.max(
                        0,
                        this.createEnergyStackPoint(
                            day.t,
                            day.solar_energy_wh,
                            day.battery_discharge_wh,
                            day.grid_import_wh,
                            day.grid_export_wh,
                            day.grid_charger_energy_wh
                        ).consumption
                    );
                    const flexibleLoadEnergy = this.measuredLoadTotal(day.flexible_load_energy_wh, flexibleValues);
                    return {
                        t: day.t,
                        flexibleValues,
                        other: this.otherLoadValue(totalLoad, flexibleLoadEnergy),
                    };
                });
            }

            return this.samples.map((sample) => {
                const flexibleValues = sample.flexible_load_powers_w || [];
                const totalLoad = Math.max(0, this.createPowerStackPoint(sample).consumption);
                const flexibleLoadPower = this.measuredLoadTotal(sample.flexible_load_power_w, flexibleValues);
                return {
                    t: sample.t,
                    flexibleValues,
                    other: this.otherLoadValue(totalLoad, flexibleLoadPower),
                };
            });
        },
        plotLoadPoints(): LoadPoint[] {
            if (this.isDailyPeriod) {
                return this.loadPoints;
            }

            return this.plotSamples.map((sample) => {
                const flexibleValues = sample.flexible_load_powers_w || [];
                const totalLoad = Math.max(0, this.createPowerStackPoint(sample).consumption);
                const flexibleLoadPower = this.measuredLoadTotal(sample.flexible_load_power_w, flexibleValues);
                return {
                    t: sample.t,
                    flexibleValues,
                    other: this.otherLoadValue(totalLoad, flexibleLoadPower),
                };
            });
        },
        panelPoints(): PanelPoint[] {
            const points = this.isDailyPeriod
                ? this.days.map((day) => ({
                      t: day.t,
                      values: day.panel_energies_wh || [],
                  }))
                : this.samples.map((sample) => ({
                      t: sample.t,
                      values: sample.panel_powers_w || [],
                  }));

            return points.filter((point) => this.hasPanelPointValue(point));
        },
        plotPanelPoints(): PanelPoint[] {
            if (this.isDailyPeriod) {
                return this.panelPoints;
            }

            return this.plotSamples
                .map((sample) => ({
                    t: sample.t,
                    values: sample.panel_powers_w || [],
                }))
                .filter((point) => this.hasPanelPointValue(point));
        },
        boostPoints(): BoostPoint[] {
            if (this.isDailyPeriod) {
                return this.days.map((day) =>
                    this.createBoostPoint(
                        day.t,
                        day.grid_import_wh,
                        day.battery_boost_savings_25a_wh || 0,
                        day.battery_boost_savings_50a_wh || 0,
                        day.battery_boost_relax_limited_wh || 0
                    )
                );
            }

            return this.samples.map((sample) =>
                this.createBoostPoint(
                    sample.t,
                    sample.grid_import_power_w ?? Math.max(0, sample.grid_power_w),
                    sample.battery_boost_savings_25a_power_w || 0,
                    sample.battery_boost_savings_50a_power_w || 0,
                    sample.battery_boost_relax_limited_power_w || 0
                )
            );
        },
        plotBoostPoints(): BoostPoint[] {
            if (this.isDailyPeriod) {
                return this.boostPoints;
            }

            return this.plotSamples.map((sample) =>
                this.createBoostPoint(
                    sample.t,
                    sample.grid_import_power_w ?? Math.max(0, sample.grid_power_w),
                    sample.battery_boost_savings_25a_power_w || 0,
                    sample.battery_boost_savings_50a_power_w || 0,
                    sample.battery_boost_relax_limited_power_w || 0
                )
            );
        },
    },
    methods: {
        statisticsCacheKey(period: string, rangeStart: number | null, view: ChartTab): string {
            return `${period}:${rangeStart === null ? 'latest' : rangeStart}:${view}`;
        },
        getCachedStatus(period: string, rangeStart: number | null, view?: ChartTab): StatisticsStatus | undefined {
            const cacheView = view ?? this.selectedChartTab;
            return this.statusCache[this.statisticsCacheKey(period, rangeStart, cacheView)];
        },
        rememberStatus(period: string, requestedStart: number | null, view: ChartTab, status: StatisticsStatus) {
            this.statusCache[this.statisticsCacheKey(period, requestedStart, view)] = status;
            this.statusCache[this.statisticsCacheKey(period, status.from, view)] = status;
        },
        rangeBufferSpanSeconds(): number {
            return this.isDailyPeriod ? 24 * 60 * 60 : Math.floor(this.periodDurationSeconds / 2);
        },
        plotStatuses(): StatisticsStatus[] {
            const status = this.status;
            if (!status) {
                return [];
            }

            const period = this.selectedPeriod;
            const view = this.selectedChartTab;
            const span = this.rangeBufferSpanSeconds();
            const starts = [status.from - span, status.from, status.from + span].map((start) =>
                this.clampRangeStart(start, false)
            );

            const seen = new Set<number>();
            return starts
                .filter((start) => {
                    if (seen.has(start)) {
                        return false;
                    }
                    seen.add(start);
                    return true;
                })
                .map((start) => this.getCachedStatus(period, start, view))
                .filter((candidate): candidate is StatisticsStatus => candidate !== undefined);
        },
        categoryViewportScale(points: { t: number }[]): Record<string, number> {
            const status = this.status;
            if (this.isDailyPeriod || !status || points.length === 0) {
                return {};
            }

            let min = points.findIndex((point) => point.t >= status.from);
            if (min < 0) {
                min = 0;
            }

            let max = min;
            for (let i = min; i < points.length; ++i) {
                const point = points[i];
                if (!point || point.t > status.to) {
                    break;
                }
                max = i;
            }
            if (max < min) {
                max = min;
            }

            return { min, max };
        },
        fetchStatisticsStatus(
            period: string,
            rangeStart: number | null,
            view: ChartTab,
            useCache = true
        ): Promise<StatisticsStatus> {
            const cacheKey = this.statisticsCacheKey(period, rangeStart, view);
            const cached = useCache ? this.statusCache[cacheKey] : undefined;
            if (cached) {
                return Promise.resolve(cached);
            }

            const pending = this.pendingStatusRequests[cacheKey];
            if (pending) {
                return pending;
            }

            const params = new URLSearchParams({
                period,
                format: 'compact',
                view,
            });
            if (rangeStart !== null) {
                params.set('from', String(rangeStart));
            }
            const request = fetch(`/api/statistics/status?${params.toString()}`, { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    const status = this.normalizeStatisticsStatus(data);
                    this.rememberStatus(period, rangeStart, view, status);
                    return status;
                });
            this.pendingStatusRequests[cacheKey] = request;
            request.then(
                () => {
                    delete this.pendingStatusRequests[cacheKey];
                },
                () => {
                    delete this.pendingStatusRequests[cacheKey];
                }
            );
            return request;
        },
        applyStatus(status: StatisticsStatus, rangeStart: number | null, render = true) {
            this.destroyCharts();
            this.status = status;
            this.rangeStartInput = this.dateInputFromTimestamp(status.from);
            this.customRangeStart = rangeStart === null ? null : status.from;
            this.ensurePanelFilterDefaults();
            this.ensureVisibleChartTab();
            this.dataLoading = false;
            if (this.rangeDragFinalizeAfterApply) {
                this.rangeDragFinalizeAfterApply = false;
                this.resetRangeDragState(false);
            }
            if (render) {
                this.$nextTick(() => this.renderActiveChart());
            }
            this.scheduleAdjacentRangePrefetch(this.rangeDrag.active ? 40 : 250);
        },
        getData(options: { rangeStart?: number | null; showLoading?: boolean; useCache?: boolean } = {}) {
            const period = this.selectedPeriod;
            const view = this.selectedChartTab;
            const rangeStart = options.rangeStart === undefined ? this.customRangeStart : options.rangeStart;
            const token = ++this.requestSerial;
            if (options.showLoading ?? !this.status) {
                this.dataLoading = true;
            }

            this.fetchStatisticsStatus(period, rangeStart, view, options.useCache ?? true)
                .then((status) => {
                    if (
                        token !== this.requestSerial ||
                        period !== this.selectedPeriod ||
                        view !== this.selectedChartTab
                    ) {
                        return;
                    }
                    this.applyStatus(status, rangeStart);
                })
                .catch(() => {
                    if (token === this.requestSerial) {
                        this.dataLoading = false;
                        if (this.rangeDragFinalizeAfterApply) {
                            this.rangeDragFinalizeAfterApply = false;
                            this.resetRangeDragState(true);
                        }
                    }
                });
        },
        normalizeStatisticsStatus(data: StatisticsStatus): StatisticsStatus {
            if (!Array.isArray(data.sample_fields) || !Array.isArray(data.samples)) {
                return data;
            }

            const fields = data.sample_fields;
            data.samples = data.samples.map((sample) => {
                if (!Array.isArray(sample)) {
                    return sample;
                }

                const normalized = {} as Record<string, unknown>;
                fields.forEach((field, index) => {
                    const value = sample[index];
                    if (value !== null && value !== undefined) {
                        normalized[field] = value;
                    }
                });
                return normalized as unknown as StatisticsSample;
            });
            return data;
        },
        reloadData() {
            this.statusCache = {};
            this.getData({ showLoading: true, useCache: false });
        },
        setPeriod(period: string) {
            if (this.prefetchTimer !== null) {
                window.clearTimeout(this.prefetchTimer);
                this.prefetchTimer = null;
            }
            this.selectedPeriod = period;
            if (this.customRangeStart !== null) {
                this.customRangeStart = this.clampRangeStart(this.customRangeStart);
                this.rangeStartInput = this.dateInputFromTimestamp(this.customRangeStart);
            }
            this.getData({ showLoading: true });
        },
        setRangeStartFromInput() {
            const timestamp = this.timestampFromDateInput(this.rangeStartInput);
            if (timestamp === null) {
                return;
            }

            this.customRangeStart = this.clampRangeStart(timestamp);
            this.rangeStartInput = this.dateInputFromTimestamp(this.customRangeStart);
            this.loadRangeStart(this.customRangeStart, true);
        },
        resetToLatestRange() {
            this.customRangeStart = null;
            this.getData({ rangeStart: null, showLoading: true, useCache: false });
        },
        shiftRange(direction: -1 | 1) {
            this.shiftRangeBySeconds(direction * this.periodDurationSeconds);
        },
        shiftRangeBySeconds(seconds: number, interactive = false) {
            const currentStart = this.status?.from ?? this.customRangeStart ?? this.latestSelectableRangeStart();
            const nextStart = this.clampRangeStart(currentStart + seconds);
            if (nextStart === currentStart && this.customRangeStart !== null) {
                return;
            }

            this.loadRangeStart(nextStart, !interactive);
        },
        loadRangeStart(nextStart: number, showLoading: boolean) {
            this.customRangeStart = nextStart;
            this.rangeStartInput = this.dateInputFromTimestamp(nextStart);
            const cached = this.getCachedStatus(this.selectedPeriod, nextStart);
            if (cached) {
                this.requestSerial++;
                if (this.rangeDrag.active && !this.rangeDragEnding) {
                    this.queueRangeDragStatusApply(cached, nextStart);
                } else {
                    this.applyStatus(cached, nextStart);
                }
                return;
            }

            this.getData({ rangeStart: nextStart, showLoading });
        },
        startRangeDrag(event: PointerEvent) {
            if (!this.status || (event.pointerType === 'mouse' && event.button !== 0)) {
                return;
            }

            const target = event.currentTarget as HTMLElement;
            this.setRangeDragCursor(true, target);
            this.rangeDragFinalizeAfterApply = false;
            this.rangeDragEnding = false;
            this.rangeDragLastMoveAt = performance.now();
            this.rangeDrag = {
                active: true,
                pointerId: event.pointerId,
                startX: event.clientX,
                currentX: event.clientX,
                width: Math.max(1, target.clientWidth),
                baseStart: this.status.from,
                lastAppliedStart: this.status.from,
            };
            this.rangeDragSession++;
            try {
                target.setPointerCapture(event.pointerId);
            } catch (_) {
                // Pointer capture is best-effort; window listeners below still finish the drag.
            }
            window.addEventListener('pointermove', this.moveRangeDrag, true);
            window.addEventListener('pointerup', this.endRangeDrag, true);
            window.addEventListener('pointercancel', this.cancelRangeDragPointer, true);
            window.addEventListener('mouseup', this.endRangeDragMouseFallback, true);
            window.requestAnimationFrame(() => {
                if (this.rangeDrag.active && this.rangeDrag.pointerId === event.pointerId) {
                    this.scheduleAdjacentRangePrefetch(0);
                }
            });
            event.preventDefault();
        },
        moveRangeDrag(event: PointerEvent) {
            if (this.rangeDragEnding || !this.rangeDrag.active || this.rangeDrag.pointerId !== event.pointerId) {
                return;
            }

            this.rangeDrag.currentX = event.clientX;
            this.rangeDragLastMoveAt = performance.now();
            this.applyRangeDrag(event.clientX);
            this.scheduleActiveChartDragOffsetUpdate();
            event.preventDefault();
        },
        endRangeDrag(event: PointerEvent) {
            if (this.rangeDragEnding || !this.rangeDrag.active || this.rangeDrag.pointerId !== event.pointerId) {
                return;
            }

            this.finishRangeDrag(event.clientX);
            event.preventDefault();
        },
        endRangeDragMouseFallback(event: MouseEvent) {
            if (this.rangeDragEnding || !this.rangeDrag.active || this.rangeDrag.pointerId === null) {
                return;
            }

            this.finishRangeDrag(event.clientX);
            event.preventDefault();
        },
        finishRangeDrag(clientX: number) {
            this.rangeDragEnding = true;
            this.rangeDrag.currentX = clientX;
            this.applyRangeDrag(clientX);
            const finalStart = this.rangeDrag.lastAppliedStart;
            this.stopRangeDragTransport();
            if (finalStart !== null) {
                this.rangeDragFinalizeAfterApply = true;
                this.loadRangeStart(finalStart, false);
            } else {
                this.resetRangeDragState(true);
            }
        },
        applyRangeDrag(clientX: number) {
            const deltaX = clientX - this.rangeDrag.startX;
            if (this.rangeDrag.width <= 0) {
                return;
            }

            const rawShiftSeconds = (-deltaX / this.rangeDrag.width) * this.periodDurationSeconds;
            const nextStart = this.clampRangeStart(this.rangeDrag.baseStart + rawShiftSeconds, false);
            if (nextStart === this.rangeDrag.lastAppliedStart) {
                return;
            }

            this.rangeDrag.lastAppliedStart = nextStart;
            this.customRangeStart = nextStart;
            this.rangeStartInput = this.dateInputFromTimestamp(nextStart);
            if (this.rangeDragEnding) {
                return;
            }

            this.scheduleAdjacentRangePrefetch(40);

            const cached = this.getCachedStatus(this.selectedPeriod, nextStart);
            if (cached) {
                this.requestSerial++;
                this.queueRangeDragStatusApply(cached, nextStart);
                return;
            }

            this.queueRangeDragFetch();
        },
        queueRangeDragFetch() {
            if (this.rangeDrag.lastAppliedStart === null) {
                return;
            }
            this.rangeDragQueuedStart = this.rangeDrag.lastAppliedStart;
            if (this.rangeDragFetchTimer !== null || this.rangeDragFetchInFlight) {
                return;
            }

            this.rangeDragFetchTimer = window.setTimeout(() => {
                this.rangeDragFetchTimer = null;
                this.flushRangeDragFetch();
            }, 80);
        },
        flushRangeDragFetch() {
            if (!this.rangeDrag.active || this.rangeDragQueuedStart === null || this.rangeDragFetchInFlight) {
                return;
            }

            const period = this.selectedPeriod;
            const view = this.selectedChartTab;
            const start = this.rangeDragQueuedStart;
            const session = this.rangeDragSession;
            this.rangeDragQueuedStart = null;

            const cached = this.getCachedStatus(period, start, view);
            if (cached) {
                this.requestSerial++;
                this.queueRangeDragStatusApply(cached, start);
                return;
            }

            this.rangeDragFetchInFlight = true;
            this.fetchStatisticsStatus(period, start, view, true)
                .then((status) => {
                    if (
                        session !== this.rangeDragSession ||
                        !this.rangeDrag.active ||
                        period !== this.selectedPeriod ||
                        view !== this.selectedChartTab
                    ) {
                        return;
                    }
                    this.queueRangeDragStatusApply(status, start);
                })
                .catch(() => undefined)
                .then(() => {
                    if (session !== this.rangeDragSession) {
                        return;
                    }
                    this.rangeDragFetchInFlight = false;
                    if (this.rangeDrag.active && this.rangeDragQueuedStart !== null) {
                        this.queueRangeDragFetch();
                    }
                });
        },
        stopRangeDragTransport() {
            window.removeEventListener('pointermove', this.moveRangeDrag, true);
            window.removeEventListener('pointerup', this.endRangeDrag, true);
            window.removeEventListener('pointercancel', this.cancelRangeDragPointer, true);
            window.removeEventListener('mouseup', this.endRangeDragMouseFallback, true);
            if (this.rangeDragFetchTimer !== null) {
                window.clearTimeout(this.rangeDragFetchTimer);
                this.rangeDragFetchTimer = null;
            }
            this.rangeDragQueuedStart = null;
            this.rangeDragPendingStatus = null;
            if (this.rangeDragStatusApplyTimer !== null) {
                window.clearTimeout(this.rangeDragStatusApplyTimer);
                this.rangeDragStatusApplyTimer = null;
            }
            this.rangeDragSession++;
            this.rangeDragFetchInFlight = false;
        },
        queueRangeDragStatusApply(status: StatisticsStatus, rangeStart: number) {
            if (!this.rangeDrag.active || this.rangeDragEnding) {
                return;
            }

            this.rangeDragPendingStatus = { status, rangeStart };
            if (this.rangeDragStatusApplyTimer !== null) {
                return;
            }

            this.rangeDragStatusApplyTimer = window.setTimeout(() => this.flushRangeDragStatusApply(), 140);
        },
        flushRangeDragStatusApply() {
            this.rangeDragStatusApplyTimer = null;
            if (!this.rangeDrag.active || this.rangeDragEnding || this.rangeDragPendingStatus === null) {
                return;
            }

            const quietMs = performance.now() - this.rangeDragLastMoveAt;
            if (quietMs < 120) {
                this.rangeDragStatusApplyTimer = window.setTimeout(
                    () => this.flushRangeDragStatusApply(),
                    120 - quietMs
                );
                return;
            }

            const pending = this.rangeDragPendingStatus;
            this.rangeDragPendingStatus = null;
            this.requestSerial++;
            this.applyStatus(pending.status, pending.rangeStart);
        },
        setRangeDragCursor(active: boolean, element?: HTMLElement) {
            if (active) {
                this.rangeDragElement = element ?? this.rangeDragElement;
                this.rangeDragElement?.classList.add('chart-host-dragging');
                document.documentElement.classList.add('statistics-range-dragging');
                return;
            }

            this.rangeDragElement?.classList.remove('chart-host-dragging');
            this.rangeDragElement = null;
            document.documentElement.classList.remove('statistics-range-dragging');
        },
        resetRangeDragState(redraw: boolean) {
            this.rangeDrag = {
                active: false,
                pointerId: null,
                startX: 0,
                currentX: 0,
                width: 0,
                baseStart: 0,
                lastAppliedStart: null,
            };
            this.rangeDragEnding = false;
            this.rangeDragPendingStatus = null;
            if (this.rangeDragStatusApplyTimer !== null) {
                window.clearTimeout(this.rangeDragStatusApplyTimer);
                this.rangeDragStatusApplyTimer = null;
            }
            if (this.rangeDragDrawFrame !== null) {
                window.cancelAnimationFrame(this.rangeDragDrawFrame);
                this.rangeDragDrawFrame = null;
            }
            this.setRangeDragCursor(false);
            if (redraw) {
                this.updateActiveChartDragOffset();
            }
        },
        cancelRangeDragPointer(event: PointerEvent) {
            if (!this.rangeDrag.active || this.rangeDrag.pointerId !== event.pointerId) {
                return;
            }
            if (event.pointerType === 'mouse') {
                event.preventDefault();
                return;
            }

            this.cancelRangeDrag();
            event.preventDefault();
        },
        cancelRangeDrag() {
            this.rangeDragFinalizeAfterApply = false;
            this.stopRangeDragTransport();
            this.resetRangeDragState(true);
        },
        setChartTab(tab: ChartTab) {
            if (this.selectedChartTab === tab) {
                return;
            }
            this.destroyCharts();
            this.selectedChartTab = tab;
            this.getData({ showLoading: false });
        },
        onPanelSelectionChanged() {
            const validValues = new Set(this.panelInverterOptions.map((option) => option.value));
            this.selectedPanelInverters = this.selectedPanelInverters.filter((value) => validValues.has(value));
            if (this.selectedPanelInverters.length === 0) {
                this.selectedPanelInverters = this.panelInverterOptions.map((option) => option.value);
            }
            this.renderActiveChart();
        },
        ensureVisibleChartTab() {
            if (!this.chartTabs.some((tab) => tab.value === this.selectedChartTab)) {
                this.selectedChartTab = 'flow';
            }
        },
        ensurePanelFilterDefaults() {
            if (this.panels.length === 0) {
                return;
            }

            const options = this.panelInverterOptions.map((option) => option.value);
            const validSelection = this.selectedPanelInverters.filter((value) => options.includes(value));
            if (this.panelFilterInitialized && validSelection.length > 0) {
                if (validSelection.length !== this.selectedPanelInverters.length) {
                    this.selectedPanelInverters = validSelection;
                }
                return;
            }

            this.selectedPanelInverters = options;
            this.panelFilterInitialized = true;
        },
        scheduleAdjacentRangePrefetch(delay = 250) {
            if (this.rangeDragEnding) {
                return;
            }

            if (this.prefetchTimer !== null) {
                if (this.rangeDrag.active && delay > 0) {
                    return;
                }
                window.clearTimeout(this.prefetchTimer);
            }

            this.prefetchTimer = window.setTimeout(() => {
                this.prefetchTimer = null;
                this.prefetchAdjacentRanges();
            }, delay);
        },
        adjacentRangePrefetchStarts(period: string, view: ChartTab): number[] {
            if (!this.status) {
                return [];
            }

            const baseStart =
                this.rangeDrag.active && this.rangeDrag.lastAppliedStart !== null
                    ? this.rangeDrag.lastAppliedStart
                    : this.status.from;
            const span = this.rangeBufferSpanSeconds();
            const dragDelta = this.rangeDrag.currentX - this.rangeDrag.startX;
            const dragDirection = dragDelta > 0 ? -1 : dragDelta < 0 ? 1 : 0;
            const offsets =
                this.rangeDrag.active && dragDirection !== 0
                    ? [0, dragDirection * span, dragDirection * span * 2, -dragDirection * span]
                    : [-span, span];
            const seen = new Set<number>();

            return offsets
                .map((offset) => this.clampRangeStart(baseStart + offset, false))
                .filter((start) => {
                    if (seen.has(start)) {
                        return false;
                    }
                    seen.add(start);
                    if (!this.rangeDrag.active && start === this.status?.from) {
                        return false;
                    }
                    if (this.getCachedStatus(period, start, view)) {
                        return false;
                    }
                    return !this.pendingStatusRequests[this.statisticsCacheKey(period, start, view)];
                });
        },
        prefetchAdjacentRanges() {
            if (!this.status || this.dataLoading || this.prefetchInFlight || this.rangeDragEnding) {
                return;
            }

            const period = this.selectedPeriod;
            const view = this.selectedChartTab;
            const starts = this.adjacentRangePrefetchStarts(period, view);
            if (starts.length === 0) {
                return;
            }

            this.prefetchInFlight = true;
            starts
                .reduce(
                    (chain, start) =>
                        chain.then(() =>
                            this.fetchStatisticsStatus(period, start, view, true).then(() => {
                                if (
                                    !this.rangeDrag.active &&
                                    period === this.selectedPeriod &&
                                    view === this.selectedChartTab &&
                                    !this.dataLoading
                                ) {
                                    this.$nextTick(() => this.renderActiveChart());
                                }
                            })
                        ),
                    Promise.resolve()
                )
                .catch(() => undefined)
                .then(() => {
                    this.prefetchInFlight = false;
                    if (
                        this.rangeDrag.active &&
                        !this.rangeDragEnding &&
                        period === this.selectedPeriod &&
                        view === this.selectedChartTab
                    ) {
                        this.scheduleAdjacentRangePrefetch(80);
                    }
                });
        },
        dateInputFromTimestamp(timestamp: number): string {
            const date = new Date(timestamp * 1000);
            const year = date.getFullYear();
            const month = String(date.getMonth() + 1).padStart(2, '0');
            const day = String(date.getDate()).padStart(2, '0');
            return `${year}-${month}-${day}`;
        },
        timestampFromDateInput(value: string): number | null {
            const match = value.match(/^(\d{4})-(\d{2})-(\d{2})$/);
            if (!match) {
                return null;
            }

            const year = Number(match[1]);
            const month = Number(match[2]);
            const day = Number(match[3]);
            if (!Number.isFinite(year) || !Number.isFinite(month) || !Number.isFinite(day)) {
                return null;
            }

            return Math.floor(new Date(year, month - 1, day, 0, 0, 0, 0).getTime() / 1000);
        },
        startOfLocalDay(timestamp: number): number {
            const date = new Date(timestamp * 1000);
            date.setHours(0, 0, 0, 0);
            return Math.floor(date.getTime() / 1000);
        },
        alignRangeStartToStep(timestamp: number): number {
            const step = this.dragStepSeconds();
            if (step <= 1) {
                return Math.floor(timestamp);
            }
            return Math.floor(timestamp / step) * step;
        },
        latestSelectableRangeStart(): number {
            const now = Math.floor(Date.now() / 1000);
            if (this.selectedPeriod === 'today') {
                return this.startOfLocalDay(now);
            }
            if (this.isDailyPeriod) {
                return this.startOfLocalDay(Math.max(0, now - this.periodDurationSeconds));
            }
            return Math.max(0, now - this.periodDurationSeconds);
        },
        clampRangeStart(timestamp: number, align = true): number {
            let start = Math.floor(timestamp);
            const min = this.status?.recent_from
                ? this.isDailyPeriod
                    ? this.startOfLocalDay(this.status.recent_from)
                    : this.alignRangeStartToStep(this.status.recent_from)
                : 0;
            const max = this.latestSelectableRangeStart();

            if (min > 0) {
                start = Math.max(start, min);
            }
            start = Math.min(start, max);
            if (this.isDailyPeriod) {
                start = this.startOfLocalDay(start);
            } else if (align) {
                start = this.alignRangeStartToStep(start);
            }
            return Math.max(0, start);
        },
        dragStepSeconds(): number {
            if (this.isDailyPeriod) {
                return 24 * 60 * 60;
            }
            if (this.status?.period === this.selectedPeriod && this.status.resolution_seconds > 0) {
                return this.status.resolution_seconds;
            }
            if (this.selectedPeriod === '7d') {
                return 60 * 60;
            }
            return this.status?.sample_interval_seconds || 5 * 60;
        },
        prefetchStepCount(): number {
            if (this.isDailyPeriod) {
                return 1;
            }
            return 1;
        },
        isEmptyPowerSample(sample: StatisticsSample): boolean {
            return [
                sample.battery_discharge_power_w,
                sample.solar_power_w,
                sample.battery_power_w,
                sample.grid_power_w,
                sample.grid_import_power_w ?? 0,
                sample.grid_export_power_w ?? 0,
                sample.grid_charger_power_w,
                sample.flexible_load_power_w ?? 0,
                sample.target_power_w,
            ].every((value) => value === 0);
        },
        createPowerStackPoint(sample: StatisticsSample): FlowStackPoint {
            const solarPower = Math.max(0, sample.solar_power_w);
            const batteryDischargePower = Math.max(0, sample.battery_discharge_power_w);
            const acChargerPower = Math.max(0, sample.grid_charger_power_w);
            const gridImportShare = Math.max(0, sample.grid_import_power_w ?? Math.max(0, sample.grid_power_w));
            const gridExportShare = -Math.max(0, sample.grid_export_power_w ?? Math.max(0, -sample.grid_power_w));
            const batteryShare = batteryDischargePower;
            const solarShare = solarPower;
            const acChargerShare = -acChargerPower;

            return {
                t: sample.t,
                consumption: gridImportShare + gridExportShare + batteryShare + solarShare + acChargerShare,
                gridImportShare,
                gridExportShare,
                batteryShare,
                solarShare,
                acChargerShare,
            };
        },
        createEnergyStackPoint(
            t: number,
            solarWh: number,
            batteryDischargeWh: number,
            gridImportWh: number,
            gridExportWh: number,
            gridChargerWh: number
        ): EnergyStackPoint {
            const solarEnergy = Math.max(0, solarWh);
            const batteryDischargeEnergy = Math.max(0, batteryDischargeWh);
            const acChargerEnergy = Math.max(0, gridChargerWh);
            const gridImportShare = Math.max(0, gridImportWh);
            const gridExportShare = -Math.max(0, gridExportWh);
            const batteryShare = batteryDischargeEnergy;
            const solarShare = solarEnergy;
            const acChargerShare = -acChargerEnergy;

            return {
                t,
                consumption: gridImportShare + gridExportShare + batteryShare + solarShare + acChargerShare,
                gridImportShare,
                gridExportShare,
                batteryShare,
                solarShare,
                acChargerShare,
            };
        },
        createBoostPoint(
            t: number,
            gridImport: number,
            savings25A: number,
            savings50A: number,
            relaxLimited: number
        ): BoostPoint {
            const gridImportValue = Math.max(0, gridImport);
            const cappedSavings25A = Math.max(0, Math.min(gridImportValue, savings25A));
            const cappedSavings50A = Math.max(cappedSavings25A, Math.min(gridImportValue, savings50A));
            return {
                t,
                gridImport: gridImportValue,
                savings25A: cappedSavings25A,
                boostExtra: Math.max(0, cappedSavings50A - cappedSavings25A),
                remainingImport: Math.max(0, gridImportValue - cappedSavings50A),
                relaxLimited: Math.max(0, relaxLimited),
            };
        },
        activeManagedChart(): ManagedChart | null {
            if (this.selectedChartTab === 'flow') {
                return this.flowChart;
            }
            if (this.selectedChartTab === 'panels') {
                return this.panelChart;
            }
            if (this.selectedChartTab === 'loads') {
                return this.loadChart;
            }
            if (this.selectedChartTab === 'temperatures') {
                return this.temperatureChart;
            }
            if (this.selectedChartTab === 'boost') {
                return this.boostChart;
            }
            return this.batteryChart;
        },
        rangeDragOffsetPixels(): number {
            if (!this.rangeDrag.active || this.rangeDrag.width <= 0) {
                return 0;
            }

            const renderedStart = this.status?.from ?? this.rangeDrag.baseStart;
            const renderedShiftSeconds = renderedStart - this.rangeDrag.baseStart;
            const renderedPixels = -(renderedShiftSeconds / this.periodDurationSeconds) * this.rangeDrag.width;
            return this.rangeDrag.currentX - this.rangeDrag.startX - renderedPixels;
        },
        updateActiveChartDragOffset() {
            const chart = this.activeManagedChart() as StatisticsDragChart | null;
            if (!chart) {
                return;
            }

            chart.$statisticsRangeDragOffsetPx = this.rangeDragOffsetPixels();
            chart.draw();
        },
        scheduleActiveChartDragOffsetUpdate() {
            if (this.rangeDragDrawFrame !== null) {
                return;
            }

            this.rangeDragDrawFrame = window.requestAnimationFrame(() => {
                this.rangeDragDrawFrame = null;
                this.updateActiveChartDragOffset();
            });
        },
        renderCharts() {
            this.renderActiveChart();
        },
        renderActiveChart() {
            if (!this.status) {
                return;
            }

            if (this.selectedChartTab === 'flow') {
                this.renderFlowChart();
            } else if (this.selectedChartTab === 'panels') {
                this.renderPanelChart();
            } else if (this.selectedChartTab === 'loads') {
                this.renderLoadChart();
            } else if (this.selectedChartTab === 'temperatures') {
                this.renderTemperatureChart();
            } else if (this.selectedChartTab === 'boost') {
                this.renderBoostChart();
            } else {
                this.renderBatteryChart();
            }
            this.updateActiveChartDragOffset();
            this.resizeActiveChartAfterLayout();
        },
        resizeActiveChartAfterLayout() {
            window.requestAnimationFrame(() => {
                window.requestAnimationFrame(() => {
                    if (this.selectedChartTab === 'flow') {
                        this.flowChart?.resize();
                    } else if (this.selectedChartTab === 'panels') {
                        this.panelChart?.resize();
                    } else if (this.selectedChartTab === 'loads') {
                        this.loadChart?.resize();
                    } else if (this.selectedChartTab === 'temperatures') {
                        this.temperatureChart?.resize();
                    } else if (this.selectedChartTab === 'boost') {
                        this.boostChart?.resize();
                    } else {
                        this.batteryChart?.resize();
                    }
                });
            });
        },
        renderFlowChart() {
            const canvas = this.$refs.flowChart as HTMLCanvasElement | undefined;
            if (!canvas) {
                return;
            }

            const unit = this.isDailyPeriod ? 'energy' : 'power';
            const points = this.plotFlowPoints;
            const labels = this.chartLabels(points);
            const datasets = this.createFlowDatasets(points);
            const config: ChartConfiguration<'bar' | 'line', number[], string> = {
                type: this.isDailyPeriod ? 'bar' : 'line',
                data: { labels, datasets },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'bar' | 'line'>) => {
                                    const value = context.parsed.y || 0;
                                    return `${context.dataset.label}: ${unit === 'energy' ? this.formatWh(value) : this.formatWatt(value)}`;
                                },
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            stacked: true,
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: this.isDailyPeriod ? 10 : 8,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            stacked: true,
                            title: {
                                display: true,
                                text: this.isDailyPeriod ? this.$t('statistics.Energy') : this.$t('statistics.Power'),
                            },
                            ticks: {
                                callback: (value) =>
                                    unit === 'energy' ? this.formatWh(Number(value)) : this.formatWatt(Number(value)),
                            },
                        },
                    },
                },
            };

            this.flowChart?.destroy();
            this.flowChart = new ChartJS(canvas, config);
        },
        renderPanelChart() {
            const canvas = this.$refs.panelChart as HTMLCanvasElement | undefined;
            if (!canvas || !this.hasPanelData) {
                this.panelChart?.destroy();
                this.panelChart = null;
                return;
            }

            const unit = this.isDailyPeriod ? 'energy' : 'power';
            const points = this.plotPanelPoints;
            const labels = this.chartLabels(points);
            const colors = [
                '#0d6efd',
                '#dc3545',
                '#198754',
                '#fd7e14',
                '#6f42c1',
                '#20c997',
                '#d63384',
                '#6c757d',
                '#0dcaf0',
                '#ffc107',
                '#795548',
                '#607d8b',
            ];
            const datasets: ChartDataset<'bar' | 'line', (number | null)[]>[] = this.panelSeries.map(
                (series, index) => {
                    const color = colors[index % colors.length];
                    return {
                        type: this.isDailyPeriod ? 'bar' : 'line',
                        label: series.label,
                        data: points.map((point) => this.panelSeriesValue(point, series.positions)),
                        backgroundColor: this.isDailyPeriod ? `${color}99` : 'transparent',
                        borderColor: color,
                        borderWidth: this.isDailyPeriod ? 1 : 2,
                        clip: false,
                        fill: false,
                        pointRadius: !this.isDailyPeriod && points.length < 3 ? 2 : 0,
                        spanGaps: true,
                        tension: 0.15,
                        stack: this.isDailyPeriod ? 'panels' : undefined,
                        barPercentage: this.isDailyPeriod ? 0.82 : undefined,
                        categoryPercentage: this.isDailyPeriod ? 0.78 : undefined,
                    };
                }
            );

            const config: ChartConfiguration<'bar' | 'line', (number | null)[], string> = {
                type: this.isDailyPeriod ? 'bar' : 'line',
                data: { labels, datasets },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'bar' | 'line'>) => {
                                    const value = context.parsed.y;
                                    return `${context.dataset.label}: ${unit === 'energy' ? this.formatWh(value || 0) : this.formatWatt(value || 0)}`;
                                },
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            stacked: this.isDailyPeriod,
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: this.isDailyPeriod ? 10 : 8,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            stacked: this.isDailyPeriod,
                            beginAtZero: true,
                            title: {
                                display: true,
                                text: this.isDailyPeriod ? this.$t('statistics.Energy') : this.$t('statistics.Power'),
                            },
                            ticks: {
                                callback: (value) =>
                                    unit === 'energy' ? this.formatWh(Number(value)) : this.formatWatt(Number(value)),
                            },
                        },
                    },
                },
            };

            this.panelChart?.destroy();
            this.panelChart = new ChartJS(canvas, config);
        },
        createFlowDatasets(points?: FlowStackPoint[]): ChartDataset<'bar' | 'line', number[]>[] {
            const sourcePoints = points ?? this.flowPoints;
            const flowOptions = {
                borderWidth: 0,
                barPercentage: this.isDailyPeriod ? 0.82 : 1.0,
                categoryPercentage: this.isDailyPeriod ? 0.78 : 1.0,
                clip: false as const,
            };

            return [
                {
                    type: 'bar',
                    label: this.$t('statistics.GridImport'),
                    data: sourcePoints.map((point) => point.gridImportShare),
                    backgroundColor: 'rgba(220, 53, 69, 0.52)',
                    borderColor: '#dc3545',
                    hoverBackgroundColor: 'rgba(220, 53, 69, 0.82)',
                    stack: 'flows',
                    order: 30,
                    ...flowOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.GridExport'),
                    data: sourcePoints.map((point) => point.gridExportShare),
                    backgroundColor: 'rgba(13, 110, 253, 0.42)',
                    borderColor: '#0d6efd',
                    hoverBackgroundColor: 'rgba(13, 110, 253, 0.72)',
                    stack: 'flows',
                    order: 30,
                    ...flowOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.BatteryDischarge'),
                    data: sourcePoints.map((point) => point.batteryShare),
                    backgroundColor: 'rgba(32, 201, 151, 0.58)',
                    borderColor: '#20c997',
                    hoverBackgroundColor: 'rgba(32, 201, 151, 0.86)',
                    stack: 'flows',
                    order: 20,
                    ...flowOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.SolarEnergy'),
                    data: sourcePoints.map((point) => point.solarShare),
                    backgroundColor: 'rgba(240, 173, 0, 0.64)',
                    borderColor: '#f0ad00',
                    hoverBackgroundColor: 'rgba(240, 173, 0, 0.92)',
                    stack: 'flows',
                    order: 10,
                    ...flowOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.GridCharger'),
                    data: sourcePoints.map((point) => point.acChargerShare),
                    backgroundColor: 'rgba(111, 66, 193, 0.52)',
                    borderColor: '#6f42c1',
                    hoverBackgroundColor: 'rgba(111, 66, 193, 0.82)',
                    stack: 'flows',
                    order: 10,
                    ...flowOptions,
                },
                {
                    type: 'line',
                    label: this.$t('statistics.Consumption'),
                    data: sourcePoints.map((point) => point.consumption),
                    backgroundColor: 'transparent',
                    borderColor: '#212529',
                    borderWidth: 2.6,
                    fill: false,
                    clip: false,
                    pointRadius: 0,
                    tension: 0,
                    stack: 'consumption',
                    order: 0,
                },
            ];
        },
        getLoadDefinitions(points?: LoadPoint[]): StatisticsFlexibleLoad[] {
            const sourcePoints = points ?? this.loadPoints;
            const indices = new Set<number>();
            this.flexibleLoads.forEach((load) => indices.add(load.index));
            sourcePoints.forEach((point) => {
                point.flexibleValues.forEach((value, index) => {
                    if (value !== 0) {
                        indices.add(index);
                    }
                });
            });

            return Array.from(indices)
                .sort((a, b) => a - b)
                .map(
                    (index) =>
                        this.flexibleLoads.find((load) => load.index === index) || {
                            index,
                            name: `Load ${index + 1}`,
                            enabled: false,
                            energy_wh: 0,
                        }
                );
        },
        renderLoadChart() {
            const canvas = this.$refs.loadChart as HTMLCanvasElement | undefined;
            if (!canvas || !this.hasLoadData) {
                this.loadChart?.destroy();
                this.loadChart = null;
                return;
            }

            const unit = this.isDailyPeriod ? 'energy' : 'power';
            const points = this.plotLoadPoints;
            const labels = this.chartLabels(points);
            const colors = [
                { background: 'rgba(0, 114, 178, 0.78)', border: '#005a8f' },
                { background: 'rgba(213, 94, 0, 0.76)', border: '#a84700' },
                { background: 'rgba(0, 158, 115, 0.76)', border: '#007a59' },
                { background: 'rgba(204, 121, 167, 0.78)', border: '#a34e7d' },
            ];
            const datasets: ChartDataset<'bar', number[]>[] = this.getLoadDefinitions(points).map((load) => {
                const color = colors[load.index % colors.length] ?? {
                    background: 'rgba(108, 117, 125, 0.46)',
                    border: '#6c757d',
                };
                return {
                    type: 'bar',
                    label: load.name,
                    data: points.map((point) => Math.max(0, point.flexibleValues[load.index] || 0)),
                    backgroundColor: color.background,
                    borderColor: color.border,
                    borderWidth: 1,
                    clip: false,
                    stack: 'loads',
                    barPercentage: this.isDailyPeriod ? 0.82 : 1.0,
                    categoryPercentage: this.isDailyPeriod ? 0.78 : 1.0,
                };
            });
            datasets.push({
                type: 'bar',
                label: this.$t('statistics.OtherLoad'),
                data: points.map((point) => point.other),
                backgroundColor: 'rgba(108, 117, 125, 0.46)',
                borderColor: '#6c757d',
                borderWidth: 0,
                clip: false,
                stack: 'loads',
                barPercentage: this.isDailyPeriod ? 0.82 : 1.0,
                categoryPercentage: this.isDailyPeriod ? 0.78 : 1.0,
            });

            const config: ChartConfiguration<'bar', number[], string> = {
                type: 'bar',
                data: { labels, datasets },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'bar'>) => {
                                    const value = context.parsed.y || 0;
                                    return `${context.dataset.label}: ${unit === 'energy' ? this.formatWh(value) : this.formatWatt(value)}`;
                                },
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            stacked: true,
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: this.isDailyPeriod ? 10 : 8,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            stacked: true,
                            beginAtZero: true,
                            title: {
                                display: true,
                                text: this.isDailyPeriod ? this.$t('statistics.Energy') : this.$t('statistics.Power'),
                            },
                            ticks: {
                                callback: (value) =>
                                    unit === 'energy' ? this.formatWh(Number(value)) : this.formatWatt(Number(value)),
                            },
                        },
                    },
                },
            };

            this.loadChart?.destroy();
            this.loadChart = new ChartJS(canvas, config);
        },
        renderTemperatureChart() {
            const canvas = this.$refs.temperatureChart as HTMLCanvasElement | undefined;
            if (!canvas || this.isDailyPeriod) {
                this.temperatureChart?.destroy();
                this.temperatureChart = null;
                return;
            }

            const points = this.plotTemperatureSamples;
            const labels = this.chartLabels(points);
            const colors = [
                '#0d6efd',
                '#dc3545',
                '#198754',
                '#fd7e14',
                '#6f42c1',
                '#20c997',
                '#d63384',
                '#6c757d',
                '#0dcaf0',
                '#ffc107',
            ];
            const datasets: ChartDataset<'line', (number | null)[]>[] = this.inverters.map((inverter, index) => {
                const key = String(inverter.index);
                const color = colors[index % colors.length];
                return {
                    type: 'line',
                    label: inverter.name || inverter.serial,
                    data: points.map((sample) => sample.inverter_temperatures?.[key] ?? null),
                    borderColor: color,
                    backgroundColor: 'transparent',
                    borderWidth: 2,
                    clip: false,
                    fill: false,
                    pointRadius: 0,
                    spanGaps: true,
                    tension: 0.15,
                };
            });
            const config: ChartConfiguration<'line', (number | null)[], string> = {
                type: 'line',
                data: { labels, datasets },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'line'>) =>
                                    `${context.dataset.label}: ${this.formatTemperature(context.parsed.y ?? undefined)}`,
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: this.selectedPeriod === '7d' ? 8 : 6,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            title: {
                                display: true,
                                text: this.$t('statistics.Temperature'),
                            },
                            ticks: {
                                callback: (value) => this.formatTemperature(Number(value)),
                            },
                        },
                    },
                },
            };

            this.temperatureChart?.destroy();
            this.temperatureChart = new ChartJS(canvas, config);
        },
        renderBoostChart() {
            const canvas = this.$refs.boostChart as HTMLCanvasElement | undefined;
            if (!canvas) {
                return;
            }

            const unit = this.isDailyPeriod ? 'energy' : 'power';
            const points = this.plotBoostPoints;
            const labels = this.chartLabels(points);
            const barOptions = {
                borderWidth: 0,
                barPercentage: this.isDailyPeriod ? 0.82 : 1.0,
                categoryPercentage: this.isDailyPeriod ? 0.78 : 1.0,
                clip: false as const,
            };
            const datasets: ChartDataset<'bar' | 'line', number[]>[] = [
                {
                    type: 'bar',
                    label: this.$t('statistics.BoostSavings25A'),
                    data: points.map((point) => point.savings25A),
                    backgroundColor: 'rgba(25, 135, 84, 0.64)',
                    borderColor: '#198754',
                    hoverBackgroundColor: 'rgba(25, 135, 84, 0.88)',
                    stack: 'boost',
                    order: 20,
                    ...barOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.BoostExtra'),
                    data: points.map((point) => point.boostExtra),
                    backgroundColor: 'rgba(255, 193, 7, 0.72)',
                    borderColor: '#ffc107',
                    hoverBackgroundColor: 'rgba(255, 193, 7, 0.94)',
                    stack: 'boost',
                    order: 20,
                    ...barOptions,
                },
                {
                    type: 'bar',
                    label: this.$t('statistics.BoostRemainingImport'),
                    data: points.map((point) => point.remainingImport),
                    backgroundColor: 'rgba(220, 53, 69, 0.42)',
                    borderColor: '#dc3545',
                    hoverBackgroundColor: 'rgba(220, 53, 69, 0.72)',
                    stack: 'boost',
                    order: 30,
                    ...barOptions,
                },
                {
                    type: 'line',
                    label: this.$t('statistics.BoostRelaxLimited'),
                    data: points.map((point) => point.relaxLimited),
                    backgroundColor: 'transparent',
                    borderColor: '#6f42c1',
                    borderWidth: 2,
                    clip: false,
                    fill: false,
                    pointRadius: 0,
                    tension: 0,
                    stack: 'relax',
                    order: 0,
                },
            ];

            const config: ChartConfiguration<'bar' | 'line', number[], string> = {
                type: this.isDailyPeriod ? 'bar' : 'line',
                data: { labels, datasets },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'bar' | 'line'>) => {
                                    const value = context.parsed.y || 0;
                                    return `${context.dataset.label}: ${unit === 'energy' ? this.formatWh(value) : this.formatWatt(value)}`;
                                },
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            stacked: true,
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: this.isDailyPeriod ? 10 : 8,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            stacked: true,
                            beginAtZero: true,
                            title: {
                                display: true,
                                text: this.isDailyPeriod ? this.$t('statistics.Energy') : this.$t('statistics.Power'),
                            },
                            ticks: {
                                callback: (value) =>
                                    unit === 'energy' ? this.formatWh(Number(value)) : this.formatWatt(Number(value)),
                            },
                        },
                    },
                },
            };

            this.boostChart?.destroy();
            this.boostChart = new ChartJS(canvas, config);
        },
        renderBatteryChart() {
            const canvas = this.$refs.batteryChart as HTMLCanvasElement | undefined;
            if (!canvas) {
                return;
            }

            const points = this.isDailyPeriod
                ? this.days.map((day) => ({ t: day.t, soc: day.avg_battery_soc ?? null, plannedSoc: null }))
                : this.plotSamples.map((sample) => ({
                      t: sample.t,
                      soc: sample.battery_soc ?? null,
                      plannedSoc: sample.battery_planned_soc ?? null,
                  }));
            const labels = this.chartLabels(points);
            const hasPlannedSoc = points.some((point) => point.plannedSoc !== null);
            const datasets: ChartDataset<'line', (number | null)[]>[] = [
                {
                    label: this.$t('statistics.SoC'),
                    data: points.map((point) => point.soc),
                    borderColor: '#198754',
                    backgroundColor: 'rgba(25, 135, 84, 0.12)',
                    clip: false,
                    fill: true,
                    pointRadius: 0,
                    spanGaps: true,
                    tension: 0.15,
                },
            ];
            if (hasPlannedSoc) {
                datasets.push({
                    label: this.$t('statistics.PlannedSoc'),
                    data: points.map((point) => point.plannedSoc),
                    borderColor: '#0d6efd',
                    backgroundColor: 'transparent',
                    borderDash: [6, 4],
                    borderWidth: 2,
                    clip: false,
                    fill: false,
                    pointRadius: 0,
                    spanGaps: true,
                    tension: 0,
                });
            }
            const config: ChartConfiguration<'line', (number | null)[], string> = {
                type: 'line',
                data: {
                    labels,
                    datasets,
                },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        legend: {
                            display: hasPlannedSoc,
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = points[index];
                                    return source ? this.formatTooltipTime(source.t) : '';
                                },
                                label: (context: TooltipItem<'line'>) =>
                                    `${context.dataset.label}: ${this.formatOptionalPercent(context.parsed.y ?? undefined)}`,
                            },
                        },
                    },
                    scales: {
                        x: {
                            ...this.categoryViewportScale(points),
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: 4,
                                callback: (value) => this.formatAxisTick(points, value),
                            },
                        },
                        y: {
                            min: 0,
                            max: 100,
                            title: {
                                display: true,
                                text: 'SoC (%)',
                            },
                            ticks: {
                                callback: (value) => `${this.$n(Number(value), 'decimalNoDigits')} %`,
                            },
                        },
                    },
                },
            };

            this.batteryChart?.destroy();
            this.batteryChart = new ChartJS(canvas, config);
        },
        destroyCharts() {
            this.flowChart?.destroy();
            this.panelChart?.destroy();
            this.loadChart?.destroy();
            this.temperatureChart?.destroy();
            this.boostChart?.destroy();
            this.batteryChart?.destroy();
            this.flowChart = null;
            this.panelChart = null;
            this.loadChart = null;
            this.temperatureChart = null;
            this.boostChart = null;
            this.batteryChart = null;
        },
        sumPositive(values: number[]): number {
            return values.reduce((sum, value) => sum + Math.max(0, value || 0), 0);
        },
        measuredLoadTotal(totalValue: number | undefined, fallbackValues: number[]): number {
            if (totalValue === undefined || totalValue === null || Number.isNaN(totalValue)) {
                return this.sumPositive(fallbackValues);
            }
            return Math.max(0, totalValue);
        },
        otherLoadValue(totalLoad: number, measuredLoadTotal: number): number {
            return Math.max(0, totalLoad - measuredLoadTotal);
        },
        hasPanelPointValue(point: PanelPoint): boolean {
            return this.panelSeries.some((series) => this.panelSeriesValue(point, series.positions) !== null);
        },
        panelSeriesValue(point: PanelPoint, positions: number[]): number | null {
            let sum = 0;
            let hasValue = false;

            positions.forEach((position) => {
                const value = point.values[position];
                if (typeof value !== 'number' || !Number.isFinite(value)) {
                    return;
                }

                sum += value;
                hasValue = true;
            });

            if (!hasValue) {
                return null;
            }
            return sum;
        },
        chartLabels(points: { t: number }[]): string[] {
            return points.map((point) => String(point.t));
        },
        formatAxisTick(points: { t: number }[], value: string | number): string {
            const index = Number(value);
            if (Number.isInteger(index) && index >= 0 && index < points.length) {
                const point = points[index];
                return point ? this.formatAxisTime(point.t) : '';
            }

            const timestamp = Number(value);
            return Number.isFinite(timestamp) ? this.formatAxisTime(timestamp) : String(value);
        },
        formatAxisTime(timestamp: number): string {
            const date = new Date(timestamp * 1000);
            if (this.isDailyPeriod) {
                return new Intl.DateTimeFormat(undefined, {
                    day: '2-digit',
                    month: '2-digit',
                }).format(date);
            }

            if (this.selectedPeriod === '7d') {
                return new Intl.DateTimeFormat(undefined, {
                    weekday: 'short',
                    hour: '2-digit',
                }).format(date);
            }

            return new Intl.DateTimeFormat(undefined, {
                hour: '2-digit',
                minute: '2-digit',
            }).format(date);
        },
        formatTooltipTime(timestamp: number): string {
            return new Intl.DateTimeFormat(undefined, {
                day: '2-digit',
                month: '2-digit',
                hour: '2-digit',
                minute: '2-digit',
            }).format(new Date(timestamp * 1000));
        },
        formatWh(value: number): string {
            if (Math.abs(value) >= 1000) {
                return `${this.$n(value / 1000, 'decimalTwoDigits')} kWh`;
            }
            return `${this.$n(value, 'decimalNoDigits')} Wh`;
        },
        formatSignedWh(value: number): string {
            const formatted = this.formatWh(value);
            return value > 0 ? `+${formatted}` : formatted;
        },
        formatWatt(value: number): string {
            return `${this.$n(value, 'decimalNoDigits')} W`;
        },
        formatDuration(seconds: number): string {
            if (seconds >= 3600) {
                return `${this.$n(seconds / 3600, 'decimalOneDigit')} h`;
            }
            if (seconds >= 60) {
                return `${this.$n(seconds / 60, 'decimalOneDigit')} min`;
            }
            return `${this.$n(seconds, 'decimalNoDigits')} s`;
        },
        formatOptionalPercent(value?: number): string {
            if (value === undefined || value === null || Number.isNaN(value)) {
                return '-';
            }
            return `${this.$n(value, 'decimalOneDigit')} %`;
        },
        formatTemperature(value?: number | null): string {
            if (value === undefined || value === null || Number.isNaN(value)) {
                return '-';
            }
            return `${this.$n(value, 'decimalOneDigit')} °C`;
        },
    },
});
</script>

<style scoped>
.statistics-page {
    block-size: calc(100vh - 5.5rem);
    block-size: calc(100dvh - 5.5rem);
    display: flex;
    flex-direction: column;
    min-block-size: 0;
}

.statistic-card {
    min-height: 92px;
}

.statistics-toolbar {
    align-items: center;
    display: flex;
    flex-wrap: wrap;
    gap: 0.75rem;
    justify-content: space-between;
}

.range-controls {
    align-items: center;
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
}

.range-date-input {
    inline-size: auto;
    min-inline-size: 13.5rem;
}

.icon-button {
    align-items: center;
    display: inline-flex;
    justify-content: center;
    min-inline-size: 2rem;
}

.statistics-range-label {
    min-block-size: 1.25rem;
}

.chart-panel-card {
    flex: 1 1 auto;
    display: grid;
    grid-template-rows: auto minmax(0, 1fr);
    min-block-size: 0;
}

.chart-panel-card > .card-body {
    align-items: stretch;
    display: grid;
    grid-template-rows: minmax(0, 1fr);
    justify-items: stretch;
    min-block-size: 0;
}

.chart-tab-panel {
    align-self: stretch;
    block-size: 100%;
    display: grid;
    grid-template-rows: minmax(0, 1fr);
    inline-size: 100%;
    justify-self: stretch;
    min-block-size: 0;
}

.chart-tab-panel-with-controls {
    grid-template-rows: auto minmax(0, 1fr);
    row-gap: 0.75rem;
}

.panel-chart-controls {
    align-items: center;
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
}

.panel-mode-toggle {
    flex: 0 0 auto;
}

.panel-inverter-toggle-group {
    display: flex;
    flex: 1 1 18rem;
    flex-wrap: wrap;
    gap: 0.35rem;
    min-inline-size: 0;
}

.panel-inverter-toggle {
    max-inline-size: 14rem;
    overflow: hidden;
    text-overflow: ellipsis;
}

.chart-host {
    block-size: 100%;
    cursor: grab;
    min-block-size: 0;
    overflow: hidden;
    position: relative;
    inline-size: 100%;
    touch-action: pan-y;
    user-select: none;
}

.chart-host-dragging {
    cursor: grabbing;
}

:global(html.statistics-range-dragging),
:global(html.statistics-range-dragging *) {
    cursor: grabbing !important;
}

.chart-host-battery {
    min-block-size: 0;
}

.battery-metrics {
    display: flex;
    flex-direction: column;
    justify-content: center;
}

.metric-line {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    padding: 0.5rem 0;
    border-bottom: 1px solid var(--bs-border-color);
}
</style>
