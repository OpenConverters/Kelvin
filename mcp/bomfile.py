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
  - the header, when no row names an MPN or value column and Jev (TypeSafe's decision model,
    one closed choice per column) cannot map one, or maps no part-number column, or gives one
    role two columns;
  - a grouped designator whose count disagrees with the row's Quantity;
  - a row that carries part data but no designator (in a BOM that has a designator column);
  - a designator, or line id, that two rows claim for two DIFFERENT parts.

A BOM with no designator column (a quote or purchasing list) is referenced by its own line-id
column, else by row number, and says so in `notes`. Rows naming the same part — the same MPN
and maker, or the same row twice — are merged into one line carrying every reference and the
summed quantity, and the count merged is in `notes`.

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
    # A line's own identifier, for a BOM that names no positions (a quote or a purchasing
    # list): used verbatim as the line reference, never split like a designator list.
    "lineid": ("id", "lineid", "lineitem", "item", "itemno", "itemnumber", "line", "lineno",
               "linenumber"),
}

# What each role means, for Jev when the header is not in the vocabulary above. One closed
# choice per column: the answer can only be one of these keys.
ROLE_WORDS: dict[str, str] = {
    "mpn": ("the MANUFACTURER part number: the orderable code a distributor recognises "
            "(e.g. `GRM188R71H104KA93D`, `744770133`) — not an internal/house number, a "
            "line-item index or a distributor order number"),
    "manufacturer": "the maker / brand of the part (e.g. `Murata`, `Würth Elektronik`)",
    "ref": ("reference designators: board positions such as `C1`, `R12, R13`, `U4` — NOT a "
            "numeric or alphanumeric line/item/quote identifier"),
    "lineid": ("an identifier of the BOM LINE itself (item number, line number, quote or "
               "database id) that is not a board position"),
    "quantity": "how many of the part are needed (a count)",
    "description": "a free-text description of the part, e.g. `CAP CER 0.1UF 50V X7R 0603`",
    "value": "the electrical value on its own (`100n`, `10k`, `4.7uH`)",
    "footprint": "the package or footprint on its own (`0603`, `SOT-23`, `C_0402`)",
    "ignore": "none of the above (a category, a price, a status, a note, a URL, …)",
}
# A role two columns may share. Two description columns (an internal one and the maker's) are
# common, and both are read, joined; two part-number columns would be a guess.
MULTI_ROLES = ("description",)
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


def _first_wide_row(rows: list[list[str]]) -> int | None:
    """The first row with two or more filled cells: a header candidate when no row names a
    known column (a title block above it fills one cell, not two)."""
    for i, row in enumerate(rows):
        if sum(1 for c in row if c.strip()) >= 2:
            return i
    return None


def _shown(rows: list[list[str]], n: int = 4) -> str:
    seen = [r for r in rows if any(c.strip() for c in r)][:n]
    return " | ".join("[" + ", ".join(repr(c) for c in r if c.strip())[:160] + "]" for r in seen)


def _jev_decide(state, questions):
    """The Jev call, behind one name so tests can stand in for it."""
    from jev import decide

    return decide(state, questions)


def _jev_columns(head: list[str], data: list[list[str]], cols: list[int], roles: list[str],
                 name: str, header_no: int) -> tuple[dict[int, str], str]:
    """Ask Jev which role each of `cols` plays — one closed choice per column, the options
    being `roles` + ignore. Returns ({column index: role}, a note saying what it decided)."""
    from jev import JevError

    samples = {i: [r[i][:60] if i < len(r) else "" for r in data[:5]] for i in cols}
    options = {k: ROLE_WORDS[k] for k in (*roles, "ignore")}
    state = {"headers": [head[i] for i in cols],
             "sample_rows": [{head[i]: samples[i][n] for i in cols}
                             for n in range(min(5, len(data)))]}
    questions = {f"c{i}": {"type": "choice", "criteria": options,
                           "instructions": f"Which role does the column `{head[i]}` play in "
                                           f"this bill of materials? Its first cells read: "
                                           f"{samples[i]}."}
                 for i in cols}
    listed = ", ".join(repr(head[i]) for i in cols)
    try:
        answers = _jev_decide(state, questions)
    except JevError as error:
        raise ValueError(f"{name}: the header on row {header_no} ({listed}) is not in Kelvin's "
                         f"column vocabulary, and Jev, which decides it then, could not be "
                         f"asked: {error}") from error
    out: dict[int, str] = {}
    for i in cols:
        picked = (answers.get(f"c{i}") or {}).get("choice")
        if picked not in options:
            raise ValueError(f"{name}: Jev answered {picked!r} for column {head[i]!r}, which is "
                             f"not one of {sorted(options)}")
        if picked != "ignore":
            out[i] = picked
    said = ", ".join(f"{head[i]!r} -> {out.get(i, 'ignore')}" for i in cols)
    return out, f"columns identified by Jev (row {header_no}: {said})"


def _check_columns(columns: dict[str, list[int]], head: list[str], name: str, header_no: int,
                   decided: str, require_mpn: bool = True) -> None:
    seen = ", ".join(repr(c) for c in head if c.strip())
    for role, idx in columns.items():
        if len(idx) > 1 and role not in MULTI_ROLES:
            raise ValueError(f"{name}: {decided}, but {len(idx)} columns "
                             f"({', '.join(repr(head[i]) for i in idx)}) were both called the "
                             f"{role} — which one holds it is not something to guess. The "
                             f"header is: {seen}")
    if require_mpn and "mpn" not in columns:
        raise ValueError(f"{name}: {decided}, but no column holds the manufacturer part number, "
                         f"so no line can be identified. The header (row {header_no}) is: "
                         f"{seen}. Name the part-number column MPN.")


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
        # Nothing in the vocabulary: the table goes to Jev, IF its separator is not a guess —
        # exactly one of them splits the first row into two or more cells.
        tables = {}
        for delim, label in DELIMITERS.items():
            rows, lines = _csv_rows(text, delim)
            first = _first_wide_row(rows)
            if first is not None:
                tables[label] = (rows, lines, None)
        if len(tables) == 1:
            (label, (rows, lines, header)), = tables.items()
            return rows, lines, header, label, notes
        raise ValueError(
            f"{name}: no row names the BOM's columns, with tab, semicolon or comma as the "
            f"separator, and no row has two or more cells for Jev to map. A header needs an "
            f"MPN or value column. The first rows read: "
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
        with_header, with_data = [], []
        for sheet in book.worksheets:
            rows = [[_cell(v) for v in r] for r in sheet.iter_rows(min_row=1, values_only=True)]
            header = _find_header(rows)
            if header is not None:
                with_header.append((sheet.title, rows, header))
            if _first_wide_row(rows) is not None:
                with_data.append((sheet.title, rows))
        titles = [s.title for s in book.worksheets]
    finally:
        book.close()
    if not with_header:
        # Nothing in the vocabulary: a workbook with ONE sheet holding data goes to Jev.
        if len(with_data) == 1:
            title, rows = with_data[0]
            return rows, list(range(1, len(rows) + 1)), None, f"sheet {title!r}", []
        raise ValueError(
            f"{name}: none of its sheets ({', '.join(repr(t) for t in titles)}) has a row naming "
            f"the BOM's columns (an MPN or value column), and none or several of them hold a "
            f"table for Jev to map — save the BOM's sheet on its own, or as CSV")
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

    decided = "the header was matched to Kelvin's column vocabulary"
    if header is None:
        # No row names a known column. Jev maps the first row with two or more filled cells,
        # column by column, from a closed list of roles; a part-number column is then required.
        header = _first_wide_row(rows)
        head_row = rows[header]
        header_no = numbers[header]
        filled = [i for i, c in enumerate(head_row) if c.strip()]
        jev_roles, note = _jev_columns(head_row, rows[header + 1:], filled,
                                       [r for r in ROLE_WORDS if r != "ignore"], name, header_no)
        notes.append(note)
        decided = note
        roles = jev_roles
        columns: dict[str, list[int]] = {}
        for i, role in roles.items():
            columns.setdefault(role, []).append(i)
        ignored: list[str] = []
        _check_columns(columns, head_row, name, header_no, decided)
    else:
        head_row = rows[header]
        header_no = numbers[header]
        roles = _header_roles(head_row)
        columns = {}
        ignored = []
        for i, role in roles.items():
            names = ROLES[role]
            if role not in columns:
                columns[role] = [i]
                continue
            kept, other = columns[role][0], i
            if norm(head_row[kept]) == norm(head_row[other]):
                raise ValueError(f"{name}: two columns are both called {head_row[i]!r} — which "
                                 f"one holds the {role} is not something to guess")
            if names.index(norm(head_row[other])) < names.index(norm(head_row[kept])):
                kept, other = other, kept
            columns[role] = [kept]
            ignored.append(f"{head_row[other]!r} (a {role} column; {head_row[kept]!r} is used)")
        unknown = [i for i, c in enumerate(head_row) if c.strip() and i not in roles]
        if "mpn" not in columns and unknown:
            # The vocabulary found the header but no part-number column, and some columns it
            # does not know: one of them may be the part number under another name. Jev is
            # asked about those columns only, for the roles still open.
            open_roles = [r for r in ROLE_WORDS if r != "ignore"
                          and (r not in columns or r in MULTI_ROLES)]
            extra, note = _jev_columns(head_row, rows[header + 1:], unknown, open_roles, name,
                                       header_no)
            notes.append(note)
            for i, role in extra.items():
                roles[i] = role
                columns.setdefault(role, []).append(i)
            # The vocabulary already found a value or description column here, so a missing
            # part number is not fatal: the lines are identified by value, as before.
            _check_columns(columns, head_row, name, header_no, note, require_mpn=False)
    unread = [c for i, c in enumerate(head_row) if c.strip() and i not in roles]
    seen = [c for c in head_row if c.strip()]
    if not {"mpn", "value", "description"} & set(columns):
        raise ValueError(
            f"{name}: row {header_no} is the header ({', '.join(repr(c) for c in seen)}) but "
            f"it has neither a part-number column (MPN, Manufacturer Part Number, Mfr Part "
            f"Number, Part Number, PN, Mfg P/N) nor a value column (Value, Comment, "
            f"Description) — there is nothing to identify a part by")
    # The line reference: designators when the BOM has them; else the BOM's own line id;
    # else the row number. Said in notes, because the caller's answer names lines by it.
    if "ref" in columns:
        ref_kind = "designator"
    elif "lineid" in columns:
        ref_kind = "id"
        notes.append(f"the BOM has no designator column: each line is referenced by its "
                     f"{head_row[columns['lineid'][0]]!r}")
    else:
        ref_kind = "row"
        notes.append("the BOM has neither a designator nor a line-id column: each line is "
                     "referenced by its row number ('row N')")

    def get(row: list[str], role: str) -> str:
        cells = [row[i].strip() for i in columns.get(role, ()) if i < len(row) and row[i].strip()]
        return " | ".join(cells)

    out: list[dict] = []
    skipped: dict[str, int] = {}
    claimed: dict[str, dict] = {}
    by_part: dict[str, dict] = {}
    merged = 0
    data_rows = 0

    def part_of(row: list[str]) -> dict:
        return {"partNumber": get(row, "mpn") or None,
                "manufacturer": get(row, "manufacturer") or None,
                "value": get(row, "value") or None,
                "description": get(row, "description") or None,
                "footprint": get(row, "footprint") or None}

    def part_key(part: dict) -> str:
        # The same part: the same MPN from the same maker (case and punctuation aside); a line
        # without an MPN is the same part only when everything it says is the same.
        if part["partNumber"]:
            return "mpn:" + norm(part["partNumber"]) + "|" + norm(part["manufacturer"] or "")
        return "row:" + "|".join(str(part[k] or "") for k in
                                 ("value", "description", "footprint", "manufacturer"))

    for row, row_no in zip(rows[header + 1:], numbers[header + 1:]):
        if not any(c.strip() for c in row):
            skipped["blank"] = skipped.get("blank", 0) + 1
            continue
        if all(c.strip() == head_row[i].strip() for i, c in enumerate(row) if i < len(head_row)) \
                or (ref_kind == "designator" and _header_roles(row) == _header_roles(head_row)):
            skipped["repeated header"] = skipped.get("repeated header", 0) + 1
            continue
        part = part_of(row)
        if ref_kind == "designator":
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
                    f"{name}: row {row_no} names {len(refs)} designator(s) ({ref_cell!r}) but "
                    f"its Quantity is {qty}; one of them is wrong, and which is not for this "
                    f"tool to decide")
            data_rows += 1
            fresh = 0
            for ref in refs:
                # A designator is the line's identity. The same position listed twice for the
                # same part is a duplicate and is merged; for two different parts it is a BOM
                # that contradicts itself, and keeping either would be choosing for the user.
                if ref in claimed:
                    if part_key(claimed[ref]) == part_key(part):
                        continue
                    raise ValueError(
                        f"{name}: {ref} appears on row {claimed[ref]['row']} and again on row "
                        f"{row_no} naming a different part; a reference designator names one "
                        f"position, so one of the rows is wrong")
                line = {"ref": ref, **part, "row": row_no, "refKind": ref_kind}
                claimed[ref] = line
                out.append(line)
                fresh += 1
            if not fresh:
                merged += 1
            continue
        if ref_kind == "id":
            ref = get(row, "lineid")
            if not ref:
                raise ValueError(
                    f"{name}: row {row_no} carries part data "
                    f"({', '.join(repr(c) for c in row if c.strip())[:160]}) but no "
                    f"{head_row[columns['lineid'][0]]!r} — its line cannot be named")
        else:
            ref = f"row {row_no}"
        qty = _quantity(get(row, "quantity"), row_no) if "quantity" in columns else None
        data_rows += 1
        if ref in claimed and part_key(claimed[ref]["_part"]) != part_key(part):
            raise ValueError(f"{name}: {ref!r} names one part on row {claimed[ref]['row']} and "
                             f"another on row {row_no}; which is meant is not for this tool to "
                             f"decide")
        key = part_key(part)
        if key in by_part:
            # The same part on another row is one line: every reference and the summed
            # quantity on it, cross-referenced once.
            line = by_part[key]
            if ref not in line["refs"]:
                line["refs"].append(ref)
            if line["quantity"] is not None or qty is not None:
                line["quantity"] = (line["quantity"] or 0) + (qty or 0)
            line["rows"].append(row_no)
            merged += 1
            claimed.setdefault(ref, {"row": row_no, "_part": part})
            continue
        line = {"ref": ref, **part, "row": row_no, "refKind": ref_kind, "refs": [ref],
                "quantity": qty, "rows": [row_no]}
        by_part[key] = line
        claimed[ref] = {"row": row_no, "_part": part}
        out.append(line)
    if not out:
        raise ValueError(f"{name}: the header is row {header_no}, but no row below it names a "
                         f"part")
    if merged:
        notes.append(f"{merged} of {data_rows} BOM rows were duplicates and were merged: "
                     + ("the same designator listed twice for the same part"
                        if ref_kind == "designator" else
                        "rows naming the same part (MPN + manufacturer) are one line carrying "
                        "every reference and the summed quantity"))
    return {"format": fmt,
            "columns": {role: " + ".join(head_row[i] for i in idx)
                        for role, idx in columns.items()},
            "ignoredColumns": ignored, "unreadColumns": unread, "rows": out,
            "skipped": skipped, "notes": notes, "headerRow": header_no,
            "refKind": ref_kind, "rowsRead": data_rows, "merged": merged}
