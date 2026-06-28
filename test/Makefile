# Makefile for FluxDTU tests

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

TEST_EXECS = test_overscaling test_predictive_control test_flexible_load_accounting test_huawei_power_telemetry test_battery_discharge_current_limit

.PHONY: all clean test help

all: $(TEST_EXECS)

# Only build if source file is newer than executable
test_overscaling: test_overscaling.cpp ../src/OverscalingCalculator.cpp
	$(CXX) $(CXXFLAGS) -I../include -o $@ $< ../src/OverscalingCalculator.cpp

test_predictive_control: test_predictive_control.cpp ../src/PowerLimiterPredictiveControl.cpp ../include/PowerLimiterPredictiveControl.h
	$(CXX) $(CXXFLAGS) -I../include -o $@ test_predictive_control.cpp ../src/PowerLimiterPredictiveControl.cpp

test_flexible_load_accounting: test_flexible_load_accounting.cpp ../include/FlexibleLoadAccounting.h
	$(CXX) $(CXXFLAGS) -I../include -o $@ test_flexible_load_accounting.cpp

test_huawei_power_telemetry: test_huawei_power_telemetry.cpp ../include/gridcharger/huawei/PowerTelemetry.h
	$(CXX) $(CXXFLAGS) -I../include -o $@ test_huawei_power_telemetry.cpp

test_battery_discharge_current_limit: test_battery_discharge_current_limit.cpp ../include/PowerLimiterBatteryCurrentLimit.h
	$(CXX) $(CXXFLAGS) -I../include -o $@ test_battery_discharge_current_limit.cpp

test: $(TEST_EXECS)
	@echo "Running overscaling bug fix tests..."
	./test_overscaling
	@echo "Running predictive control core tests..."
	./test_predictive_control
	@echo "Running flexible load accounting tests..."
	./test_flexible_load_accounting
	@echo "Running Huawei power telemetry tests..."
	./test_huawei_power_telemetry
	@echo "Running battery discharge current limit tests..."
	./test_battery_discharge_current_limit

clean:
	rm -f $(TEST_EXECS)

help:
	@echo "Available targets:"
	@echo "  all    - Build test executables"
	@echo "  test   - Build and run tests"
	@echo "  clean  - Remove test executable"
	@echo "  help   - Show this help"
	@echo ""
	@echo "This test verifies the fix for PowerLimiterOverscalingInverter.cpp"
	@echo "Bug: Was using current shading state instead of new shading state"
	@echo "Fix: Now uses new shading state for overscaling calculation"
