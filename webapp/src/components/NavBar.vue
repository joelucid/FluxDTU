<template>
    <nav class="navbar navbar-expand-md fixed-top bg-body-tertiary" data-bs-theme="dark">
        <div class="container-fluid">
            <router-link @click="onClick" class="navbar-brand" to="/" style="display: flex; height: 30px; padding: 0">
                <BIconTree v-if="isXmas" width="30" height="30" class="d-inline-block align-text-top text-success" />

                <BIconEgg v-else-if="isEaster" width="30" height="30" class="d-inline-block align-text-top text-info" />

                <BIconSun v-else width="30" height="30" class="d-inline-block align-text-top text-warning" />

                <span style="margin-left: 0.5rem"> FluxDTU </span>
                <span class="text-info mx-2"
                    ><BIconBatteryCharging width="20" height="20" class="d-inline-block align-text-center"
                /></span>
            </router-link>
            <button
                class="navbar-toggler"
                type="button"
                data-bs-toggle="collapse"
                data-bs-target="#navbarNavAltMarkup"
                aria-controls="navbarNavAltMarkup"
                aria-expanded="false"
                aria-label="Toggle navigation"
            >
                <span class="navbar-toggler-icon"></span>
            </button>
            <div class="collapse navbar-collapse" ref="navbarCollapse" id="navbarNavAltMarkup">
                <ul class="navbar-nav navbar-nav-scroll d-flex me-auto flex-sm-fill">
                    <li class="nav-item">
                        <router-link @click="onClick" class="nav-link" to="/">{{ $t('menu.LiveView') }}</router-link>
                    </li>
                    <li class="nav-item dropdown">
                        <a
                            class="nav-link dropdown-toggle"
                            :class="{ active: isStatisticsRoute }"
                            href="#"
                            id="statisticsDropdown"
                            role="button"
                            data-bs-toggle="dropdown"
                            aria-expanded="false"
                        >
                            {{ $t('menu.Statistics') }}
                        </a>
                        <ul class="dropdown-menu" aria-labelledby="statisticsDropdown">
                            <li v-for="entry in statisticsMenuEntries" :key="entry.path">
                                <router-link @click="onClick" class="dropdown-item" :to="entry.path">
                                    {{ $t(entry.label) }}
                                </router-link>
                            </li>
                        </ul>
                    </li>
                    <li class="nav-item dropdown">
                        <a
                            class="nav-link dropdown-toggle"
                            href="#"
                            id="navbarScrollingDropdown"
                            role="button"
                            data-bs-toggle="dropdown"
                            aria-expanded="false"
                        >
                            {{ $t('menu.Settings') }}
                        </a>
                        <ul class="dropdown-menu" aria-labelledby="navbarScrollingDropdown">
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/network">{{
                                    $t('menu.NetworkSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/ntp">{{
                                    $t('menu.NTPSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/mqtt">{{
                                    $t('menu.MQTTSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/inverter"
                                    >{{ $t('menu.InverterSettings') }}
                                </router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/security"
                                    >{{ $t('menu.SecuritySettings') }}
                                </router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/logging"
                                    >{{ $t('menu.LoggingSettings') }}
                                </router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/dtu">{{
                                    $t('menu.DTUSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/solarcharger">{{
                                    $t('menu.SolarChargerSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/powermeter">{{
                                    $t('menu.PowerMeterSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/powerlimiter"
                                    >Dynamic Power Limiter</router-link
                                >
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/operationprofiles">{{
                                    $t('menu.OperationProfiles')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/battery">{{
                                    $t('menu.BatterySettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/chargerac">{{
                                    $t('menu.AcChargerSettings')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/device">{{
                                    $t('menu.DeviceManager')
                                }}</router-link>
                            </li>
                            <li>
                                <hr class="dropdown-divider" />
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/config">{{
                                    $t('menu.ConfigManagement')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/firmware/upgrade">{{
                                    $t('menu.FirmwareUpgrade')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/maintenance/reboot">{{
                                    $t('menu.DeviceReboot')
                                }}</router-link>
                            </li>
                        </ul>
                    </li>
                    <li class="nav-item dropdown">
                        <a
                            class="nav-link dropdown-toggle"
                            href="#"
                            id="navbarScrollingDropdown"
                            role="button"
                            data-bs-toggle="dropdown"
                            aria-expanded="false"
                        >
                            {{ $t('menu.Info') }}
                        </a>
                        <ul class="dropdown-menu" aria-labelledby="navbarScrollingDropdown">
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/info/system">{{
                                    $t('menu.System')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/info/network">{{
                                    $t('menu.Network')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/info/ntp">{{
                                    $t('menu.NTP')
                                }}</router-link>
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/info/mqtt">{{
                                    $t('menu.MQTT')
                                }}</router-link>
                            </li>
                            <li>
                                <hr class="dropdown-divider" />
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/info/console">{{
                                    $t('menu.Console')
                                }}</router-link>
                            </li>
                        </ul>
                    </li>
                    <li class="nav-item">
                        <router-link @click="onClick" class="nav-link" to="/about">{{ $t('menu.About') }}</router-link>
                    </li>
                    <li class="flex-sm-fill"></li>
                    <ThemeSwitcher class="me-2" />
                    <li v-if="isLogged && operationProfilesStatus.profiles.length > 0" class="nav-item dropdown me-2">
                        <button
                            class="btn btn-link nav-link py-2 px-0 px-lg-2 dropdown-toggle"
                            id="operationProfileDropdown"
                            type="button"
                            data-bs-toggle="dropdown"
                            aria-expanded="false"
                        >
                            {{ operationProfilesStatus.active_profile_name }}
                        </button>
                        <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="operationProfileDropdown">
                            <li v-for="profile in operationProfilesStatus.profiles" :key="profile.id">
                                <button
                                    type="button"
                                    :class="['dropdown-item', profile.active ? 'active' : '']"
                                    @click="applyOperationProfile(profile.id)"
                                >
                                    {{ profile.name }}
                                </button>
                            </li>
                            <li>
                                <hr class="dropdown-divider" />
                            </li>
                            <li>
                                <router-link @click="onClick" class="dropdown-item" to="/settings/operationprofiles">
                                    {{ $t('menu.OperationProfiles') }}
                                </router-link>
                            </li>
                        </ul>
                    </li>
                    <form class="d-flex" role="search">
                        <LocaleSwitcher class="me-2" />
                        <button v-if="isLogged" class="btn btn-outline-danger" @click="signout">
                            {{ $t('menu.Logout') }}
                        </button>
                        <button v-if="!isLogged" class="btn btn-outline-success" @click="signin">
                            {{ $t('menu.Login') }}
                        </button>
                    </form>
                </ul>
            </div>
        </div>
    </nav>
</template>

<script lang="ts">
import type { OperationProfilesStatus } from '@/types/OperationProfiles';
import { authHeader, handleResponse, isLoggedIn, logout } from '@/utils/authentication';
import { Collapse, Dropdown } from 'bootstrap';
import { BIconEgg, BIconSun, BIconTree, BIconBatteryCharging } from 'bootstrap-icons-vue';
import { defineComponent } from 'vue';
import LocaleSwitcher from './LocaleSwitcher.vue';
import ThemeSwitcher from './ThemeSwitcher.vue';

export default defineComponent({
    components: {
        BIconEgg,
        BIconSun,
        BIconTree,
        BIconBatteryCharging,
        LocaleSwitcher,
        ThemeSwitcher,
    },
    data() {
        return {
            isLogged: isLoggedIn(),
            now: {} as Date,
            operationProfilesStatus: {
                version: 1,
                active_profile_id: '',
                active_profile_name: '',
                modified: false,
                profiles: [],
            } as OperationProfilesStatus,
            statisticsMenuEntries: [
                { path: '/statistics/flow', label: 'statistics.PowerHistory' },
                { path: '/statistics/loads', label: 'statistics.Loads' },
                { path: '/statistics/panels', label: 'statistics.Panels' },
                { path: '/statistics/battery', label: 'statistics.Battery' },
                { path: '/statistics/temperatures', label: 'statistics.Temperatures' },
                { path: '/statistics/boost', label: 'statistics.Boost' },
                { path: '/statistics/requests', label: 'statistics.Requests' },
                { path: '/statistics/powerlimiter', label: 'statistics.PowerLimiterTimeline' },
            ],
        };
    },
    created() {
        this.$emitter.on('logged-in', () => {
            this.isLogged = this.isLoggedIn();
            this.getOperationProfiles();
        });
        this.$emitter.on('logged-out', () => {
            this.isLogged = this.isLoggedIn();
            this.clearOperationProfiles();
        });
        this.$emitter.on('operation-profiles-changed', () => {
            this.getOperationProfiles();
        });

        this.now = new Date();
        setInterval(() => {
            this.now = new Date();
        }, 10000);

        if (this.isLogged) {
            this.getOperationProfiles();
        }
    },
    computed: {
        isXmas() {
            return this.now.getMonth() + 1 == 12 && this.now.getDate() >= 24 && this.now.getDate() <= 26;
        },
        isEaster() {
            const easter = this.getEasterSunday(this.now.getFullYear());
            const easterStart = new Date(easter);
            const easterEnd = new Date(easter);
            easterStart.setDate(easterStart.getDate() - 2);
            easterEnd.setDate(easterEnd.getDate() + 1);
            return this.now >= easterStart && this.now < easterEnd;
        },
        isStatisticsRoute(): boolean {
            return this.$route.path.startsWith('/statistics');
        },
    },
    methods: {
        isLoggedIn,
        logout,
        signin(e: Event) {
            e.preventDefault();
            this.$router.push('/login');
        },
        signout(e: Event) {
            e.preventDefault();
            this.logout();
            this.$emitter.emit('logged-out');
            this.$router.push('/');
        },
        onClick() {
            if (this.$refs.navbarCollapse) {
                Collapse.getOrCreateInstance(this.$refs.navbarCollapse as HTMLElement, { toggle: false }).hide();
            }

            document.querySelectorAll<HTMLElement>('.dropdown-toggle.show').forEach((toggle) => {
                Dropdown.getOrCreateInstance(toggle).hide();
            });
        },
        clearOperationProfiles() {
            this.operationProfilesStatus = {
                version: 1,
                active_profile_id: '',
                active_profile_name: '',
                modified: false,
                profiles: [],
            };
        },
        getOperationProfiles() {
            if (!this.isLogged) {
                this.clearOperationProfiles();
                return;
            }

            fetch('/api/operationprofiles/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.operationProfilesStatus = data;
                })
                .catch(() => {
                    this.clearOperationProfiles();
                });
        },
        applyOperationProfile(id: string) {
            const formData = new FormData();
            formData.append('data', JSON.stringify({ id }));

            fetch('/api/operationprofiles/apply', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then(() => {
                    this.getOperationProfiles();
                    this.$emitter.emit('operation-profiles-changed');
                    this.onClick();
                });
        },
        getEasterSunday(year: number): Date {
            const f = Math.floor;
            const G = year % 19;
            const C = f(year / 100);
            const H = (C - f(C / 4) - f((8 * C + 13) / 25) + 19 * G + 15) % 30;
            const I = H - f(H / 28) * (1 - f(29 / (H + 1)) * f((21 - G) / 11));
            const J = (year + f(year / 4) + I + 2 - C + f(C / 4)) % 7;
            const L = I - J;
            const month = 3 + f((L + 40) / 44);
            const day = L + 28 - 31 * f(month / 4);

            return new Date(year, month - 1, day);
        },
    },
});
</script>
