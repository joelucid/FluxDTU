<template>
    <BasePage :title="$t('operationprofiles.Title')" :isLoading="dataLoading" :show-reload="true" @reload="getStatus">
        <BootstrapAlert
            v-model="alert.show"
            dismissible
            :variant="alert.type"
            :auto-dismiss="alert.type != 'success' ? 0 : 5000"
        >
            {{ alert.message }}
        </BootstrapAlert>

        <div class="row g-4">
            <div class="col-lg-5">
                <CardElement :text="$t('operationprofiles.ActiveProfile')" textVariant="text-bg-primary">
                    <div class="d-flex align-items-center justify-content-between gap-3 flex-wrap">
                        <div>
                            <div class="h4 mb-1">{{ status.active_profile_name }}</div>
                            <span v-if="status.modified" class="badge text-bg-warning">
                                {{ $t('operationprofiles.Modified') }}
                            </span>
                            <span v-else class="badge text-bg-success">
                                {{ $t('operationprofiles.Saved') }}
                            </span>
                        </div>
                    </div>

                    <form class="row g-2 mt-4" @submit.prevent="createProfile">
                        <div class="col">
                            <input
                                class="form-control"
                                type="text"
                                v-model="newProfileName"
                                :maxlength="maxNameLength"
                                :placeholder="$t('operationprofiles.NewProfileName')"
                                required
                            />
                        </div>
                        <div class="col-auto">
                            <button
                                type="submit"
                                class="btn btn-success"
                                :disabled="busy || newProfileName.trim() === ''"
                            >
                                <BIconPlusCircle /> {{ $t('operationprofiles.CreateFromCurrent') }}
                            </button>
                        </div>
                    </form>
                </CardElement>
            </div>

            <div class="col-lg-7">
                <CardElement :text="$t('operationprofiles.Profiles')" textVariant="text-bg-primary">
                    <div class="table-responsive">
                        <table class="table align-middle">
                            <thead>
                                <tr>
                                    <th scope="col">{{ $t('operationprofiles.Name') }}</th>
                                    <th scope="col">{{ $t('operationprofiles.State') }}</th>
                                    <th scope="col">{{ $t('operationprofiles.Action') }}</th>
                                </tr>
                            </thead>
                            <tbody>
                                <tr v-for="profile in status.profiles" :key="profile.id">
                                    <td>{{ profile.name }}</td>
                                    <td>
                                        <span v-if="profile.active" class="badge text-bg-primary">
                                            {{ $t('operationprofiles.Active') }}
                                        </span>
                                    </td>
                                    <td>
                                        <div class="btn-group btn-group-sm" role="group">
                                            <button
                                                type="button"
                                                class="btn btn-outline-primary"
                                                :disabled="busy"
                                                :title="$t('operationprofiles.Apply')"
                                                @click="applyProfile(profile.id)"
                                            >
                                                <BIconCheck2 />
                                            </button>
                                            <button
                                                type="button"
                                                class="btn btn-outline-secondary"
                                                :disabled="busy"
                                                :title="$t('operationprofiles.Rename')"
                                                @click="openRenameModal(profile)"
                                            >
                                                <BIconPencil />
                                            </button>
                                            <button
                                                type="button"
                                                class="btn btn-outline-danger"
                                                :disabled="busy || status.profiles.length <= 1"
                                                :title="$t('operationprofiles.Delete')"
                                                @click="openDeleteModal(profile)"
                                            >
                                                <BIconTrash />
                                            </button>
                                        </div>
                                    </td>
                                </tr>
                            </tbody>
                        </table>
                    </div>
                </CardElement>
            </div>
        </div>
    </BasePage>

    <ModalDialog
        modalId="operationProfileRename"
        small
        :title="$t('operationprofiles.Rename')"
        :closeText="$t('base.Cancel')"
    >
        <input class="form-control" type="text" v-model="selectedProfile.name" :maxlength="maxNameLength" required />
        <template #footer>
            <button
                type="button"
                class="btn btn-primary"
                :disabled="busy || selectedProfile.name.trim() === ''"
                @click="renameProfile"
            >
                <BIconPencil /> {{ $t('operationprofiles.Rename') }}
            </button>
        </template>
    </ModalDialog>

    <ModalDialog
        modalId="operationProfileDelete"
        small
        :title="$t('operationprofiles.Delete')"
        :closeText="$t('base.Cancel')"
    >
        {{
            $t('operationprofiles.DeleteQuestion', {
                name: selectedProfile.name,
            })
        }}
        <template #footer>
            <button type="button" class="btn btn-danger" :disabled="busy" @click="deleteProfile">
                <BIconTrash /> {{ $t('operationprofiles.Delete') }}
            </button>
        </template>
    </ModalDialog>
</template>

<script lang="ts">
import BasePage from '@/components/BasePage.vue';
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import CardElement from '@/components/CardElement.vue';
import ModalDialog from '@/components/ModalDialog.vue';
import type { AlertResponse } from '@/types/AlertResponse';
import type { OperationProfile, OperationProfilesStatus } from '@/types/OperationProfiles';
import { authHeader, handleResponse } from '@/utils/authentication';
import * as bootstrap from 'bootstrap';
import { BIconCheck2, BIconPencil, BIconPlusCircle, BIconTrash } from 'bootstrap-icons-vue';
import { defineComponent } from 'vue';

export default defineComponent({
    components: {
        BasePage,
        BootstrapAlert,
        CardElement,
        ModalDialog,
        BIconCheck2,
        BIconPencil,
        BIconPlusCircle,
        BIconTrash,
    },
    data() {
        return {
            dataLoading: true,
            busy: false,
            maxNameLength: 31,
            status: {
                version: 1,
                active_profile_id: '',
                active_profile_name: '',
                modified: false,
                profiles: [],
            } as OperationProfilesStatus,
            selectedProfile: { id: '', name: '', active: false } as OperationProfile,
            newProfileName: '',
            alert: { message: '', type: 'info', code: 0, show: false } as AlertResponse,
            modalRename: {} as bootstrap.Modal,
            modalDelete: {} as bootstrap.Modal,
        };
    },
    mounted() {
        this.modalRename = new bootstrap.Modal('#operationProfileRename');
        this.modalDelete = new bootstrap.Modal('#operationProfileDelete');
    },
    created() {
        this.getStatus();
    },
    methods: {
        getStatus() {
            this.dataLoading = true;
            fetch('/api/operationprofiles/status', { headers: authHeader() })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.status = data;
                    this.dataLoading = false;
                });
        },
        callEndpoint(endpoint: string, payload: object, successMessageKey: string) {
            this.busy = true;
            const formData = new FormData();
            formData.append('data', JSON.stringify(payload));

            fetch('/api/operationprofiles/' + endpoint, {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((data) => {
                    this.alert.type = data.type;
                    this.alert.message = data.type === 'success' ? this.$t(successMessageKey) : data.message;
                    this.alert.show = true;
                    this.$emitter.emit('operation-profiles-changed');
                    this.getStatus();
                })
                .finally(() => {
                    this.busy = false;
                });
        },
        createProfile() {
            const name = this.newProfileName.trim();
            if (name === '') {
                return;
            }
            this.callEndpoint('create', { name }, 'operationprofiles.Created');
            this.newProfileName = '';
        },
        applyProfile(id: string) {
            this.callEndpoint('apply', { id }, 'operationprofiles.Applied');
        },
        openRenameModal(profile: OperationProfile) {
            this.selectedProfile = { ...profile };
            this.modalRename.show();
        },
        renameProfile() {
            this.callEndpoint(
                'rename',
                { id: this.selectedProfile.id, name: this.selectedProfile.name.trim() },
                'operationprofiles.Renamed'
            );
            this.modalRename.hide();
        },
        openDeleteModal(profile: OperationProfile) {
            this.selectedProfile = { ...profile };
            this.modalDelete.show();
        },
        deleteProfile() {
            this.callEndpoint('delete', { id: this.selectedProfile.id }, 'operationprofiles.Deleted');
            this.modalDelete.hide();
        },
    },
});
</script>
