<template>
    <div class="power-limiter-trace-panel">
        <div class="d-flex flex-wrap align-items-center gap-2 mb-3">
            <button
                class="btn btn-primary"
                type="button"
                :disabled="traceLoading || traceFetchInFlight || !isTraceLiveWindow"
                @click="getTraceData"
            >
                <BIconArrowClockwise :class="{ 'power-limiter-trace-spin': traceLoading }" />
                {{ $t('powerlimitertrace.Refresh') }}
            </button>

            <div class="btn-group" role="group" :aria-label="traceText('WindowNavigation')">
                <button
                    class="btn btn-outline-secondary"
                    type="button"
                    :disabled="traceLoading || !canShowOlderTraceWindow"
                    :aria-label="traceText('WindowOlder', { minutes: traceWindowMinutes })"
                    :title="traceText('WindowOlder', { minutes: traceWindowMinutes })"
                    @click="showOlderTraceWindow"
                >
                    <BIconChevronLeft />
                </button>
                <button
                    class="btn btn-outline-secondary"
                    type="button"
                    :disabled="traceLoading || !canShowNewerTraceWindow"
                    :aria-label="traceText('WindowNewer', { minutes: traceWindowMinutes })"
                    :title="traceText('WindowNewer', { minutes: traceWindowMinutes })"
                    @click="showNewerTraceWindow"
                >
                    <BIconChevronRight />
                </button>
                <button
                    class="btn btn-outline-secondary"
                    type="button"
                    :disabled="traceLoading || traceOffsetSeconds === 0"
                    @click="showLatestTraceWindow"
                >
                    {{ $t('powerlimitertrace.WindowLatestButton') }}
                </button>
            </div>

            <div
                class="d-flex align-items-center gap-2 power-limiter-trace-window-selector"
                role="group"
                :aria-label="traceText('WindowDuration')"
            >
                <button
                    v-for="seconds in traceWindowOptions"
                    :key="seconds"
                    class="btn"
                    :class="seconds === traceSelectedWindowSeconds ? 'btn-primary' : 'btn-outline-primary'"
                    type="button"
                    :aria-pressed="seconds === traceSelectedWindowSeconds"
                    @click="setTraceSelectedWindowSeconds(seconds)"
                >
                    {{ formatTraceWindowOption(seconds) }}
                </button>
            </div>

            <div class="form-check form-switch ms-1">
                <input
                    id="power-limiter-trace-auto-refresh"
                    class="form-check-input"
                    type="checkbox"
                    v-model="traceAutoRefresh"
                    :disabled="!isTraceLiveWindow"
                />
                <label class="form-check-label" for="power-limiter-trace-auto-refresh">
                    {{ $t('powerlimitertrace.AutoRefresh') }}
                </label>
            </div>

            <span class="badge text-bg-secondary">
                {{
                    $t('powerlimitertrace.SampleCount', {
                        count: traceVisibleSampleCount,
                        capacity: traceData?.capacity || 0,
                    })
                }}
            </span>
            <span class="badge text-bg-secondary">{{ formatTraceWindowLabel() }}</span>
        </div>

        <div v-if="!traceLoading && traceCacheSampleCount === 0" class="alert alert-secondary mb-0">
            {{ $t('powerlimitertrace.NoSamples') }}
        </div>

        <div
            v-show="traceData && traceCacheSampleCount > 0"
            class="power-limiter-trace-chart"
            :class="{ 'power-limiter-trace-chart-dragging': traceDrag.active }"
            @pointerdown="startTraceDrag"
            @pointermove="moveTraceDrag"
            @pointerup="endTraceDrag"
            @pointercancel="cancelTraceDragPointer"
        >
            <canvas ref="traceChart"></canvas>
        </div>
    </div>
</template>

<script lang="ts">
import { defineComponent, markRaw } from 'vue';
import { BIconArrowClockwise, BIconChevronLeft, BIconChevronRight } from 'bootstrap-icons-vue';
import {
    CategoryScale,
    Chart as ChartJS,
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
import { authHeader, handleResponse } from '@/utils/authentication';

interface PowerLimiterTraceConfidenceBandPoint {
    x: number;
    min: number | null;
    max: number | null;
}

interface PowerLimiterTraceDragPluginOptions {
    confidenceBand?: PowerLimiterTraceConfidenceBandPoint[];
}

type PowerLimiterTraceDragChart = ChartJS & { $powerLimiterTraceDragOffsetPx?: number };

const clippedTraceCharts = new WeakSet<object>();
const traceConfidenceBandFillColor = 'rgba(255, 99, 132, 0.14)';

function drawTraceConfidenceBand(chart: ChartJS, points?: PowerLimiterTraceConfidenceBandPoint[]) {
    if (!points || points.length === 0) {
        return;
    }

    const xScale = chart.scales.x;
    const yScale = chart.scales.y;
    if (!xScale || !yScale) {
        return;
    }

    const { ctx } = chart;
    ctx.save();
    const segment: Array<{ x: number; min: number; max: number }> = [];
    const drawSegment = () => {
        if (segment.length < 2) {
            segment.length = 0;
            return;
        }

        ctx.beginPath();
        segment.forEach((point, index) => {
            const x = xScale.getPixelForValue(point.x);
            const y = yScale.getPixelForValue(point.min);
            if (index === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        });
        for (let index = segment.length - 1; index >= 0; index--) {
            const point = segment[index];
            if (!point) {
                continue;
            }
            ctx.lineTo(xScale.getPixelForValue(point.x), yScale.getPixelForValue(point.max));
        }
        ctx.closePath();
        ctx.fillStyle = traceConfidenceBandFillColor;
        ctx.fill();
        segment.length = 0;
    };

    points.forEach((point) => {
        if (point.min === null || point.max === null) {
            drawSegment();
            return;
        }

        const previousPoint = segment[segment.length - 1];
        if (previousPoint && Math.abs(point.x - previousPoint.x) > 2.5) {
            drawSegment();
        }

        segment.push({
            x: point.x,
            min: Math.min(point.min, point.max),
            max: Math.max(point.min, point.max),
        });
    });
    drawSegment();
    ctx.restore();
}

const powerLimiterTraceDragPlugin: Plugin = {
    id: 'powerLimiterTraceDrag',
    beforeDatasetsDraw(chart) {
        const offset = (chart as PowerLimiterTraceDragChart).$powerLimiterTraceDragOffsetPx || 0;
        const { ctx, chartArea } = chart;
        const pluginOptions = (chart.options.plugins as { powerLimiterTraceDrag?: PowerLimiterTraceDragPluginOptions })
            ?.powerLimiterTraceDrag;

        ctx.save();
        ctx.beginPath();
        ctx.rect(chartArea.left, chartArea.top, chartArea.right - chartArea.left, chartArea.bottom - chartArea.top);
        ctx.clip();
        if (Math.abs(offset) >= 0.1) {
            ctx.translate(offset, 0);
        }
        drawTraceConfidenceBand(chart, pluginOptions?.confidenceBand);
        clippedTraceCharts.add(chart);
    },
    afterDatasetsDraw(chart) {
        if (!clippedTraceCharts.has(chart)) {
            return;
        }

        chart.ctx.restore();
        clippedTraceCharts.delete(chart);
    },
};

ChartJS.register(
    CategoryScale,
    Legend,
    LineController,
    LineElement,
    LinearScale,
    PointElement,
    Tooltip,
    powerLimiterTraceDragPlugin
);

interface PowerLimiterTraceProvider {
    out: number;
    exp: number;
    lim: number;
    elim: number;
    pending: number;
    assumed: number;
}

interface PowerLimiterTraceForecast {
    valid: boolean;
    virtual?: number;
    min?: number;
    nominal?: number;
    max?: number;
    pending?: number;
    due_min?: number;
    due_nominal?: number;
    due_max?: number;
    due_age_ms?: number;
    oracle?: number;
    horizon_ms?: number;
    oracle_age_ms?: number;
}

interface PowerLimiterTracePredictive {
    storage?: PowerLimiterTraceForecast;
    global?: PowerLimiterTraceForecast;
    storage_target?: PowerLimiterTraceForecast;
    charger_active?: boolean;
    storage_output_active?: boolean;
    storage_charger_overlap?: boolean;
    ledger?: number;
    active?: number;
}

interface PowerLimiterTraceSample {
    ms: number;
    pm: number | null;
    target: number;
    storage: number;
    expected?: number;
    battery_bms?: number;
    status: number;
    charger: {
        in: number;
        target?: number;
        exp?: number;
        max: number;
    };
    battery: PowerLimiterTraceProvider;
    solar: PowerLimiterTraceProvider;
    smart: PowerLimiterTraceProvider;
    predictive?: PowerLimiterTracePredictive;
}

interface PowerLimiterTraceResponse {
    now_ms: number;
    now_ts?: number;
    capacity: number;
    history_s?: number;
    window_s?: number;
    offset_s?: number;
    range_from_age_s?: number;
    range_to_age_s?: number;
    available_s?: number;
    sample_count_total?: number;
    psram?: boolean;
    samples: PowerLimiterTraceSample[];
}

type PowerLimiterTraceCompactSample = Array<number | boolean | null>;

interface PowerLimiterTraceWireResponse extends Omit<PowerLimiterTraceResponse, 'samples'> {
    format?: string;
    samples: Array<PowerLimiterTraceSample | PowerLimiterTraceCompactSample>;
}

interface TraceRange {
    from: number;
    to: number;
}

interface TraceDragState {
    active: boolean;
    pointerId: number | null;
    startX: number;
    currentX: number;
    width: number;
    baseOffset: number;
}

interface TraceChartPoint {
    x: number;
    y: number | null;
}

type PowerLimiterTraceChartPluginOptions = NonNullable<
    NonNullable<ChartConfiguration<'line', TraceChartPoint[], number>['options']>['plugins']
> & {
    powerLimiterTraceDrag?: PowerLimiterTraceDragPluginOptions;
};

type PowerLimiterTraceDataset = ChartDataset<'line', TraceChartPoint[]> & {
    traceKey: string;
};

const traceDatasetVisibilityStorageKey = 'powerLimiterTraceDatasetHidden.v1';
const traceWindowSecondsStorageKey = 'powerLimiterTraceWindowSeconds.v1';
const traceDefaultWindowSeconds = 5 * 60;
const traceWindowOptions = [traceDefaultWindowSeconds, 15 * 60, 30 * 60];
const traceFetchRangeMaxSeconds = traceDefaultWindowSeconds;
const traceIncrementalRefreshMinSeconds = 3;
const traceIncrementalRefreshOverlapSeconds = 2;
const traceAutoRefreshIntervalMillis = 1_000;
const traceDragCacheRenderMillis = 80;

function normalizeTraceWindowSeconds(value: number | string): number {
    const parsed = Number(value);
    return traceWindowOptions.includes(parsed) ? parsed : traceDefaultWindowSeconds;
}

function readTraceWindowSeconds(): number {
    try {
        return normalizeTraceWindowSeconds(
            localStorage.getItem(traceWindowSecondsStorageKey) || traceDefaultWindowSeconds
        );
    } catch {
        return traceDefaultWindowSeconds;
    }
}

export default defineComponent({
    components: {
        BIconArrowClockwise,
        BIconChevronLeft,
        BIconChevronRight,
    },
    props: {
        active: {
            type: Boolean,
            default: true,
        },
    },
    data() {
        return {
            traceData: null as PowerLimiterTraceResponse | null,
            traceChart: null as ChartJS<'line', TraceChartPoint[], number> | null,
            traceSamplesByMillis: {} as Record<string, PowerLimiterTraceSample>,
            traceTooltipSamples: [] as PowerLimiterTraceSample[],
            traceFetchedRanges: [] as TraceRange[],
            traceLoading: false,
            traceFetchInFlight: false,
            traceFetchAbortController: null as AbortController | null,
            traceQueuedRange: null as TraceRange | null,
            traceLastResponseClientMillis: null as number | null,
            traceLastResponseClientEpochSeconds: null as number | null,
            tracePrefetchTimer: undefined as number | undefined,
            traceSelectedWindowSeconds: readTraceWindowSeconds(),
            traceAutoRefresh: true,
            traceRefreshTimer: undefined as number | undefined,
            traceOffsetSeconds: 0,
            traceDrag: {
                active: false,
                pointerId: null,
                startX: 0,
                currentX: 0,
                width: 0,
                baseOffset: 0,
            } as TraceDragState,
            traceDragEnding: false,
            traceDragElement: null as HTMLElement | null,
            traceDragDrawFrame: undefined as number | undefined,
            traceDragRenderTimer: undefined as number | undefined,
        };
    },
    computed: {
        traceWindowOptions(): number[] {
            return traceWindowOptions;
        },
        traceWindowSeconds(): number {
            return this.traceSelectedWindowSeconds;
        },
        traceWindowMinutes(): number {
            return Math.round(this.traceWindowSeconds / 60);
        },
        traceHistorySeconds(): number {
            return this.traceData?.history_s || this.traceData?.capacity || this.traceWindowSeconds;
        },
        traceAvailableSeconds(): number {
            return this.traceData?.available_s || this.traceData?.sample_count_total || 0;
        },
        canShowOlderTraceWindow(): boolean {
            return this.traceOffsetSeconds + this.traceWindowSeconds < this.traceHistorySeconds;
        },
        canShowNewerTraceWindow(): boolean {
            return this.traceOffsetSeconds > 0;
        },
        isTraceLiveWindow(): boolean {
            return this.traceOffsetSeconds === 0;
        },
        traceCacheSamples(): PowerLimiterTraceSample[] {
            return Object.values(this.traceSamplesByMillis);
        },
        traceCacheSampleCount(): number {
            return this.traceCacheSamples.length;
        },
        traceVisibleSampleCount(): number {
            return this.traceSamplesForAgeRange(
                this.traceOffsetSeconds,
                this.traceOffsetSeconds + this.traceWindowSeconds
            ).length;
        },
    },
    mounted() {
        if (this.active) {
            this.activate();
        }
    },
    beforeUnmount() {
        this.abortTraceFetch();
        this.stopTraceDragTransport();
        this.destroyTraceChart();
        this.stopTraceAutoRefresh();
        if (this.tracePrefetchTimer !== undefined) {
            window.clearTimeout(this.tracePrefetchTimer);
            this.tracePrefetchTimer = undefined;
        }
    },
    watch: {
        active(enabled: boolean) {
            if (enabled) {
                this.activate();
                return;
            }
            this.stopTraceAutoRefresh();
            this.cancelTraceDrag();
        },
        traceAutoRefresh() {
            this.updateTraceAutoRefreshState();
        },
    },
    methods: {
        activate() {
            if (!this.traceData && !this.traceLoading) {
                this.getTraceData();
            }
            this.resizeTraceChartAfterLayout();
            this.updateTraceAutoRefreshState();
        },
        getTraceData() {
            if (this.traceLoading || this.traceFetchInFlight || !this.isTraceLiveWindow) {
                return Promise.resolve();
            }

            if (this.traceData && this.traceCacheSampleCount > 0) {
                return this.fetchTraceRange(0, this.incrementalTraceRefreshRangeSeconds(), true);
            }

            this.resetTraceCache();
            this.traceOffsetSeconds = 0;
            return this.fetchTraceRange(0, this.initialTraceRangeSeconds(), true);
        },
        initialTraceRangeSeconds() {
            return this.traceWindowSeconds;
        },
        incrementalTraceRefreshRangeSeconds() {
            if (this.traceLastResponseClientMillis === null) {
                return this.initialTraceRangeSeconds();
            }

            const elapsedSeconds = Math.ceil(
                Math.max(0, performance.now() - this.traceLastResponseClientMillis) / 1000
            );
            return Math.min(
                this.initialTraceRangeSeconds(),
                Math.max(traceIncrementalRefreshMinSeconds, elapsedSeconds + traceIncrementalRefreshOverlapSeconds)
            );
        },
        runTraceAutoRefreshCycle() {
            this.traceRefreshTimer = undefined;
            if (!this.active || !this.traceAutoRefresh || !this.isTraceLiveWindow) {
                return;
            }
            if (this.traceFetchInFlight || this.traceLoading) {
                this.scheduleTraceAutoRefreshCycle();
                return;
            }

            this.fetchTraceRange(0, this.incrementalTraceRefreshRangeSeconds(), false)
                .catch(() => undefined)
                .finally(() => {
                    if (!this.active || !this.traceAutoRefresh || !this.isTraceLiveWindow) {
                        return;
                    }
                    this.scheduleTraceAutoRefreshCycle();
                });
        },
        updateTraceAutoRefreshState() {
            if (!this.traceAutoRefresh || !this.active || !this.isTraceLiveWindow) {
                this.stopTraceAutoRefresh();
                return;
            }

            this.scheduleTraceAutoRefreshCycle();
        },
        scheduleTraceAutoRefreshCycle() {
            if (
                !this.traceAutoRefresh ||
                !this.active ||
                !this.isTraceLiveWindow ||
                this.traceRefreshTimer !== undefined
            ) {
                return;
            }

            this.traceRefreshTimer = window.setTimeout(
                () => this.runTraceAutoRefreshCycle(),
                traceAutoRefreshIntervalMillis
            );
        },
        stopTraceAutoRefresh() {
            if (this.traceRefreshTimer !== undefined) {
                window.clearTimeout(this.traceRefreshTimer);
                this.traceRefreshTimer = undefined;
            }
        },
        abortTraceFetch() {
            this.traceFetchAbortController?.abort();
            this.traceFetchAbortController = null;
        },
        resetTraceCache() {
            this.traceSamplesByMillis = {};
            this.traceFetchedRanges = [];
            this.traceQueuedRange = null;
            this.traceLastResponseClientMillis = null;
            this.traceLastResponseClientEpochSeconds = null;
        },
        fetchTraceRange(fromAgeSeconds: number, toAgeSeconds: number, showLoading: boolean): Promise<void> {
            const range = this.normalizeTraceRange(fromAgeSeconds, toAgeSeconds);
            if (range.to <= range.from) {
                return Promise.resolve();
            }
            if (this.traceFetchInFlight) {
                this.traceQueuedRange = this.mergeQueuedTraceRange(this.traceQueuedRange, range);
                return Promise.resolve();
            }

            const requestRange = {
                from: range.from,
                to: Math.min(range.to, range.from + traceFetchRangeMaxSeconds),
            };
            let completed = false;
            let aborted = false;
            const abortController = new AbortController();
            this.traceFetchInFlight = true;
            this.traceFetchAbortController = abortController;
            if (showLoading) {
                this.traceLoading = true;
            }

            const query = new URLSearchParams({
                from_age_s: String(requestRange.from),
                to_age_s: String(requestRange.to),
                window_s: String(this.traceWindowSeconds),
                format: 'compact-v2',
            });

            return fetch(`/api/powerlimiter/trace?${query.toString()}`, {
                headers: authHeader(),
                signal: abortController.signal,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data: PowerLimiterTraceWireResponse) => {
                    if (abortController.signal.aborted) {
                        aborted = true;
                        return Promise.resolve();
                    }
                    this.applyTraceResponse(this.normalizeTraceResponse(data));
                    completed = true;
                    return this.$nextTick();
                })
                .then(() => {
                    if (abortController.signal.aborted || !completed) {
                        return;
                    }
                    this.renderTraceChart();
                    if (this.traceDrag.active) {
                        this.updateTraceChartDragOffset();
                    }
                })
                .catch((error) => {
                    if (abortController.signal.aborted || (error instanceof Error && error.name === 'AbortError')) {
                        aborted = true;
                        return;
                    }
                    throw error;
                })
                .finally(() => {
                    if (this.traceFetchAbortController === abortController) {
                        this.traceFetchAbortController = null;
                    }
                    this.traceFetchInFlight = false;
                    if (showLoading) {
                        this.traceLoading = false;
                    }
                    if (aborted) {
                        return;
                    }
                    const queuedRange = this.traceQueuedRange;
                    this.traceQueuedRange = null;
                    let followUpRange = queuedRange;
                    if (completed && this.traceMissingRanges(range.from, range.to).length > 0) {
                        followUpRange = this.mergeQueuedTraceRange(followUpRange, range);
                    }
                    if (followUpRange) {
                        this.fetchMissingTraceRange(followUpRange.from, followUpRange.to, queuedRange ? 0 : 20);
                    } else {
                        this.updateTraceAutoRefreshState();
                    }
                });
        },
        applyTraceResponse(data: PowerLimiterTraceResponse) {
            if (this.traceData) {
                this.shiftTraceFetchedRanges(this.traceNowDeltaSeconds(this.traceData.now_ms, data.now_ms));
            }

            this.traceData = data;
            data.samples.forEach((sample) => {
                this.traceSamplesByMillis[String(sample.ms)] = sample;
            });
            this.addTraceFetchedRange(
                data.range_from_age_s ?? data.offset_s ?? 0,
                data.range_to_age_s ?? (data.offset_s ?? 0) + this.traceWindowSeconds
            );
            this.pruneTraceCache();
            this.traceOffsetSeconds = this.clampTraceOffset(this.traceOffsetSeconds);
            this.traceLastResponseClientMillis = performance.now();
            this.traceLastResponseClientEpochSeconds = Date.now() / 1000;
            this.updateTraceAutoRefreshState();
        },
        traceNowDeltaSeconds(previousNowMs: number, nextNowMs: number) {
            return Math.floor(((nextNowMs - previousNowMs) >>> 0) / 1000);
        },
        shiftTraceFetchedRanges(deltaSeconds: number) {
            if (deltaSeconds <= 0) {
                return;
            }

            const historySeconds = this.traceHistorySeconds;
            this.traceFetchedRanges = this.traceFetchedRanges
                .map((range) => ({
                    from: Math.min(historySeconds, range.from + deltaSeconds),
                    to: Math.min(historySeconds, range.to + deltaSeconds),
                }))
                .filter((range) => range.to > range.from);
        },
        pruneTraceCache() {
            const nowMs = this.traceData?.now_ms;
            if (nowMs === undefined) {
                return;
            }

            const maxAge = this.traceHistorySeconds + this.traceWindowSeconds;
            Object.entries(this.traceSamplesByMillis).forEach(([key, sample]) => {
                if (this.traceSampleAgeSeconds(sample, nowMs) > maxAge) {
                    delete this.traceSamplesByMillis[key];
                }
            });
        },
        normalizeTraceRange(fromAgeSeconds: number, toAgeSeconds: number): TraceRange {
            const maxSeconds = this.traceData
                ? Math.max(this.traceWindowSeconds, this.traceHistorySeconds)
                : Math.max(this.traceWindowSeconds, this.initialTraceRangeSeconds(), toAgeSeconds);
            const from = Math.max(0, Math.floor(fromAgeSeconds));
            const to = Math.min(Math.max(from, Math.ceil(toAgeSeconds)), maxSeconds);
            return { from, to };
        },
        mergeQueuedTraceRange(current: TraceRange | null, next: TraceRange): TraceRange {
            if (!current) {
                return next;
            }

            return {
                from: Math.min(current.from, next.from),
                to: Math.max(current.to, next.to),
            };
        },
        addTraceFetchedRange(fromAgeSeconds: number, toAgeSeconds: number) {
            const range = this.normalizeTraceRange(fromAgeSeconds, toAgeSeconds);
            if (range.to <= range.from) {
                return;
            }

            const ranges = [...this.traceFetchedRanges, range].sort((left, right) => left.from - right.from);
            const merged: TraceRange[] = [];
            ranges.forEach((candidate) => {
                const previous = merged[merged.length - 1];
                if (!previous || candidate.from > previous.to + 1) {
                    merged.push({ ...candidate });
                    return;
                }

                previous.to = Math.max(previous.to, candidate.to);
            });
            this.traceFetchedRanges = merged;
        },
        traceMissingRanges(fromAgeSeconds: number, toAgeSeconds: number): TraceRange[] {
            const target = this.normalizeTraceRange(fromAgeSeconds, toAgeSeconds);
            if (target.to <= target.from) {
                return [];
            }

            const missing: TraceRange[] = [];
            let cursor = target.from;
            this.traceFetchedRanges
                .filter((range) => range.to > target.from && range.from < target.to)
                .sort((left, right) => left.from - right.from)
                .forEach((range) => {
                    if (range.from > cursor) {
                        missing.push({ from: cursor, to: Math.min(range.from, target.to) });
                    }
                    cursor = Math.max(cursor, range.to);
                });

            if (cursor < target.to) {
                missing.push({ from: cursor, to: target.to });
            }
            return missing.filter((range) => range.to > range.from);
        },
        fetchMissingTraceRange(fromAgeSeconds: number, toAgeSeconds: number, delay = 40) {
            if (this.tracePrefetchTimer !== undefined) {
                window.clearTimeout(this.tracePrefetchTimer);
                this.tracePrefetchTimer = undefined;
            }

            this.tracePrefetchTimer = window.setTimeout(() => {
                this.tracePrefetchTimer = undefined;
                const missing = this.traceMissingRanges(fromAgeSeconds, toAgeSeconds);
                if (missing.length === 0) {
                    return;
                }

                const range = missing.reduce(
                    (acc, item) => ({
                        from: Math.min(acc.from, item.from),
                        to: Math.max(acc.to, item.to),
                    }),
                    missing[0] as TraceRange
                );
                this.fetchTraceRange(range.from, range.to, false).catch(() => undefined);
            }, delay);
        },
        traceCacheTargetRange(offsetSeconds?: number): TraceRange {
            const targetOffsetSeconds = offsetSeconds ?? this.traceOffsetSeconds;
            const padding = Math.floor(this.traceWindowSeconds / 2);
            return this.normalizeTraceRange(
                Math.max(0, targetOffsetSeconds - padding),
                targetOffsetSeconds + this.traceWindowSeconds + padding
            );
        },
        ensureTraceCacheForOffset(offsetSeconds: number, delay = 40) {
            if (!this.traceData) {
                return;
            }

            const range = this.traceCacheTargetRange(offsetSeconds);
            this.fetchMissingTraceRange(range.from, range.to, delay);
        },
        ensureTraceCacheForCurrentViewport(delay = 40) {
            this.ensureTraceCacheForOffset(this.traceOffsetSeconds, delay);
        },
        setTraceWindowOffset(offsetSeconds: number) {
            const normalizedOffset = this.clampTraceOffset(Math.round(offsetSeconds));
            if (normalizedOffset === this.traceOffsetSeconds) {
                return Promise.resolve();
            }

            this.traceOffsetSeconds = normalizedOffset;
            this.renderTraceChart();
            this.ensureTraceCacheForCurrentViewport(40);
            this.updateTraceAutoRefreshState();
            return Promise.resolve();
        },
        clampTraceOffset(offsetSeconds: number): number {
            const maxOffsetSeconds = Math.max(0, this.traceHistorySeconds - this.traceWindowSeconds);
            return Math.min(Math.max(0, Math.round(offsetSeconds)), maxOffsetSeconds);
        },
        showOlderTraceWindow() {
            return this.setTraceWindowOffset(this.traceOffsetSeconds + this.traceWindowSeconds);
        },
        showNewerTraceWindow() {
            return this.setTraceWindowOffset(this.traceOffsetSeconds - this.traceWindowSeconds);
        },
        showLatestTraceWindow() {
            return this.setTraceWindowOffset(0).then(() => this.ensureTraceCacheForCurrentViewport(0));
        },
        startTraceDrag(event: PointerEvent) {
            if (
                !this.traceData ||
                (event.pointerType === 'mouse' && event.button !== 0) ||
                this.isPointerInTraceLegend(event)
            ) {
                return;
            }

            const target = event.currentTarget as HTMLElement;
            this.cancelTraceDrag();
            this.traceDragEnding = false;
            window.addEventListener('blur', this.cancelTraceDragFromEvent, true);
            document.addEventListener('visibilitychange', this.cancelTraceDragOnHidden, true);
            this.traceDragElement = target;
            target.classList.add('power-limiter-trace-chart-dragging');
            this.traceDrag = {
                active: true,
                pointerId: event.pointerId,
                startX: event.clientX,
                currentX: event.clientX,
                width: Math.max(1, target.clientWidth),
                baseOffset: this.traceOffsetSeconds,
            };
            try {
                target.setPointerCapture(event.pointerId);
            } catch {
                // Pointer capture is best-effort; window listeners below still finish the drag.
            }
            window.addEventListener('pointermove', this.moveTraceDrag, true);
            window.addEventListener('pointerup', this.endTraceDrag, true);
            window.addEventListener('pointercancel', this.cancelTraceDragPointer, true);
            window.addEventListener('mouseup', this.endTraceDragMouseFallback, true);
            event.preventDefault();
        },
        isPointerInTraceLegend(event: PointerEvent) {
            const canvas = this.$refs.traceChart as HTMLCanvasElement | undefined;
            const chart = this.traceChart;
            const legend = chart?.legend;
            if (!canvas || !legend || !legend.options.display) {
                return false;
            }

            const rect = canvas.getBoundingClientRect();
            const scaleX = chart.width / Math.max(1, rect.width);
            const scaleY = chart.height / Math.max(1, rect.height);
            const x = (event.clientX - rect.left) * scaleX;
            const y = (event.clientY - rect.top) * scaleY;

            return x >= legend.left && x <= legend.right && y >= legend.top && y <= legend.bottom;
        },
        moveTraceDrag(event: PointerEvent) {
            if (this.traceDragEnding || !this.traceDrag.active || this.traceDrag.pointerId !== event.pointerId) {
                return;
            }

            this.traceDrag.currentX = event.clientX;
            this.applyTraceDrag();
            event.preventDefault();
        },
        endTraceDrag(event: PointerEvent) {
            if (this.traceDragEnding || !this.traceDrag.active || this.traceDrag.pointerId !== event.pointerId) {
                return;
            }

            this.finishTraceDrag(event.clientX);
            event.preventDefault();
        },
        endTraceDragMouseFallback(event: MouseEvent) {
            if (this.traceDragEnding || !this.traceDrag.active || this.traceDrag.pointerId === null) {
                return;
            }

            this.finishTraceDrag(event.clientX);
            event.preventDefault();
        },
        finishTraceDrag(clientX: number) {
            this.traceDragEnding = true;
            this.traceDrag.currentX = clientX;
            const finalOffset = this.traceDragTargetOffsetSeconds();
            this.stopTraceDragTransport();
            this.resetTraceDragState(finalOffset === this.traceOffsetSeconds);
            this.setTraceWindowOffset(finalOffset).then(() => this.ensureTraceCacheForCurrentViewport(0));
        },
        cancelTraceDragPointer(event: PointerEvent) {
            if (!this.traceDrag.active || this.traceDrag.pointerId !== event.pointerId) {
                return;
            }
            if (event.pointerType === 'mouse') {
                event.preventDefault();
                return;
            }

            this.cancelTraceDrag();
            event.preventDefault();
        },
        cancelTraceDragFromEvent(event: Event) {
            this.cancelTraceDrag();
            event.preventDefault();
        },
        cancelTraceDragOnHidden() {
            if (document.visibilityState !== 'visible') {
                this.cancelTraceDrag();
            }
        },
        cancelTraceDrag() {
            if (!this.traceDrag.active) {
                return;
            }

            this.stopTraceDragTransport();
            this.resetTraceDragState();
        },
        stopTraceDragTransport() {
            window.removeEventListener('pointermove', this.moveTraceDrag, true);
            window.removeEventListener('pointerup', this.endTraceDrag, true);
            window.removeEventListener('pointercancel', this.cancelTraceDragPointer, true);
            window.removeEventListener('mouseup', this.endTraceDragMouseFallback, true);
            window.removeEventListener('blur', this.cancelTraceDragFromEvent, true);
            document.removeEventListener('visibilitychange', this.cancelTraceDragOnHidden, true);
            if (this.traceDragRenderTimer !== undefined) {
                window.clearTimeout(this.traceDragRenderTimer);
                this.traceDragRenderTimer = undefined;
            }
        },
        resetTraceDragState(redraw = true) {
            this.traceDragElement?.classList.remove('power-limiter-trace-chart-dragging');
            this.traceDragElement = null;
            this.traceDragEnding = false;
            this.traceDrag = {
                active: false,
                pointerId: null,
                startX: 0,
                currentX: 0,
                width: 0,
                baseOffset: this.traceOffsetSeconds,
            };
            if (this.traceDragDrawFrame !== undefined) {
                window.cancelAnimationFrame(this.traceDragDrawFrame);
                this.traceDragDrawFrame = undefined;
            }
            if (redraw) {
                this.updateTraceChartDragOffset();
            }
        },
        applyTraceDrag() {
            if (this.traceDrag.width <= 0) {
                return;
            }

            this.scheduleTraceChartDragOffsetUpdate();
            this.ensureTraceCacheForOffset(this.traceDragTargetOffsetSeconds(), traceDragCacheRenderMillis);
            this.scheduleTraceDragCachedRender();
        },
        traceDragTargetOffsetSeconds() {
            if (this.traceDrag.width <= 0) {
                return this.traceDrag.baseOffset;
            }

            const deltaX = this.traceDrag.currentX - this.traceDrag.startX;
            const shiftSeconds = (deltaX / this.traceDrag.width) * this.traceWindowSeconds;
            return this.clampTraceOffset(this.traceDrag.baseOffset + shiftSeconds);
        },
        traceDragOffsetPixels(): number {
            if (!this.traceDrag.active || this.traceDrag.width <= 0) {
                return 0;
            }

            return (
                ((this.traceDragTargetOffsetSeconds() - this.traceOffsetSeconds) / this.traceWindowSeconds) *
                this.traceDrag.width
            );
        },
        updateTraceChartDragOffset() {
            const chart = this.traceChart as PowerLimiterTraceDragChart | null;
            if (!chart) {
                return;
            }

            chart.$powerLimiterTraceDragOffsetPx = this.traceDragOffsetPixels();
            chart.draw();
        },
        scheduleTraceChartDragOffsetUpdate() {
            if (this.traceDragDrawFrame !== undefined) {
                return;
            }

            this.traceDragDrawFrame = window.requestAnimationFrame(() => {
                this.traceDragDrawFrame = undefined;
                this.updateTraceChartDragOffset();
            });
        },
        scheduleTraceDragCachedRender() {
            if (this.traceDragRenderTimer !== undefined || this.traceDragEnding) {
                return;
            }

            this.traceDragRenderTimer = window.setTimeout(() => {
                this.traceDragRenderTimer = undefined;
                this.renderTraceDragCachedWindow();
            }, traceDragCacheRenderMillis);
        },
        renderTraceDragCachedWindow() {
            if (!this.traceDrag.active || this.traceDragEnding) {
                return;
            }

            const targetOffset = this.traceDragTargetOffsetSeconds();
            if (targetOffset !== this.traceOffsetSeconds) {
                this.traceOffsetSeconds = targetOffset;
                this.renderTraceChart();
                this.updateTraceAutoRefreshState();
            }
            this.updateTraceChartDragOffset();
        },
        destroyTraceChart() {
            this.traceChart?.destroy();
            this.traceChart = null;
        },
        resizeTraceChartAfterLayout() {
            window.requestAnimationFrame(() => {
                window.requestAnimationFrame(() => this.traceChart?.resize());
            });
        },
        traceText(key: string, values?: Record<string, string | number>) {
            return String(this.$t(`powerlimitertrace.${key}`, values || {}));
        },
        setTraceSelectedWindowSeconds(value: number | string) {
            const windowSeconds = normalizeTraceWindowSeconds(value);
            if (windowSeconds === this.traceSelectedWindowSeconds) {
                return;
            }

            this.traceSelectedWindowSeconds = windowSeconds;
            try {
                localStorage.setItem(traceWindowSecondsStorageKey, String(windowSeconds));
            } catch {
                // Ignore storage failures; the active selection still applies for this session.
            }

            this.traceOffsetSeconds = this.clampTraceOffset(this.traceOffsetSeconds);
            this.renderTraceChart();
            this.ensureTraceCacheForCurrentViewport(0);
            this.updateTraceAutoRefreshState();
        },
        formatTraceWindowOption(seconds: number) {
            return this.traceText('WindowDurationMinutes', { minutes: Math.round(seconds / 60) });
        },
        traceStatusLabel(status: number) {
            const keys = [
                'StatusInitializing',
                'StatusDisabledByConfig',
                'StatusDisabledByMqtt',
                'StatusWaitingForValidTimestamp',
                'StatusPowerMeterPending',
                'StatusInverterInvalid',
                'StatusInverterCmdPending',
                'StatusConfigReload',
                'StatusInverterStatsPending',
                'StatusUnconditionalSolarPassthrough',
                'StatusEmergencyFullOutput',
                'StatusStable',
            ];
            return this.traceText(keys[status] || 'StatusUnknown', { status });
        },
        normalizeTraceResponse(data: PowerLimiterTraceWireResponse): PowerLimiterTraceResponse {
            if (data.format !== 'compact-v1' && data.format !== 'compact-v2') {
                return data as PowerLimiterTraceResponse;
            }

            return {
                ...data,
                samples: data.samples.map((sample) => this.normalizeTraceSample(sample, data.now_ms, data.format)),
            };
        },
        normalizeTraceSample(
            sample: PowerLimiterTraceSample | PowerLimiterTraceCompactSample,
            nowMs: number,
            format?: string
        ): PowerLimiterTraceSample {
            if (!Array.isArray(sample)) {
                return sample;
            }

            const value = (index: number) => Number(sample[index] ?? 0);
            const optionalValue = (index: number) =>
                typeof sample[index] === 'number' ? Number(sample[index]) : undefined;
            const provider = (index: number): PowerLimiterTraceProvider => ({
                out: value(index),
                exp: value(index + 1),
                lim: value(index + 2),
                elim: value(index + 3),
                pending: value(index + 4),
                assumed: value(index + 5),
            });
            const forecast = (index: number): PowerLimiterTraceForecast | undefined => {
                const min = optionalValue(index);
                const nominal = optionalValue(index + 1);
                const max = optionalValue(index + 2);
                if (min === undefined || nominal === undefined || max === undefined) {
                    return undefined;
                }

                return {
                    valid: true,
                    min,
                    nominal,
                    max,
                };
            };
            const millis = format === 'compact-v2' ? (nowMs - value(0)) >>> 0 : value(0);
            const globalForecast = forecast(28);
            const storageTargetForecast = forecast(31) || { valid: false };
            const storageTargetOracle = optionalValue(34);
            const storageTargetHorizon = optionalValue(35);
            const storageTargetDueForecast = forecast(36);
            const storageTargetDueAge = optionalValue(39);
            if (storageTargetOracle !== undefined) {
                storageTargetForecast.oracle = storageTargetOracle;
            }
            if (storageTargetHorizon !== undefined) {
                storageTargetForecast.horizon_ms = storageTargetHorizon;
            }
            if (storageTargetDueForecast) {
                storageTargetForecast.due_min = storageTargetDueForecast.min;
                storageTargetForecast.due_nominal = storageTargetDueForecast.nominal;
                storageTargetForecast.due_max = storageTargetDueForecast.max;
            }
            if (storageTargetDueAge !== undefined) {
                storageTargetForecast.due_age_ms = storageTargetDueAge;
            }
            const predictive: PowerLimiterTracePredictive | undefined =
                globalForecast ||
                storageTargetOracle !== undefined ||
                storageTargetDueForecast ||
                storageTargetForecast.valid
                    ? {
                          ...(globalForecast ? { global: globalForecast } : {}),
                          ...(storageTargetOracle !== undefined ||
                          storageTargetDueForecast ||
                          storageTargetForecast.valid
                              ? { storage_target: storageTargetForecast }
                              : {}),
                      }
                    : undefined;

            return {
                ms: millis,
                pm: optionalValue(1) ?? null,
                target: value(2),
                storage: value(3),
                expected: optionalValue(4),
                status: value(5),
                charger: {
                    in: value(6),
                    target: optionalValue(7),
                    exp: optionalValue(7),
                    max: value(8),
                },
                battery: provider(9),
                solar: provider(15),
                smart: provider(21),
                battery_bms: optionalValue(27),
                predictive,
            };
        },
        readTraceDatasetVisibility() {
            try {
                const stored = localStorage.getItem(traceDatasetVisibilityStorageKey);
                return stored ? (JSON.parse(stored) as Record<string, boolean>) : {};
            } catch {
                return {};
            }
        },
        isTraceDatasetHidden(traceKey: string, defaultHidden = false) {
            const visibility = this.readTraceDatasetVisibility();
            return typeof visibility[traceKey] === 'boolean' ? visibility[traceKey] : defaultHidden;
        },
        setTraceDatasetHidden(traceKey: string, hidden: boolean) {
            try {
                const visibility = this.readTraceDatasetVisibility();
                visibility[traceKey] = hidden;
                localStorage.setItem(traceDatasetVisibilityStorageKey, JSON.stringify(visibility));
            } catch {
                // Ignore storage failures; legend state still works for the current chart instance.
            }
        },
        formatWatt(value: number | null | undefined) {
            if (value === null || value === undefined || Number.isNaN(value)) {
                return '-';
            }
            return `${Math.round(value)} W`;
        },
        formatTraceWindowLabel() {
            const from = this.traceTimestampForAgeSeconds(this.traceOffsetSeconds + this.traceWindowSeconds);
            const to = this.traceTimestampForAgeSeconds(this.traceOffsetSeconds);
            return `${this.formatTraceTooltipTime(from)} - ${this.formatTraceTooltipTime(to)}`;
        },
        traceSampleAgeSeconds(sample: PowerLimiterTraceSample, nowMs?: number) {
            const referenceNowMs = nowMs ?? this.traceData?.now_ms ?? 0;
            return Math.max(0, Math.round(((referenceNowMs - sample.ms) >>> 0) / 1000));
        },
        traceReferenceEpochSeconds() {
            const timestamp = this.traceData?.now_ts;
            if (typeof timestamp === 'number' && timestamp > 0) {
                return timestamp;
            }
            return this.traceLastResponseClientEpochSeconds ?? Date.now() / 1000;
        },
        traceTimestampForAgeSeconds(ageSeconds: number) {
            return this.traceReferenceEpochSeconds() - ageSeconds;
        },
        traceSampleTimestampSeconds(sample: PowerLimiterTraceSample, nowMs?: number) {
            const referenceNowMs = nowMs ?? this.traceData?.now_ms ?? sample.ms;
            const ageMillis = (referenceNowMs - sample.ms) >>> 0;
            return this.traceReferenceEpochSeconds() - ageMillis / 1000;
        },
        traceProviderHasFlag(sample: PowerLimiterTraceSample, flag: 'pending' | 'assumed') {
            return sample.battery[flag] || sample.solar[flag] || sample.smart[flag];
        },
        traceSamplesForAgeRange(fromAgeSeconds: number, toAgeSeconds: number): PowerLimiterTraceSample[] {
            const nowMs = this.traceData?.now_ms;
            if (nowMs === undefined) {
                return [];
            }

            return this.traceCacheSamples
                .filter((sample) => {
                    const ageSeconds = this.traceSampleAgeSeconds(sample, nowMs);
                    return ageSeconds >= fromAgeSeconds && ageSeconds < toAgeSeconds;
                })
                .sort(
                    (left, right) => this.traceSampleAgeSeconds(right, nowMs) - this.traceSampleAgeSeconds(left, nowMs)
                );
        },
        traceCachedSamplesSorted(): PowerLimiterTraceSample[] {
            const nowMs = this.traceData?.now_ms;
            if (nowMs === undefined) {
                return [];
            }

            return [...this.traceCacheSamples].sort(
                (left, right) => this.traceSampleAgeSeconds(right, nowMs) - this.traceSampleAgeSeconds(left, nowMs)
            );
        },
        traceRenderSamples(): PowerLimiterTraceSample[] {
            const range = this.traceCacheTargetRange();
            return this.traceSamplesForAgeRange(range.from, range.to);
        },
        tracePoint(sample: PowerLimiterTraceSample, y: number | null, xOffsetSeconds = 0): TraceChartPoint {
            return {
                x: this.traceSampleTimestampSeconds(sample) + xOffsetSeconds,
                y,
            };
        },
        traceGlobalForecastValue(sample: PowerLimiterTraceSample, field: 'min' | 'nominal' | 'max') {
            const forecast = sample.predictive?.global;
            if (!forecast?.valid) {
                return null;
            }

            const value = forecast[field];
            return typeof value === 'number' ? value : null;
        },
        traceStorageTargetForecastValue(sample: PowerLimiterTraceSample, field: 'min' | 'nominal' | 'max') {
            const forecast = sample.predictive?.storage_target;
            if (!forecast?.valid) {
                return null;
            }

            const value = forecast[field];
            return typeof value === 'number' ? value : null;
        },
        traceStorageTargetForecastOffsetSeconds(sample: PowerLimiterTraceSample) {
            const horizonMillis = sample.predictive?.storage_target?.horizon_ms;
            return typeof horizonMillis === 'number' ? horizonMillis / 1000 : 0;
        },
        traceStorageTargetDueForecastValue(
            sample: PowerLimiterTraceSample,
            field: 'due_min' | 'due_nominal' | 'due_max'
        ) {
            const value = sample.predictive?.storage_target?.[field];
            return typeof value === 'number' ? value : null;
        },
        traceStorageTargetOracleValue(sample: PowerLimiterTraceSample) {
            const value = sample.predictive?.storage_target?.oracle;
            return typeof value === 'number' ? value : null;
        },
        createTraceConfidenceBand(samples: PowerLimiterTraceSample[]): PowerLimiterTraceConfidenceBandPoint[] {
            return samples.map((sample) => ({
                x: this.traceSampleTimestampSeconds(sample),
                min: this.traceGlobalForecastValue(sample, 'min'),
                max: this.traceGlobalForecastValue(sample, 'max'),
            }));
        },
        createTraceDatasets(samples: PowerLimiterTraceSample[]): PowerLimiterTraceDataset[] {
            const line = (
                traceKey: string,
                label: string,
                data: (number | null)[],
                color: string,
                borderDash: number[] = [],
                hidden = false,
                borderWidth = 1.6,
                xOffsetSeconds: (sample: PowerLimiterTraceSample) => number = () => 0
            ): PowerLimiterTraceDataset => ({
                traceKey,
                label,
                data: samples.map((sample, index) =>
                    this.tracePoint(sample, data[index] ?? null, xOffsetSeconds(sample))
                ),
                borderColor: color,
                backgroundColor: color,
                borderWidth,
                borderDash,
                fill: false,
                hidden: this.isTraceDatasetHidden(traceKey, hidden),
                pointRadius: 0,
                pointHoverRadius: 4,
                spanGaps: true,
                tension: 0.12,
            });

            const expectedGrid = samples.map((sample) =>
                typeof sample.expected === 'number' ? sample.expected : null
            );

            return [
                line(
                    'grid_power',
                    this.traceText('GridPower'),
                    samples.map((sample) => sample.pm),
                    '#dc3545',
                    [],
                    false,
                    2.2
                ),
                line(
                    'target',
                    this.traceText('Target'),
                    samples.map((sample) => sample.target),
                    '#6c757d',
                    [7, 4]
                ),
                line(
                    'storage_target',
                    this.traceText('StorageTarget'),
                    samples.map((sample) => sample.storage),
                    '#6f42c1',
                    [2, 4],
                    false
                ),
                line(
                    'storage_target_forecast_min',
                    this.traceText('StorageTargetForecastMin'),
                    samples.map((sample) => this.traceStorageTargetForecastValue(sample, 'min')),
                    '#b197fc',
                    [1, 5],
                    true,
                    1.6,
                    this.traceStorageTargetForecastOffsetSeconds
                ),
                line(
                    'storage_target_forecast',
                    this.traceText('StorageTargetForecast'),
                    samples.map((sample) => this.traceStorageTargetForecastValue(sample, 'nominal')),
                    '#8f63d9',
                    [6, 3],
                    true,
                    1.6,
                    this.traceStorageTargetForecastOffsetSeconds
                ),
                line(
                    'storage_target_forecast_max',
                    this.traceText('StorageTargetForecastMax'),
                    samples.map((sample) => this.traceStorageTargetForecastValue(sample, 'max')),
                    '#7048e8',
                    [1, 5],
                    true,
                    1.6,
                    this.traceStorageTargetForecastOffsetSeconds
                ),
                line(
                    'storage_target_due_forecast_min',
                    this.traceText('StorageTargetDueForecastMin'),
                    samples.map((sample) => this.traceStorageTargetDueForecastValue(sample, 'due_min')),
                    '#74c0fc',
                    [3, 4],
                    true
                ),
                line(
                    'storage_target_due_forecast',
                    this.traceText('StorageTargetDueForecast'),
                    samples.map((sample) => this.traceStorageTargetDueForecastValue(sample, 'due_nominal')),
                    '#0d6efd',
                    [8, 3],
                    true,
                    2
                ),
                line(
                    'storage_target_due_forecast_max',
                    this.traceText('StorageTargetDueForecastMax'),
                    samples.map((sample) => this.traceStorageTargetDueForecastValue(sample, 'due_max')),
                    '#1864ab',
                    [3, 4],
                    true
                ),
                line(
                    'storage_target_oracle',
                    this.traceText('StorageTargetOracle'),
                    samples.map((sample) => this.traceStorageTargetOracleValue(sample)),
                    '#e83e8c',
                    [4, 2],
                    true,
                    1.8
                ),
                line('expected_grid', this.traceText('ExpectedGrid'), expectedGrid, '#198754', [7, 4], true),
                line(
                    'battery_bms_power',
                    this.traceText('BatteryBmsPower'),
                    samples.map((sample) => (typeof sample.battery_bms === 'number' ? sample.battery_bms : null)),
                    '#795548',
                    [2, 3]
                ),
                line(
                    'battery_output',
                    this.traceText('BatteryOutput'),
                    samples.map((sample) => sample.battery.out),
                    '#fd7e14'
                ),
                line(
                    'battery_target',
                    this.traceText('BatteryTarget'),
                    samples.map((sample) => sample.battery.elim),
                    '#fd7e14',
                    [5, 4]
                ),
                line(
                    'solar_output',
                    this.traceText('SolarOutput'),
                    samples.map((sample) => sample.solar.out),
                    '#ffc107'
                ),
                line(
                    'solar_limit',
                    this.traceText('SolarLimit'),
                    samples.map((sample) => sample.solar.elim),
                    '#b58100',
                    [5, 4]
                ),
                line(
                    'smart_output',
                    this.traceText('SmartOutput'),
                    samples.map((sample) => sample.smart.out),
                    '#20c997'
                ),
                line(
                    'smart_limit',
                    this.traceText('SmartLimit'),
                    samples.map((sample) => sample.smart.elim),
                    '#20c997',
                    [5, 4]
                ),
                line(
                    'charger_input',
                    this.traceText('ChargerInput'),
                    samples.map((sample) => sample.charger.in),
                    '#0dcaf0'
                ),
                line(
                    'charger_target',
                    this.traceText('ChargerTarget'),
                    samples.map((sample) => sample.charger.target ?? sample.charger.exp ?? null),
                    '#0dcaf0',
                    [5, 4],
                    false,
                    2.1
                ),
                line(
                    'charger_maximum',
                    this.traceText('ChargerMaximum'),
                    samples.map((sample) => sample.charger.max),
                    '#0d6efd',
                    [2, 4],
                    true
                ),
            ];
        },
        traceViewportMinX() {
            return this.traceTimestampForAgeSeconds(this.traceOffsetSeconds + this.traceWindowSeconds);
        },
        traceViewportMaxX() {
            return this.traceTimestampForAgeSeconds(this.traceOffsetSeconds);
        },
        formatTraceAxisTime(timestampSeconds: number) {
            return new Intl.DateTimeFormat(undefined, {
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
            }).format(new Date(timestampSeconds * 1000));
        },
        formatTraceTooltipTime(timestampSeconds: number) {
            return new Intl.DateTimeFormat(undefined, {
                day: '2-digit',
                month: '2-digit',
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
            }).format(new Date(timestampSeconds * 1000));
        },
        renderTraceChart() {
            const canvas = this.$refs.traceChart as HTMLCanvasElement | undefined;
            const traceData = this.traceData;
            if (!canvas || !traceData || this.traceCacheSampleCount === 0) {
                this.destroyTraceChart();
                return;
            }

            const samples = this.traceRenderSamples();
            this.traceTooltipSamples = samples;
            const datasets = this.createTraceDatasets(samples);
            const confidenceBand = this.createTraceConfidenceBand(samples);
            const config: ChartConfiguration<'line', TraceChartPoint[], number> = {
                type: 'line',
                data: {
                    datasets,
                },
                options: {
                    animation: false,
                    maintainAspectRatio: false,
                    parsing: false,
                    responsive: true,
                    interaction: {
                        intersect: false,
                        mode: 'index',
                    },
                    plugins: {
                        powerLimiterTraceDrag: {
                            confidenceBand,
                        },
                        legend: {
                            display: true,
                            position: 'top',
                            labels: {
                                boxWidth: 12,
                                usePointStyle: true,
                            },
                            onClick: (_event, legendItem, legend) => {
                                const datasetIndex = legendItem.datasetIndex;
                                if (typeof datasetIndex !== 'number') {
                                    return;
                                }

                                const chart = legend.chart;
                                chart.setDatasetVisibility(datasetIndex, !chart.isDatasetVisible(datasetIndex));
                                chart.update();

                                const dataset = chart.data.datasets[datasetIndex] as
                                    | PowerLimiterTraceDataset
                                    | undefined;
                                if (dataset?.traceKey) {
                                    this.setTraceDatasetHidden(dataset.traceKey, !chart.isDatasetVisible(datasetIndex));
                                }
                            },
                        },
                        tooltip: {
                            callbacks: {
                                title: (items) => {
                                    return this.formatTraceTooltipTime(Number(items[0]?.parsed.x ?? 0));
                                },
                                label: (context: TooltipItem<'line'>) => {
                                    return `${context.dataset.label}: ${this.formatWatt(context.parsed.y)}`;
                                },
                                afterBody: (items) => {
                                    const index = items[0]?.dataIndex ?? 0;
                                    const source = this.traceTooltipSamples[index];
                                    if (!source) {
                                        return [];
                                    }

                                    const lines = [
                                        this.traceText('StatusLine', {
                                            status: this.traceStatusLabel(source.status),
                                        }),
                                    ];
                                    if (this.traceProviderHasFlag(source, 'pending')) {
                                        lines.push(this.traceText('PendingCommand'));
                                    }
                                    if (this.traceProviderHasFlag(source, 'assumed')) {
                                        lines.push(this.traceText('AssumedOutput'));
                                    }
                                    return lines;
                                },
                            },
                        },
                    } as PowerLimiterTraceChartPluginOptions,
                    scales: {
                        x: {
                            type: 'linear',
                            min: this.traceViewportMinX(),
                            max: this.traceViewportMaxX(),
                            grid: {
                                display: false,
                            },
                            ticks: {
                                autoSkip: true,
                                maxRotation: 0,
                                maxTicksLimit: 10,
                                callback: (value) => this.formatTraceAxisTime(Number(value)),
                            },
                        },
                        y: {
                            title: {
                                display: true,
                                text: 'W',
                            },
                            ticks: {
                                callback: (value) => this.formatWatt(Number(value)),
                            },
                        },
                    },
                },
            };

            if (this.traceChart) {
                this.destroyTraceChart();
            }

            this.traceChart = markRaw(new ChartJS(canvas, config));
        },
    },
});
</script>

<style scoped>
.power-limiter-trace-panel {
    block-size: 100%;
    display: grid;
    grid-template-rows: auto minmax(0, 1fr);
    min-block-size: 0;
}

.power-limiter-trace-chart {
    block-size: 100%;
    cursor: grab;
    inline-size: 100%;
    min-block-size: 360px;
    position: relative;
    touch-action: pan-y;
    user-select: none;
}

.power-limiter-trace-chart-dragging {
    cursor: grabbing;
}

.power-limiter-trace-spin {
    animation: power-limiter-trace-spin 0.9s linear infinite;
}

.power-limiter-trace-window-selector {
    white-space: nowrap;
}

@keyframes power-limiter-trace-spin {
    from {
        transform: rotate(0deg);
    }

    to {
        transform: rotate(360deg);
    }
}
</style>
