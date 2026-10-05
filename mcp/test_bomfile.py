"""The BOM-file parser behind crossref_bom — no catalogue needed.

    python3 -m pytest mcp/test_bomfile.py

The catalogue side (identification, cross-reference, the payload contract) is in smoke.py.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from bomfile import expand_designators, read_bom_file

FIXTURES = Path(__file__).parent / "fixtures" / "bom"


def _write(tmp_path: Path, name: str, text: str, encoding: str = "utf-8") -> Path:
    path = tmp_path / name
    path.write_bytes(text.encode(encoding))
    return path


def test_altium_csv_under_a_title_block():
    doc = read_bom_file(FIXTURES / "altium_bom.csv", "altium_bom.csv")
    assert doc["headerRow"] == 8                      # seven title/blank lines above it
    assert doc["columns"]["mpn"] == "Manufacturer Part Number"
    assert doc["columns"]["value"] == "Comment"
    assert doc["unreadColumns"] == ["LibRef"]
    assert doc["skipped"] == {"totals": 1}
    refs = [r["ref"] for r in doc["rows"]]
    assert refs == ["R1", "R2", "R3", "R4", "R5", "C1", "C2", "C3", "C4", "C5", "L1", "D1",
                    "D2", "U1"]
    # Windows-1252 is read, and SAID — a misread µ changes a value
    assert any("Windows-1252" in n for n in doc["notes"])
    l1 = next(r for r in doc["rows"] if r["ref"] == "L1")
    assert l1["value"] == "10µH" and l1["partNumber"] == "744031100"
    assert l1["manufacturer"] == "Würth Elektronik"


def test_kicad_csv():
    doc = read_bom_file(FIXTURES / "kicad_bom.csv", "kicad_bom.csv")
    assert doc["format"] == "delimited text, comma-separated"
    assert doc["notes"] == []
    assert set(doc["unreadColumns"]) == {"Datasheet", "DNP"}
    rows = {r["ref"]: r for r in doc["rows"]}
    assert list(rows) == ["C1", "C2", "C3", "R1", "R2", "R3", "R4", "U1"]
    assert rows["C1"]["partNumber"] is None and rows["C1"]["value"] == "100n"
    assert rows["C3"]["partNumber"] == "885012106006"
    assert rows["R2"]["footprint"] == "Resistor_SMD:R_0402_1005Metric"


def test_tab_separated_txt():
    doc = read_bom_file(FIXTURES / "tab_bom.txt", "tab_bom.txt")
    assert doc["format"] == "delimited text, tab-separated"
    assert doc["headerRow"] == 3
    assert [r["ref"] for r in doc["rows"]] == ["D1", "C1", "C2", "C3", "U1"]


def test_xlsx_generated_here(tmp_path):
    openpyxl = pytest.importorskip("openpyxl")
    book = openpyxl.Workbook()
    notes = book.active
    notes.title = "Notes"
    notes.append(["This workbook's BOM is on the next sheet"])
    sheet = book.create_sheet("BOM")
    sheet.append(["Assembly 42 — bill of materials"])
    sheet.append([])
    sheet.append(["Designator", "Qty", "Manufacturer", "MPN", "Value"])
    sheet.append(["C1-C3", 3, "Würth Elektronik", 885012206095, "100n"])   # numeric MPN cell
    sheet.append(["R1", 1, "YAGEO", "RC0402FR-132K2L", "2k2"])
    path = tmp_path / "bom.xlsx"
    book.save(path)
    doc = read_bom_file(path, "bom.xlsx")
    assert doc["format"] == "xlsx, sheet 'BOM'"
    assert doc["headerRow"] == 3
    assert [r["ref"] for r in doc["rows"]] == ["C1", "C2", "C3", "R1"]
    # an ordering code stored as a NUMBER comes back as the digits an engineer typed
    assert doc["rows"][0]["partNumber"] == "885012206095"


def test_grouped_designators_expand():
    assert expand_designators("C1, C2 C5", 3) == ["C1", "C2", "C5"]
    assert expand_designators("R1-R4", 3) == ["R1", "R2", "R3", "R4"]
    assert expand_designators("R1-4", 3) == ["R1", "R2", "R3", "R4"]
    assert expand_designators("R1 – R3", 3) == ["R1", "R2", "R3"]
    with pytest.raises(ValueError, match="spans two prefixes"):
        expand_designators("R1-C4", 3)
    with pytest.raises(ValueError, match="runs backwards"):
        expand_designators("R4-R1", 3)


def test_quantity_mismatch_names_the_row(tmp_path):
    path = _write(tmp_path, "bad.csv",
                  "Designator,Quantity,MPN\nR1-R3,3,RC0402FR-1310KL\nC1 C2,3,885012206095\n")
    with pytest.raises(ValueError, match=r"row 3 names 2 designator\(s\) \('C1 C2'\) but its "
                                         r"Quantity is 3"):
        read_bom_file(path, "bad.csv")


def test_unknown_header_without_jev_fails_naming_the_columns(tmp_path, monkeypatch):
    for var in ("MOEBIUS_JEV_API_KEY", "OPENROUTER_API_KEY"):
        monkeypatch.delenv(var, raising=False)
    path = _write(tmp_path, "nohead.csv", "Part,Amount,Where\nRC0402FR-1310KL,3,top\n")
    with pytest.raises(ValueError) as error:
        read_bom_file(path, "nohead.csv")
    message = str(error.value)
    assert "('Part', 'Amount', 'Where') is not in Kelvin's column vocabulary" in message
    assert "MOEBIUS_JEV_API_KEY" in message


def test_header_without_a_designator_references_rows(tmp_path):
    path = _write(tmp_path, "noref.csv", "MPN,Value,Notes\nRC0402FR-1310KL,10k,x\nX2,1k,y\n")
    doc = read_bom_file(path, "noref.csv")
    assert [r["ref"] for r in doc["rows"]] == ["row 2", "row 3"]
    assert doc["refKind"] == "row"
    assert any("row number" in n for n in doc["notes"])


def test_a_line_id_column_is_the_reference_and_is_never_split(tmp_path):
    # LumiQuote's id cell holds spaces and a '|': read as designators, every row claimed '|'
    # and the whole BOM was refused as duplicates.
    path = _write(tmp_path, "ids.csv",
                  "ID,MPN,Manufacturer\n"
                  "2056137 | OffTheShelf:567a,ABM10-27.000MHZ-D30-T3,Abracon\n"
                  "2054277 | OffTheShelf:345f,ABM11W-16.0000MHZ-8-K1Z-T3,Abracon\n")
    doc = read_bom_file(path, "ids.csv")
    assert [r["ref"] for r in doc["rows"]] == ["2056137 | OffTheShelf:567a",
                                              "2054277 | OffTheShelf:345f"]
    assert doc["refKind"] == "id" and doc["merged"] == 0
    assert any("referenced by its 'ID'" in n for n in doc["notes"])


def test_rows_naming_the_same_part_are_merged(tmp_path):
    path = _write(tmp_path, "dups.csv",
                  "ID,MPN,Manufacturer,Qty\n"
                  "A1,GRM155R71H103KA88D,Murata,2\n"
                  "A2,BLM18PG121SN1D,Murata,1\n"
                  "A3,grm155r71h103ka88d,Murata,3\n"       # same MPN, other case
                  "A4,BLM18PG121SN1D,Murata,1\n"           # exact duplicate of A2 bar its id
                  "A4,BLM18PG121SN1D,Murata,1\n")          # exact duplicate row
    doc = read_bom_file(path, "dups.csv")
    rows = {r["ref"]: r for r in doc["rows"]}
    assert list(rows) == ["A1", "A2"]
    assert rows["A1"]["refs"] == ["A1", "A3"] and rows["A1"]["quantity"] == 5
    assert rows["A2"]["refs"] == ["A2", "A4"] and rows["A2"]["quantity"] == 3
    assert doc["merged"] == 3 and doc["rowsRead"] == 5
    assert any("3 of 5 BOM rows were duplicates and were merged" in n for n in doc["notes"])


def test_an_id_naming_two_different_parts_is_refused(tmp_path):
    path = _write(tmp_path, "clash.csv", "ID,MPN\nA1,RC0402FR-1310KL\nA1,RC0402FR-132K2L\n")
    with pytest.raises(ValueError, match="'A1' names one part on row 2 and another on row 3"):
        read_bom_file(path, "clash.csv")


def test_a_designator_listed_twice_for_the_same_part_is_merged(tmp_path):
    path = _write(tmp_path, "dup.csv", "Designator,MPN\nR1,RC0402FR-1310KL\nR1,RC0402FR-1310KL\n")
    doc = read_bom_file(path, "dup.csv")
    assert [r["ref"] for r in doc["rows"]] == ["R1"] and doc["merged"] == 1


def test_a_designator_claimed_twice_is_refused(tmp_path):
    path = _write(tmp_path, "dup.csv", "Designator,MPN\nR1,RC0402FR-1310KL\nR1,RC0402FR-132K2L\n")
    with pytest.raises(ValueError, match="R1 appears on row 2 and again on row 3 naming a different part"):
        read_bom_file(path, "dup.csv")


def test_a_part_row_without_a_designator_is_refused(tmp_path):
    path = _write(tmp_path, "noref.csv", "Designator,MPN\nR1,RC0402FR-1310KL\n,RC0402FR-132K2L\n")
    with pytest.raises(ValueError, match="row 3 carries part data"):
        read_bom_file(path, "noref.csv")


def test_ambiguous_separator_is_refused(tmp_path):
    path = _write(tmp_path, "both.csv", "Designator,MPN\nR1,X\nRef;Value\nC1;100n\n")
    with pytest.raises(ValueError, match="separator is ambiguous"):
        read_bom_file(path, "both.csv")


def test_utf16_without_a_bom_is_named(tmp_path):
    path = tmp_path / "u16.csv"
    path.write_bytes("Designator,MPN\nR1,X\n".encode("utf-16-le"))
    with pytest.raises(ValueError, match="UTF-16 without a byte-order mark"):
        read_bom_file(path, "u16.csv")


def test_utf16_with_its_bom_is_read(tmp_path):
    path = tmp_path / "u16.txt"
    path.write_bytes("Designator\tMPN\nR1\tRC0402FR-1310KL\n".encode("utf-16"))
    doc = read_bom_file(path, "u16.txt")
    assert doc["rows"][0]["partNumber"] == "RC0402FR-1310KL" and doc["notes"] == []


def test_line_numbers_survive_a_quoted_newline(tmp_path):
    path = _write(tmp_path, "nl.csv",
                  'Designator,MPN,Description\nR1,A1,"two\nlines"\nR2,A2,x\n')
    doc = read_bom_file(path, "nl.csv")
    assert [(r["ref"], r["row"]) for r in doc["rows"]] == [("R1", 2), ("R2", 4)]


def test_legacy_xls_is_refused(tmp_path):
    path = tmp_path / "old.xls"
    path.write_bytes(b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1" + b"\x00" * 64)
    with pytest.raises(ValueError, match="legacy binary .xls"):
        read_bom_file(path, "old.xls")


# --- Jev: the header Kelvin's vocabulary does not know ----------------------

LUMIQUOTE = ("LumiQuote-ID,Description (IPN),Description (Part),Part category,Offered MPN*,"
             "Offered manufacturer*\n"
             "2056137 | OffTheShelf:567a,Y S CRY 27MHz,Crystal 27MHz 10pF,Crystals,"
             "ABM10-27.000MHZ-D30-T3,Abracon\n"
             "2056007 | OffTheShelf:baaa,Y S CRY 16MHz,Quartz Crystal 16MHz,Crystals,"
             "ABM8-16.000MHZ-B2-T,Abracon\n"
             "2056008 | OffTheShelf:db85,Y S CRY 16MHz,Crystal 16MHz 18pF,Crystals,"
             "ABM8-16.000MHz-B2-T,Abracon\n")
LUMIQUOTE_ROLES = {"LumiQuote-ID": "lineid", "Description (IPN)": "description",
                   "Description (Part)": "description", "Part category": "ignore",
                   "Offered MPN*": "mpn", "Offered manufacturer*": "manufacturer"}


def _fake_jev(monkeypatch, roles: dict[str, str]):
    """Jev, answering from `roles` by header; records what it was asked."""
    import bomfile

    asked = {}

    def decide(state, questions):
        asked.update(questions)
        out = {}
        for qid, q in questions.items():
            header = q["instructions"].split("`")[1]
            assert set(q["criteria"]) >= {"mpn", "ignore"}       # a closed choice
            out[qid] = {"choice": roles[header]}
        return out

    monkeypatch.setattr(bomfile, "_jev_decide", decide)
    return asked


def test_jev_maps_a_header_outside_the_vocabulary(tmp_path, monkeypatch):
    asked = _fake_jev(monkeypatch, LUMIQUOTE_ROLES)
    doc = read_bom_file(_write(tmp_path, "Test BOM.csv", LUMIQUOTE), "Test BOM.csv")
    assert len(asked) == 6                                       # one question per column
    assert doc["columns"]["mpn"] == "Offered MPN*"
    assert doc["columns"]["description"] == "Description (IPN) + Description (Part)"
    assert doc["unreadColumns"] == ["Part category"]
    assert doc["refKind"] == "id"
    rows = doc["rows"]
    # ABM8-16.000MHZ-B2-T and ABM8-16.000MHz-B2-T are one part: one line, both ids
    assert [r["refs"] for r in rows] == [["2056137 | OffTheShelf:567a"],
                                         ["2056007 | OffTheShelf:baaa",
                                          "2056008 | OffTheShelf:db85"]]
    assert rows[0]["description"] == "Y S CRY 27MHz | Crystal 27MHz 10pF"
    assert any("columns identified by Jev" in n and "'Offered MPN*' -> mpn" in n
               for n in doc["notes"])


def test_jev_naming_no_part_number_column_fails_loudly(tmp_path, monkeypatch):
    _fake_jev(monkeypatch, {**LUMIQUOTE_ROLES, "Offered MPN*": "ignore"})
    with pytest.raises(ValueError, match=r"no column holds the manufacturer part number.*"
                                         r"'LumiQuote-ID', 'Description \(IPN\)'"):
        read_bom_file(_write(tmp_path, "b.csv", LUMIQUOTE), "b.csv")


def test_jev_giving_one_role_two_columns_fails_loudly(tmp_path, monkeypatch):
    _fake_jev(monkeypatch, {**LUMIQUOTE_ROLES, "Offered manufacturer*": "mpn"})
    with pytest.raises(ValueError, match=r"2 columns \('Offered MPN\*', 'Offered "
                                         r"manufacturer\*'\) were both called the mpn"):
        read_bom_file(_write(tmp_path, "b.csv", LUMIQUOTE), "b.csv")


def test_jev_answer_outside_the_choice_fails(tmp_path, monkeypatch):
    _fake_jev(monkeypatch, {**LUMIQUOTE_ROLES, "Part category": "category"})
    with pytest.raises(ValueError, match="Jev answered 'category' for column 'Part category'"):
        read_bom_file(_write(tmp_path, "b.csv", LUMIQUOTE), "b.csv")


def _live_key() -> str | None:
    import os
    import sys

    for var in ("MOEBIUS_JEV_API_KEY", "OPENROUTER_API_KEY"):
        if os.environ.get(var):
            return os.environ[var]
    sys.path.insert(0, str(Path.home() / ".claude" / "jev"))
    try:
        import jevlib

        return jevlib.api_key()
    except Exception:                                            # no local jev set-up
        return None


def test_live_jev_maps_the_lumiquote_header(tmp_path, monkeypatch):
    key = _live_key()
    if not key:
        pytest.skip("no Jev key here (MOEBIUS_JEV_API_KEY, OPENROUTER_API_KEY or jevlib)")
    monkeypatch.setenv("OPENROUTER_API_KEY", key)
    doc = read_bom_file(_write(tmp_path, "Test BOM.csv", LUMIQUOTE), "Test BOM.csv")
    assert doc["columns"]["mpn"] == "Offered MPN*"
    assert doc["columns"]["manufacturer"] == "Offered manufacturer*"
    assert doc["refKind"] == "id" and len(doc["rows"]) == 2
