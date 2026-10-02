"""Reading a bill of materials FILE: .csv, .txt (tab, semicolon or comma) and .xlsx.

A layout carries its parts in its own structure; a BOM spreadsheet carries them in whatever
columns the tool that exported it chose, under a title block, with designators grouped. This
module turns one into rows of {ref, partNumber, manufacturer, value, description, footprint} —
one per reference designator — and refuses, with a specific message, every case where it would
otherwise have to guess:

  - the encoding, when the bytes are neither UTF-8, UTF-16 (with its BOM) nor Windows-1252,
    or carry NUL bytes (UTF-16 without its byte-order mark, or not text at all);
  - the delimiter, when more than one of tab / semicolon / comma yields a recognisable header;
  - the sheet, when none or several of a workbook's sheets carry one;
  - the header, when no row names a designator column and an MPN or value column;
  - a grouped designator whose count disagrees with the row's Quantity;
  - a row that carries part data but no designator;
  - a designator that two rows both claim.

Rows that are clearly not parts — blank rows, totals rows, a header repeated on a page break —
are skipped and COUNTED, so the reader is told how many there were. Columns the vocabulary does
not know are NAMED in `unreadColumns`, so a part-number column under an unfamiliar heading
reads as "not read" rather than as "this BOM has no part numbers".

Windows-1252 is accepted, because it is what Altium and Excel write on a Western Windows
machine, but it is SAID in `notes`: a misread µ, Ω or ± changes a value, and the caller must
carry that note into whatever it answers with.

Salvaged from Faraday's BOM work (the file never landed there) and brought into Kelvin, which
owns BOM cross-reference. No MCP here: server.py's crossref_bom is the tool; this is its parser.
"""

from __future__ import annotations

import csv
import io
import re
import zipfile
from pathlib import Path

ZIP_MAGIC = b"PK\x03\x04"
OLE_MAGIC = b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"      # legacy binary .xls

# The column vocabulary, normalised (lower case, letters and digits only). Within a role the
# order is the PRIORITY when a sheet has several matching columns: "MPN" beats a bare "Part
# Number", which in many exports is the company's internal number rather than the maker's.
ROLES: dict[str, tuple[str, ...]] = {
    "ref": ("designator", "designators", "refdes", "reference", "references",
            "partreference", "referencedesignator", "referencedesignators", "ref", "refs"),
    "mpn": ("mpn", "manufacturerpartnumber", "mfrpartnumber", "mfgpartnumber", "mfrpn",
            "mfgpn", "manufacturerpn", "manufacturerpart", "mfrpart", "mfgpart",
            "manufacturerpartno", "mfrpartno", "partnumber", "pn"),
    "manufacturer": ("manufacturer", "manufacturername", "mfr", "mfg", "vendor"),
    "value": ("value", "val", "comment"),
    "description": ("description", "desc"),
    "footprint": ("footprint", "pcbfootprint", "package", "case", "casepackage"),
    "quantity": ("quantity", "qty"),
}
_ROLE_OF = {name: role for role, names in ROLES.items() for name in names}
DELIMITERS = {"\t": "tab", ";": "semicolon", ",": "comma"}


def norm(cell: str) -> str:
    return re.sub(r"[^a-z0-9]", "", str(cell or "").lower())


def _cell(value) -> str:
    """A spreadsheet cell as the text an engineer typed. An integral float is an integer —
    openpyxl hands a numeric ordering code (Würth's 885012206095) or a quantity back as one."""
    if value is None:
        return ""
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value).strip()


def _header_roles(row: list[str]) -> dict[int, str]:
    return {i: _ROLE_OF[norm(c)] for i, c in enumerate(row) if norm(c) in _ROLE_OF}


def _find_header(rows: list[list[str]]) -> int | None:
    """The first row naming at least two known columns. A title block above it ("Bill of
    Materials", "Source Data From: …") names none; a data row names none either."""
    for i, row in enumerate(rows):
        if len(set(_header_roles(row).values())) >= 2:
            return i
    return None


def _shown(rows: list[list[str]], n: int = 4) -> str:
    seen = [r for r in rows if any(c.strip() for c in r)][:n]
    return " | ".join("[" + ", ".join(repr(c) for c in r if c.strip())[:160] + "]" for r in seen)


# --- the file to rows of cells ----------------------------------------------

def _decode(raw: bytes, name: str) -> tuple[str, str | None]:
    """The text, and a note when it was not UTF-8. UTF-8 (with or without a BOM) and UTF-16
    with its BOM — what Excel's 'Unicode text' writes — are read silently. Windows-1252 is
    accepted and SAID, because a misread µ or Ω changes a value."""
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16"), None
    if b"\x00" in raw:
        # UTF-8 decodes NUL happily, so UTF-16 without its byte-order mark would come through
        # as text with a NUL between every letter and fail later as "no header" — the wrong
        # diagnosis. Say what it is.
        raise ValueError(f"{name} contains NUL bytes: it is UTF-16 without a byte-order mark, "
                         f"or not a text file at all — save it as UTF-8 CSV")
    try:
        return raw.decode("utf-8-sig"), None
    except UnicodeDecodeError as error:
        try:
            return raw.decode("cp1252"), (f"{name} is not UTF-8 (byte {error.start}); it was "
                                          f"read as Windows-1252 — check µ, Ω and ± in values")
        except UnicodeDecodeError:
            raise ValueError(f"{name} is neither UTF-8, UTF-16 nor Windows-1252 text — "
                             f"save it as UTF-8 CSV") from error


def _csv_rows(text: str, delim: str) -> tuple[list[list[str]], list[int]]:
    """Records and the 1-based LINE each starts on. A quoted cell may hold a newline, so the
    record index is not the line a text editor shows; the reader's own line counter is."""
    reader = csv.reader(io.StringIO(text), delimiter=delim)
    rows, lines = [], []
    start = 1
    for record in reader:
        rows.append([c.strip() for c in record])
        lines.append(start)
        start = reader.line_num + 1
    return rows, lines


def _text_rows(raw: bytes, name: str) -> tuple[list[list[str]], list[int], int, str, list[str]]:
    """(rows, line numbers, header index, delimiter name, notes) for a delimited text file."""
    text, note = _decode(raw, name)
    if not text.strip():
        raise ValueError(f"{name} is empty")
    notes = [note] if note else []
    found = {}
    for delim, label in DELIMITERS.items():
        rows, lines = _csv_rows(text, delim)
        header = _find_header(rows)
        if header is not None:
            found[label] = (rows, lines, header)
    if not found:
        raise ValueError(
            f"{name}: no row names the BOM's columns, with tab, semicolon or comma as the "
            f"separator. A header needs a designator column (Designator, Ref, RefDes, "
            f"Reference, Part Reference) and an MPN or value column. The first rows read: "
            f"{_shown([[ln] for ln in text.splitlines()])}")
    if len(found) > 1:
        raise ValueError(
            f"{name}: the separator is ambiguous — a header is recognisable when the file is "
            f"split on {' and on '.join(found)}. Re-export it with one separator.")
    (label, (rows, lines, header)), = found.items()
    return rows, lines, header, label, notes


def _xlsx_rows(path: Path, name: str) -> tuple[list[list[str]], list[int], int, str, list[str]]:
    """(rows, row numbers, header index, 'sheet <title>', notes) for a workbook.

    Loaded in full rather than read-only: read-only mode trusts the sheet's declared dimension,
    which some writers get wrong, and silently truncates to it. A BOM is small.
    """
    import openpyxl

    try:
        book = openpyxl.load_workbook(path, data_only=True)
    except (zipfile.BadZipFile, KeyError, OSError) as error:
        raise ValueError(f"{name} is a zip but not a readable .xlsx workbook: {error}") from error
    try:
        with_header = []
        for sheet in book.worksheets:
            rows = [[_cell(v) for v in r] for r in sheet.iter_rows(min_row=1, values_only=True)]
            header = _find_header(rows)
            if header is not None:
                with_header.append((sheet.title, rows, header))
        titles = [s.title for s in book.worksheets]
    finally:
        book.close()
    if not with_header:
        raise ValueError(
            f"{name}: none of its sheets ({', '.join(repr(t) for t in titles)}) has a row naming "
            f"the BOM's columns — a designator column (Designator, Ref, RefDes, Reference) and "
            f"an MPN or value column")
    if len(with_header) > 1:
        raise ValueError(
            f"{name}: several sheets look like a BOM "
            f"({', '.join(repr(t) for t, _, _ in with_header)}); which one is the BOM is not "
            f"something to guess. Save that sheet on its own, or as CSV.")
    title, rows, header = with_header[0]
    return rows, list(range(1, len(rows) + 1)), header, f"sheet {title!r}", []


# --- designators ------------------------------------------------------------

_RANGE = re.compile(r"^([A-Za-z_]+)(\d+)-([A-Za-z_]*)(\d+)$")


def expand_designators(cell: str, row_no: int) -> list[str]:
    """'C1, C2 C5' -> [C1, C2, C5]; 'R1-R4' and 'R1-4' -> [R1, R2, R3, R4]."""
    text = re.sub(r"\s*[-–—]\s*", "-", cell.strip())
    refs: list[str] = []
    for token in re.split(r"[,;\s]+", text):
        if not token:
            continue
        m = _RANGE.match(token)
        if m:
            prefix, lo, prefix2, hi = m.group(1), int(m.group(2)), m.group(3), int(m.group(4))
            if prefix2 and prefix2 != prefix:
                raise ValueError(f"row {row_no}: the designator range {token!r} spans two "
                                 f"prefixes ({prefix} and {prefix2})")
            if hi < lo:
                raise ValueError(f"row {row_no}: the designator range {token!r} runs backwards")
            if hi - lo > 10_000:
                raise ValueError(f"row {row_no}: the designator range {token!r} names "
                                 f"{hi - lo + 1} parts")
            refs += [f"{prefix}{n}" for n in range(lo, hi + 1)]
        else:
            refs.append(token)
    return refs


def _quantity(cell: str, row_no: int) -> int | None:
    if not cell.strip():
        return None
    try:
        q = float(cell.replace(",", "."))
    except ValueError:
        raise ValueError(f"row {row_no}: Quantity {cell!r} is not a number") from None
    if not q.is_integer() or q < 0:
        raise ValueError(f"row {row_no}: Quantity {cell!r} is not a whole count of parts")
    return int(q)


# --- the whole file ---------------------------------------------------------

def read_bom_file(path: Path, name: str) -> dict:
    """Parse a BOM file.

    Returns {format, columns: {role: header text}, ignoredColumns, unreadColumns,
    rows: [{ref, partNumber, manufacturer, value, description, footprint, row}],
    skipped: {reason: count}, notes, headerRow}. `row` is the 1-based line (text) or row
    (sheet) the BOM line starts on, as an editor or spreadsheet program numbers it.
    """
    with path.open("rb") as handle:
        head = handle.read(8)
    if head.startswith(OLE_MAGIC):
        raise ValueError(f"{name} is a legacy binary .xls workbook; save it as .xlsx or CSV")
    if head.startswith(ZIP_MAGIC):
        rows, numbers, header, fmt, notes = _xlsx_rows(path, name)
        fmt = f"xlsx, {fmt}"
    else:
        rows, numbers, header, delim, notes = _text_rows(path.read_bytes(), name)
        fmt = f"delimited text, {delim}-separated"

    head_row = rows[header]
    header_no = numbers[header]
    roles = _header_roles(head_row)
    columns: dict[str, int] = {}
    ignored: list[str] = []
    for i, role in roles.items():
        names = ROLES[role]
        if role not in columns:
            columns[role] = i
            continue
        kept, other = columns[role], i
        if norm(head_row[kept]) == norm(head_row[other]):
            raise ValueError(f"{name}: two columns are both called {head_row[i]!r} — which "
                             f"one holds the {role} is not something to guess")
        if names.index(norm(head_row[other])) < names.index(norm(head_row[kept])):
            kept, other = other, kept
        columns[role] = kept
        ignored.append(f"{head_row[other]!r} (a {role} column; {head_row[kept]!r} is used)")
    unread = [c for i, c in enumerate(head_row) if c.strip() and i not in roles]
    seen = [c for c in head_row if c.strip()]
    if "ref" not in columns:
        raise ValueError(
            f"{name}: row {header_no} is the header ({', '.join(repr(c) for c in seen)}) but "
            f"none of it is a designator column — expected Designator, Ref, RefDes, "
            f"Reference(s) or Part Reference")
    if not {"mpn", "value", "description"} & set(columns):
        raise ValueError(
            f"{name}: row {header_no} is the header ({', '.join(repr(c) for c in seen)}) but "
            f"it has neither a part-number column (MPN, Manufacturer Part Number, Mfr Part "
            f"Number, Part Number, PN, Mfg P/N) nor a value column (Value, Comment, "
            f"Description) — there is nothing to identify a part by")

    def get(row: list[str], role: str) -> str:
        i = columns.get(role)
        return row[i].strip() if i is not None and i < len(row) else ""

    out: list[dict] = []
    skipped: dict[str, int] = {}
    claimed: dict[str, int] = {}
    for row, row_no in zip(rows[header + 1:], numbers[header + 1:]):
        if not any(c.strip() for c in row):
            skipped["blank"] = skipped.get("blank", 0) + 1
            continue
        if _header_roles(row) == roles:
            skipped["repeated header"] = skipped.get("repeated header", 0) + 1
            continue
        ref_cell = get(row, "ref")
        if not ref_cell:
            if any(re.search(r"\btotals?\b", c, re.I) for c in row):
                skipped["totals"] = skipped.get("totals", 0) + 1
                continue
            raise ValueError(
                f"{name}: row {row_no} carries part data "
                f"({', '.join(repr(c) for c in row if c.strip())[:160]}) but no designator — "
                f"a BOM line without one cannot be placed")
        refs = expand_designators(ref_cell, row_no)
        qty = _quantity(get(row, "quantity"), row_no) if "quantity" in columns else None
        if qty is not None and qty != len(refs):
            raise ValueError(
                f"{name}: row {row_no} names {len(refs)} designator(s) ({ref_cell!r}) but its "
                f"Quantity is {qty}; one of them is wrong, and which is not for this tool to "
                f"decide")
        for ref in refs:
            # A designator is the line's identity. Two rows claiming one position is a BOM
            # that contradicts itself, and keeping either would be choosing for the user.
            if ref in claimed:
                raise ValueError(
                    f"{name}: {ref} appears on row {claimed[ref]} and again on row {row_no}; "
                    f"a reference designator names one position, so one of the rows is wrong")
            claimed[ref] = row_no
            out.append({"ref": ref, "partNumber": get(row, "mpn") or None,
                        "manufacturer": get(row, "manufacturer") or None,
                        "value": get(row, "value") or None,
                        "description": get(row, "description") or None,
                        "footprint": get(row, "footprint") or None, "row": row_no})
    if not out:
        raise ValueError(f"{name}: the header is row {header_no}, but no row below it names a "
                         f"part")
    return {"format": fmt, "columns": {role: head_row[i] for role, i in columns.items()},
            "ignoredColumns": ignored, "unreadColumns": unread, "rows": out,
            "skipped": skipped, "notes": notes, "headerRow": header_no}
