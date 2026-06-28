# DPL Predictive Control Simulator

This is a small Python reference model for the algorithm specified in
`docs/PowerLimiterPredictiveControl.md`.

It is deliberately independent from the firmware. Use it to describe synthetic
meter samples, pending actuator commands, and edge cases before porting the
validated behavior into the C++ power limiter.

Run the scenarios with:

```sh
python3 -m unittest discover -s tools/dpl_sim -p 'test_*.py'
```

Generate the visual timeline report with:

```sh
python3 tools/dpl_sim/visual_report.py
```

Generate a 10-minute synthetic household regulation report with:

```sh
python3 tools/dpl_sim/household_report.py
```
