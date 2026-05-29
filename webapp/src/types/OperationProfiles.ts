export interface OperationProfile {
    id: string;
    name: string;
    active: boolean;
}

export interface OperationProfilesStatus {
    version: number;
    active_profile_id: string;
    active_profile_name: string;
    modified: boolean;
    profiles: OperationProfile[];
}
