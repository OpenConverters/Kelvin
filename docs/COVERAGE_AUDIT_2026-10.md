# Catalogue coverage audit — 2026-10-26

How often each family's parts *state* each filterable spec (share of the family's parts with a
value). Measured with PyKelvin over `PSMA/TAS/data` as of this date; re-run `docs/coverage_audit.py`
to refresh. A low figure means a filter on that spec silently excludes most of the family, so an
AI asking for it gets few or no answers — these are the data gaps that cost Kelvin answers.

| Family | Parts | Least-stated specs (% of parts stating it) |
|---|---:|---|
| resistor | 443,754 | all ≥ 90 % |
| capacitor | 431,873 | `rth` 0.4%, `esl` 0.9%, `v_rated_ac` 3.8%, `ripple_current_rms` 13.2%, `esr_frequency` 16%, `esr` 17.7%, `dielectric_code` 54.2% |
| connector | 416,126 | `mating_cycles` 17.3%, `pitch` 63.1%, `rated_voltage` 71.9%, `termination` 5.2%, `interface_standard` 13.8%, `contact_plating` 23.9% |
| magnetic | 94,271 | `turns_ratio` 2.8%, `saturation_percent_drop` 7.6%, `impedance_100mhz` 13.4%, `impedance_peak` 13.6%, `impedance_peak_freq` 13.6%, `saturation_current` 38.7%, `family` 84.1% |
| diode | 38,109 | `qrr` 1.3%, `rth_jc` 1.9%, `rth_ja` 2.9%, `trr` 7.4%, `junction_capacitance` 7.7%, `vf_typ` 28.7%, `peak_pulse_power_waveform` 30.9%, `clamping_waveform` 31.2%, `peak_pulse_current_waveform` 32.8% |
| timing | 22,418 | `rms_phase_jitter` 26.1%, `esr` 36.9%, `load_capacitance` 37.8%, `frequency_tolerance` 40.1%, `vsupply_min` 59.8%, `vsupply_max` 59.8%, `mode` 37.9% |
| mosfet | 10,126 | `rth_ja` 16.5%, `rth_jc` 30.7%, `tj_max` 34.9%, `vgs_max` 39.5%, `coss` 45.1%, `rds_on_vgs` 66.5%, `qualification` 22.2% |
| controller | 5,937 | `vref` 5.8% |
| analog | 4,942 | `on_resistance` 10.4%, `sample_rate` 26.8%, `resolution` 34.1%, `cmrr` 34.7%, `slew_rate` 35.5%, `gain_bandwidth` 36.6%, `input_stage` 22.3%, `architecture` 36.1% |
| bjt | 4,800 | `tj_max` 0% |
| varistor | 4,232 | all ≥ 90 % |
| igbt | 1,978 | `rth_jc` 24.9%, `tj_max` 79.1% |

## Reading it

Caveat: the denominator is every part in the family, so a spec that does not apply to some parts
(e.g. `qrr` on a zener) understates coverage of the parts it does apply to. Applicability is not
modelled here.

- **Diodes** are the weakest power family: `qrr` 1.3 %, `trr` 7.4 %, thermal resistances < 3 %, `vf_typ` 29 %. Switching-loss and thermal questions cannot be answered from most diode records.
- **MOSFETs** (10,126 parts) state `rth_ja` 17 %, `rth_jc` 31 %, `tj_max` 35 %, `vgs_max` 40 %, `coss` 45 %, `rds_on_vgs` 66 % — the loss/thermal specs a designer selects on.
- **Capacitors** lack `ripple_current_rms` (13 %), `esr` (18 %), `esl` (0.9 %), `v_rated_ac` (3.8 %); safety class is not a field at all.
- **Magnetics**: `saturation_current` 39 %, `saturation_percent_drop` 8 % — an Isat is only comparable at a stated drop criterion.
- **Resistors, varistors and BJTs** are well covered on their selection specs (BJT `tj_max` is 0 %).
- Power-switch volume is small next to passives: 10 k MOSFETs, 2 k IGBTs, 6 k controllers against ~430 k capacitors and ~440 k resistors.

Highest leverage first: diode switching/thermal specs, MOSFET thermal + Coss + Vgs, capacitor ripple/ESR, magnetic Isat criterion.
