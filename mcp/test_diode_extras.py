"""recommend_parts' diode text line carries the operating-point loss, over-rating, board area
and price the structured result holds (owner 2026-11-14, diode selector).

    KELVIN_TAS_DATA_DIR=/path/to/TAS/data python3 -m pytest mcp/test_diode_extras.py

Real catalogue records through the real engine; it fails loudly without a catalogue.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import server as S

# Su et al. buck freewheel diode: Vin 36 V, Vout 12 V, Iout 2 A, fs 80 kHz;
# Vrrm >= 1.2 * 36 V, If(avg) >= 1.25 * 2 A * (1 - 12/36).
REQ = {"ratedReverseVoltage": 43.2, "ratedForwardCurrent": 1.25 * 2 * (1 - 12 / 36)}
POINT = {"position": "buckFreewheel", "inputVoltage": 36, "outputVoltage": 12,
         "outputCurrent": 2, "switchingFrequency": 80e3}


def _run(n=10):
    r = S.recommend_parts("diode", REQ, {"tiebreaker": "lowest_operating_point_loss",
                                         "lossOperatingPoint": POINT, "priceQuantity": 1000}, n)
    text = r.content[0].text
    cands = r.structuredContent["candidates"]
    assert cands, text
    return text, cands


def _line(text, c):
    lines = [ln for ln in text.splitlines() if ln.startswith(f"  {c['mpn']} ")]
    assert len(lines) == 1, (c["mpn"], text)
    return lines[0]


def test_every_term_board_and_price_are_in_the_line():
    text, cands = _run()
    assert "tiebreaker lowest_operating_point_loss" in text
    for c in cands:
        loss = c["_loss"]
        line = _line(text, c)
        t = loss["terms"]
        for k, label in (("conduction", "cond"), ("switching", "sw"), ("leakage", "leak")):
            if t[k].get("watts") is not None:
                assert f"{label} {S._watts(t[k]['watts'])}" in line
            else:
                assert f"{label} UNVERIFIED ({'; '.join(t[k]['missing'])})" in line
        if loss["total"] is not None:
            assert f"| loss total {S._watts(loss['total'])}: " in line
        else:
            assert "| loss total >= " in line and "(incomplete)" in line
        area = c["_boardArea"]
        assert (f"| board {area['mm2']:.1f} mm2" if area.get("mm2") is not None
                else "| board area not stated") in line
        q = (c["_price"].get("quotes") or [None])[0]
        assert (f"| unit cost {q['unitCost']:.2f} {q['currency']}" if q
                else "| price not stated") in line
        axes = c["_overRating"]["axes"]
        assert (f"| over-rated {'/'.join(axes)}" in line) == bool(axes)


def test_complete_candidates_rank_first_and_in_loss_order():
    _, cands = _run(25)
    counts = [len(c["_loss"]["unverified"]) for c in cands]
    assert counts == sorted(counts)
    complete = [c["_loss"]["total"] for c in cands if not c["_loss"]["unverified"]]
    assert complete and complete == sorted(complete)
