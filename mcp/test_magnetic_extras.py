"""recommend_parts' magnetic text line carries the operating-point ranking the structured result
holds (owner 2026-11-14): L at Ipk, ripple, Ipk, Irms, the DCR and core-loss terms, the
saturation and thermal checks, over-rating, board area and price.

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_magnetic_extras.py

Real catalogue records through the real engine; it fails loudly without a catalogue.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import pytest

import server as S

# Su et al. buck output inductor: 166.7 uH required; Vin 36 V, Vout 12 V, Iout 2 A, fs 80 kHz.
REQ = {"magnetizingInductance": {"nominal": 166.7e-6}}
POINT = {"position": "buckInductor", "conductionMode": "ccm", "inputVoltage": 36,
         "outputVoltage": 12, "outputCurrent": 2, "switchingFrequency": 80e3}


def _run():
    r = S.recommend_parts("magnetic", REQ, {"lossOperatingPoint": POINT,
                                            "priceQuantity": 1000}, 10)
    text = r.content[0].text
    cands = r.structuredContent["candidates"]
    assert cands, text
    return text, cands, r.structuredContent


def _line(text, c):
    lines = [ln for ln in text.splitlines() if ln.startswith(f"  {c['mpn']} ")]
    assert len(lines) == 1, (c["mpn"], text)
    return lines[0]


def test_gates_and_the_line_carry_every_term_and_check():
    text, cands, sc = _run()
    assert sc["tiebreaker"] == "lowest_operating_point_loss"
    rej = sc["rejections"]
    # Power inductors only, and both current checks are hard gates with their own counts.
    assert rej.get("subtype_transformer", 0) > 0
    assert rej.get("saturation_current_below_peak", 0) > 0
    # The rated currents per rise are wired in (MagneticRow::rated_points, owner 2026-11-14): the
    # thermal gate rejects catalogue rows again (MSS1583-224KED, DRQ127-221-R; was == 0 while the
    # index carried no rise).
    assert rej.get("rated_current_below_rms", 0) > 0
    assert rej.get("inductance_below_required", 0) > 0
    for c in cands:
        loss = c["_loss"]
        assert loss["checks"]["saturation"]["status"] != "fail"
        assert loss["checks"]["thermal"]["status"] != "fail"
        assert loss["terms"]["coreLoss"]["watts"] is None
        line = _line(text, c)
        assert f"Ipk {loss['peakCurrent']:.3g} A" in line
        assert "core UNVERIFIED (" in line
        t = loss["terms"]["dcr"]
        assert (f"DCR {S._watts(t['watts'])}" if t.get("watts") is not None
                else "DCR UNVERIFIED (") in line
        area = c["_boardArea"]
        assert (f"| board {area['mm2']:.1f} mm2" if area.get("mm2") is not None
                else "| board area not stated") in line
        # Volume beside the board area (owner 2026-11-14, equal billing).
        vol = c["_volume"]
        assert (f"| volume {vol['mm3']:.0f} mm3" if vol.get("mm3") is not None
                else "| volume not stated") in line
        assert line.count("| CCM") == 1
        assert f"inductance {loss['checks']['inductance']['status']}" in line


def test_unverified_checks_rank_after_verified_ones():
    _, cands, _ = _run()
    keys = [(c.get("sortKey") or c["_sortKey"])["unverifiedItems"] for c in cands]
    assert keys == sorted(keys)
    first = [c for c in cands if (c.get("sortKey") or c["_sortKey"])["unverifiedItems"] == 0]
    sums = [(c.get("sortKey") or c["_sortKey"])["value"] for c in first]
    assert sums == sorted(sums)


def test_conduction_mode_is_required_and_dcm_gates_on_the_boundary():
    # Nothing defaults: an operating point without its conduction mode is refused.
    bad = {k: v for k, v in POINT.items() if k != "conductionMode"}
    with pytest.raises(S.kelvin.InvalidOptions, match="conductionMode is required"):
        S.recommend_parts("magnetic", REQ, {"lossOperatingPoint": bad}, 5)
    # DCM, Vin 24-36 V: L_crit = 12 * (1 - 12/24) / (2 * 2 A * 80 kHz) = 18.75 uH and the gate is
    # 0.9 x L_crit = 16.875 uH (owner 2026-11-14: a 10 % margin; was L_crit); every candidate
    # stays at or below it and the gate has its own rejection count.
    dcm = dict(POINT, conductionMode="dcm", minimumInputVoltage=24)
    r = S.recommend_parts("magnetic", {}, {"lossOperatingPoint": dcm}, 10)
    sc = r.structuredContent
    assert sc["rejections"].get("inductance_above_dcm_maximum", 0) > 0
    assert sc["candidates"]
    for c in sc["candidates"]:
        assert c["_ratings"]["inductance"] <= 0.9 * 18.75e-6 * (1 + 1e-12)
        assert c["_loss"]["conductionMode"] == "dcm"
        assert c["_loss"]["checks"]["inductance"]["status"] != "fail"
    assert "| DCM" in r.content[0].text
