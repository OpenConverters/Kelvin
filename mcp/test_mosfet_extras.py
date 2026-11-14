"""recommend_parts' MOSFET text line carries the operating-point loss, over-rating, board area
and price the structured result holds (owner 2026-11-14, selector item 4).

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_mosfet_extras.py

Real catalogue records through the real engine; it fails loudly without a catalogue.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import server as S

# Su et al. buck high side: Vin 36 V, Vout 12 V, Iout 2 A, fs 80 kHz, Vds >= 1.2 * 36 V.
REQ = {"ratedDrainSourceVoltage": 43.2, "ratedContinuousDrainCurrent": 2.0,
       "maximumOnResistance": 1.0}
POINT = {"position": "buckHighSide", "inputVoltage": 36, "outputVoltage": 12,
         "outputCurrent": 2, "switchingFrequency": 80e3}


def _run(point):
    r = S.recommend_parts("mosfet", REQ, {"tiebreaker": "lowest_operating_point_loss",
                                          "lossOperatingPoint": point, "priceQuantity": 1000}, 5)
    text = r.content[0].text
    cands = r.structuredContent["candidates"]
    assert cands, text
    return text, cands


def _line(text, c):
    lines = [ln for ln in text.splitlines() if ln.startswith(f"  {c['mpn']} ")]
    assert len(lines) == 1, (c["mpn"], text)
    return lines[0]


def test_complete_loss_terms_board_and_price_are_in_the_line():
    text, cands = _run({**POINT, "gateDriveVoltage": 10, "gateDriveCurrent": 1})
    c = cands[0]
    loss = c["_loss"]
    assert loss["total"] is not None
    line = _line(text, c)
    assert f"| loss total {S._watts(loss['total'])}: " in line
    t = loss["terms"]
    for k, label in (("conduction", "cond"), ("switching", "sw"), ("gateDrive", "gate"),
                     ("coss", "Coss")):
        assert f"{label} {S._watts(t[k]['watts'])}" in line
    area = c["_boardArea"]
    assert (f"| board {area['mm2']:.1f} mm2" if area.get("mm2") is not None
            else "| board area not stated") in line
    q = (c["_price"].get("quotes") or [None])[0]
    assert (f"| unit cost {q['unitCost']:.2f} {q['currency']}" if q else "| price not stated") in line


def test_unverified_terms_are_named_not_estimated():
    text, cands = _run(POINT)  # no gate drive: switching and gate terms cannot be computed
    c = cands[0]
    assert c["_loss"]["total"] is None
    line = _line(text, c)
    assert "| loss total >= " in line and "(incomplete)" in line
    assert "sw UNVERIFIED (gate drive current not given" in line
    assert "gate UNVERIFIED (gate drive voltage not given)" in line
