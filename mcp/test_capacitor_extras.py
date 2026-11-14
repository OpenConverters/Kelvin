"""recommend_parts' capacitor text line carries the operating-point ESR loss, the checks
(effective capacitance, ripple voltage, ripple-current rating), over-rating, board area, volume
and price the structured result holds (owner 2026-11-14, capacitor operating-point selector).

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_capacitor_extras.py

Real catalogue records through the real engine; it fails loudly without a catalogue.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import server as S

# Su et al. buck at 36 V in: Vout 12 V, Iout 2 A, fs 80 kHz, L 180 uH.
VIN, VOUT, IOUT, FS, L = 36.0, 12.0, 2.0, 80e3, 180e-6
DIPP = (VIN - VOUT) * (VOUT / VIN) / (L * FS)
OUT_REQ = {"capacitance": 150e-6, "ratedVoltage": 1.2 * VOUT,
           "minimumRippleCurrent": 1.25 * DIPP / 12 ** 0.5}
IN_REQ = {"capacitance": 30e-6, "ratedVoltage": 1.2 * VIN}
POINT = {"inputVoltage": VIN, "outputVoltage": VOUT, "outputCurrent": IOUT,
         "switchingFrequency": FS}


def _run(req, point):
    r = S.recommend_parts("capacitor", req, {"lossOperatingPoint": point,
                                             "priceQuantity": 1000}, 8)
    text = r.content[0].text
    cands = r.structuredContent["candidates"]
    assert cands, text
    assert "(tiebreaker lowest_operating_point_loss)" in text
    return text, cands


def _line(text, c):
    lines = [ln for ln in text.splitlines() if ln.startswith(f"  {c['mpn']} ")]
    assert len(lines) == 1, (c["mpn"], text)
    return lines[0]


def test_output_loss_checks_board_volume_and_price_are_in_the_line():
    text, cands = _run(OUT_REQ, {**POINT, "position": "buckOutput",
                                 "inductorRippleCurrent": DIPP, "maxRippleVoltage": 0.1})
    for c in cands:
        loss = c["_loss"]
        line = _line(text, c)
        t = loss["terms"]["esrLoss"]
        if t.get("watts") is not None:
            assert f"| ESR loss {'<= ' if t.get('upperBound') else ''}{S._watts(t['watts'])}" in line
        else:
            assert "| ESR loss UNVERIFIED (" in line
        assert f" at I_ripple {loss['rippleCurrent']:.3g} A" in line
        rv = loss["checks"]["rippleVoltage"]
        if rv.get("volts") is not None:
            assert f"| ripple {rv['volts'] * 1e3:.3g} mV" in line
            assert ("OVER the 100 mV limit" in line) == bool(rv["overLimit"])
        rr = loss["checks"]["rippleCurrentRating"]
        if rr.get("amperes") is not None:
            assert f"| ripple rating {rr['amperes']:.3g} A at the operating point" in line
        else:
            assert "| ripple rating UNVERIFIED (" in line
        area, vol = c["_boardArea"], c["_volume"]
        assert (f"| board {area['mm2']:.1f} mm2" if area.get("mm2") is not None
                else "| board area not stated") in line
        assert (f"| volume {vol['mm3']:.0f} mm3" if vol.get("mm3") is not None
                else "| volume not stated") in line
        q = (c["_price"].get("quotes") or [None])[0]
        assert (f"| unit cost {q['unitCost']:.2f} {q['currency']}" if q
                else "| price not stated") in line
        if c["_overRating"]["axes"]:
            assert f"| over-rated {'/'.join(c['_overRating']['axes'])}" in line


def test_input_ripple_voltage_and_unverified_terms_are_named():
    text, cands = _run(IN_REQ, {**POINT, "position": "buckInput", "maxRippleVoltage": 0.2})
    for c in cands:
        loss = c["_loss"]
        line = _line(text, c)
        assert loss["position"] == "buckInput"
        rv = loss["checks"]["rippleVoltage"]
        if rv.get("volts") is not None:
            assert "dV_in = Iout*D*(1-D)/(fs*Ceff) + Iout*ESR" in rv["basis"]
            assert f"| ripple {rv['volts'] * 1e3:.3g} mV" in line
        else:
            assert "| ripple UNVERIFIED (" in line
        t = loss["terms"]["esrLoss"]
        if t.get("watts") is None:
            assert f"| ESR loss UNVERIFIED ({'; '.join(t['missing'])})" in line
            assert "not stated in the record" in t["missing"][0]
