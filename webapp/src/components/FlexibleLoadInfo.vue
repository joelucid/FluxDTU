<template>
    <div v-if="loads.length > 0" class="row gy-3 mt-0">
        <div class="tab-content col-sm-12 col-md-12">
            <div class="card">
                <div class="card-header d-flex justify-content-between align-items-center">
                    <div class="p-1 flex-grow-1">
                        <div class="d-flex flex-wrap">
                            <div style="padding-right: 2em">{{ $t('home.FlexibleLoads') }}</div>
                        </div>
                    </div>
                </div>

                <div class="card-body">
                    <div class="row row-cols-1 row-cols-md-3 g-3">
                        <div class="col" v-for="load in loads" :key="load.index">
                            <CardElement
                                centerContent
                                textVariant="text-bg-primary"
                                :text="load.name || $t('invertertotalinfo.FlexibleLoad')"
                            >
                                <div class="text-start">
                                    <div>
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadEnableStatus') }}:</strong>
                                        {{ $t(flexibleLoadEnabledLabelKey(load.enabled)) }}
                                    </div>
                                    <div>
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadPriority') }}:</strong>
                                        {{ load.priority ?? load.index + 1 }}
                                    </div>
                                    <div>
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadState') }}:</strong>
                                        {{ formatFlexibleLoadText(load.state) }}
                                    </div>
                                    <div v-if="load.power">
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadPower') }}:</strong>
                                        {{
                                            $n(load.power.v, 'decimal', {
                                                minimumFractionDigits: load.power.d,
                                                maximumFractionDigits: load.power.d,
                                            })
                                        }}
                                        {{ load.power.u }}
                                    </div>
                                    <div>
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadStartBlockReason') }}:</strong>
                                        {{ formatFlexibleLoadText(load.startBlockReason) }}
                                    </div>
                                    <div>
                                        <strong>{{ $t('invertertotalinfo.FlexibleLoadStartReason') }}:</strong>
                                        {{ formatFlexibleLoadText(load.startReason) }}
                                    </div>
                                </div>
                                <div class="d-grid mt-3">
                                    <button
                                        type="button"
                                        class="btn"
                                        :class="load.enabled ? 'btn-success' : 'btn-secondary'"
                                        :disabled="isFlexibleLoadSettingLoading(load.index)"
                                        :aria-pressed="load.enabled"
                                        :aria-label="$t(flexibleLoadToggleActionKey(load.enabled))"
                                        :title="$t(flexibleLoadToggleActionKey(load.enabled))"
                                        @click="onSetFlexibleLoadEnabled(load.index, !load.enabled)"
                                    >
                                        <span
                                            v-if="isFlexibleLoadSettingLoading(load.index)"
                                            class="spinner-border spinner-border-sm me-2"
                                            aria-hidden="true"
                                        ></span>
                                        <BIconToggleOn v-else-if="load.enabled" class="fs-4 me-2" aria-hidden="true" />
                                        <BIconToggleOff v-else class="fs-4 me-2" aria-hidden="true" />
                                        {{ $t(flexibleLoadEnabledLabelKey(load.enabled)) }}
                                    </button>
                                </div>
                            </CardElement>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>
</template>

<script lang="ts">
import CardElement from '@/components/CardElement.vue';
import type { FlexibleLoad } from '@/types/LiveDataStatus';
import { authHeader, handleResponse } from '@/utils/authentication';
import { BIconToggleOff, BIconToggleOn } from 'bootstrap-icons-vue';
import { defineComponent, type PropType } from 'vue';

export default defineComponent({
    components: {
        BIconToggleOff,
        BIconToggleOn,
        CardElement,
    },
    emits: {
        'enabled-change': (index: number, enabled: boolean) =>
            typeof index === 'number' && typeof enabled === 'boolean',
    },
    props: {
        loads: { type: Array as PropType<FlexibleLoad[]>, required: true },
    },
    data() {
        return {
            flexibleLoadSettingLoadingIndex: -1,
        };
    },
    methods: {
        flexibleLoadEnabledLabelKey(enabled: boolean): string {
            return enabled ? 'invertertotalinfo.FlexibleLoadEnabled' : 'invertertotalinfo.FlexibleLoadDisabled';
        },
        flexibleLoadToggleActionKey(enabled: boolean): string {
            return enabled
                ? 'invertertotalinfo.FlexibleLoadDisableAction'
                : 'invertertotalinfo.FlexibleLoadEnableAction';
        },
        formatFlexibleLoadText(value: string | undefined): string {
            return (value || 'none').replace(/_/g, ' ');
        },
        isFlexibleLoadSettingLoading(index: number): boolean {
            return this.flexibleLoadSettingLoadingIndex === index;
        },
        onSetFlexibleLoadEnabled(index: number, enabled: boolean) {
            if (this.flexibleLoadSettingLoadingIndex >= 0) {
                return;
            }

            this.flexibleLoadSettingLoadingIndex = index;

            const formData = new FormData();
            formData.append('data', JSON.stringify({ index, enabled }));

            fetch('/api/powerlimiter/flexible_load/enabled', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    if (response.type === 'success') {
                        this.$emit('enabled-change', index, enabled);
                    }
                })
                .catch(() => undefined)
                .finally(() => {
                    this.flexibleLoadSettingLoadingIndex = -1;
                });
        },
    },
});
</script>
