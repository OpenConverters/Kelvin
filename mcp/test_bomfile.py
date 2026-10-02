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


def test_no_recognisable_header_lists_what_it_found(tmp_path):
    path = _write(tmp_path, "nohead.csv", "Part,Amount,Where\nRC0402FR-1310KL,3,top\n")
    with pytest.raises(ValueError) as error:
        read_bom_file(path, "nohead.csv")
    message = str(error.value)
    assert "no row names the BOM's columns" in message
    assert "Part,Amount,Where" in message


def test_header_without_a_designator_lists_its_columns(tmp_path):
    path = _write(tmp_path, "noref.csv", "MPN,Value,Notes\nRC0402FR-1310KL,10k,x\n")
    with pytest.raises(ValueError, match=r"'MPN', 'Value', 'Notes'.*none of it is a designator"):
        read_bom_file(path, "noref.csv")


def test_a_designator_claimed_twice_is_refused(tmp_path):
    path = _write(tmp_path, "dup.csv", "Designator,MPN\nR1,RC0402FR-1310KL\nR1,RC0402FR-132K2L\n")
    with pytest.raises(ValueError, match="R1 appears on row 2 and again on row 3"):
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
