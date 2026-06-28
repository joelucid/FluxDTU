// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gridcharger/PowerLimiterTargetTiming.h>
#include <gridcharger/Stats.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace GridChargers {

struct PowerLimiterTargetDispatchEvent {
    uint16_t targetInputPowerWatts = 0;
    uint32_t sentMillis = 0;
    uint32_t targetEffectAssumptionMillis = PowerLimiterNormalTargetEffectAssumptionMillis;
};

struct PowerLimiterControlProposal {
    uint16_t targetInputPowerWatts = 0;
    bool limitedByAvailablePower = false;
};

class Provider {
public:
    static constexpr uint32_t PowerLimiterTargetEffectAssumptionMillis =
        PowerLimiterNormalTargetEffectAssumptionMillis;
    static constexpr uint32_t PowerLimiterStartupTargetEffectAssumptionMillis =
        GridChargers::PowerLimiterStartupTargetEffectAssumptionMillis;

    virtual ~Provider() = default;
    virtual bool init() = 0;
    virtual void deinit() = 0;
    virtual void loop() = 0;

    virtual std::shared_ptr<Stats> getStats() const = 0;
    virtual bool getAutoPowerStatus() const = 0;
    virtual int16_t getAutoPowerTargetPowerConsumption() const = 0;
    virtual bool isAutoPowerLimitedByAvailablePower() const = 0;

    virtual bool supportsPowerLimiterControl() const { return false; }
    virtual std::optional<uint32_t> getPowerLimiterOutputReferenceMillis() const { return 0; }
    virtual uint16_t getPowerLimiterCurrentInputPowerWatts() const { return 0; }
    virtual std::optional<uint16_t> getPowerLimiterTargetInputPowerWatts() const { return std::nullopt; }
    virtual uint16_t getPowerLimiterExpectedInputPowerWatts() const { return 0; }
    virtual uint16_t getPowerLimiterMaxInputPowerWatts() const { return 0; }
    virtual std::optional<PowerLimiterControlProposal> getPowerLimiterControlProposal(uint16_t) const { return std::nullopt; }
    virtual std::vector<PowerLimiterTargetDispatchEvent> consumePowerLimiterTargetDispatchEvents() { return {}; }
    virtual uint16_t applyPowerLimiterInputPowerIncrease(uint16_t) { return 0; }
    virtual uint16_t applyPowerLimiterInputPowerReduction(uint16_t) { return 0; }
    virtual void setPowerLimiterLimitedByAvailablePower(bool) { }
};

} // namespace GridChargers
