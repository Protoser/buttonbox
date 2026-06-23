"""FlyByWire MCDU helpers: parse SimBridge's remote-MCDU WebSocket frames and hold
the key/button maps. Pure (stdlib only) so it unit-tests without Qt or a sim.

SimBridge (ws://localhost:8380/interfaces/v1/mcdu) streams text frames prefixed
"update:" then JSON. The captain MCDU is under the "left" key:
  { "left": { "title": str, "scratchpad": str,
              "lines": [ [seg1, seg2, seg3], ... 12 entries ], ... } }
Each line's three segments are the left / right-aligned / centered portions of a
24-char row. Cells carry markup tokens — {white}{green}{cyan}{amber}{red}…,
{small}, {big}, {sp} (space) — which we strip down to plain ASCII for the box's
mono screen. parse_update() returns 14 flattened rows (0 title … 13 scratchpad).

Input goes back as plain text "event:left:<NAME>" (see KEY_EVENTS). The box owns the
button->output map (settings.mcduMap) and reports key presses as "mcdukey <outIdx>",
an index into MCDU_OUTPUTS (output_event() turns it into the key name to send).
"""
import json
import re

COLS = 24
ROWS = 14                       # 0 title, 1..12 body, 13 scratchpad

_TOKEN_RE = re.compile(r"\{[a-zA-Z0-9 _-]+\}")   # {white},{small},{end},…
# MCDU glyphs that aren't ASCII -> nearest printable stand-in.
_SUBST = {
    "°": "o",   # degree
    "Δ": "^",   # triangle / delta
    "→": ">", "←": "<", "↑": "^", "↓": "v",   # arrows
    "☐": "#", "□": "#", "⬜": "#",                  # entry boxes
    "\xa0": " ",     # non-breaking space
}


def _flatten(cell) -> str:
    """One markup cell -> plain ASCII (tokens stripped, glyphs substituted)."""
    if not cell:
        return ""
    s = str(cell).replace("{sp}", " ")
    for k, v in _SUBST.items():
        s = s.replace(k, v)
    s = _TOKEN_RE.sub("", s)
    return "".join(c for c in s if 32 <= ord(c) < 127)


def _compose(seg1, seg2, seg3) -> str:
    """Rebuild a 24-char row from [left, right, center] segments."""
    left, right, center = _flatten(seg1), _flatten(seg2), _flatten(seg3)
    if not right and not center:
        return left[:COLS].rstrip()
    buf = [" "] * COLS

    def put(text, start):
        start = max(0, min(start, COLS - 1))
        for i, ch in enumerate(text):
            if start + i < COLS:
                buf[start + i] = ch

    if center:
        put(center[:COLS], (COLS - len(center)) // 2)
    put(left[:COLS], 0)                              # edges win over center
    if right:
        put(right[:COLS], COLS - len(right))
    return "".join(buf).rstrip()


def parse_update(message: str):
    """A SimBridge 'update:' frame -> {'title','scratchpad','rows':[14 str]} or None."""
    if not message.startswith("update:"):
        return None
    try:
        left = json.loads(message[len("update:"):]).get("left", {})
    except (ValueError, AttributeError):
        return None
    title = _flatten(left.get("title", ""))[:COLS]
    scratch = _flatten(left.get("scratchpad", ""))[:COLS]
    rows = [""] * ROWS
    rows[0] = title
    rows[ROWS - 1] = scratch
    for i, line in enumerate(left.get("lines", [])[: ROWS - 2]):
        seg = list(line) + ["", "", ""]
        rows[i + 1] = _compose(seg[0], seg[1], seg[2])
    return {"title": title, "scratchpad": scratch, "rows": rows}


# ---- key sets ---------------------------------------------------------------
# Grouped for the companion faceplate (rows of buttons). The flat KEY_EVENTS set
# is every valid <NAME> in "event:left:<NAME>".
LSK_LEFT  = [f"L{i}" for i in range(1, 7)]
LSK_RIGHT = [f"R{i}" for i in range(1, 7)]
FUNCTION  = ["DIR", "PROG", "PERF", "INIT", "DATA", "FPLN",
             "RAD", "FUEL", "SEC", "ATC", "MENU", "AIRPORT"]
SLEW      = ["LEFT", "UP", "RIGHT", "DOWN"]
EDIT      = ["CLR", "OVFY", "DIV", "SP", "DOT", "PLUSMINUS"]
LETTERS   = [chr(c) for c in range(ord("A"), ord("Z") + 1)]
DIGITS    = [str(d) for d in range(10)]
KEY_EVENTS = set(LSK_LEFT + LSK_RIGHT + FUNCTION + SLEW + EDIT + LETTERS + DIGITS)

# Remappable MCDU outputs a physical button can map to. This list is INDEX-ALIGNED
# with the firmware (mcdu.cpp OUT_LABELS) and stored on the device (settings.mcduMap);
# the companion only edits the indices and translates incoming presses. (label, event)
# where event is None for the box-local outputs 0..2 (Off / Scroll Up / Scroll Down) —
# the box handles those itself and never emits them as mcdukey.
MCDU_OUTPUTS = [
    ("Off", None), ("Scroll Up", None), ("Scroll Down", None),
    ("LSK L1", "L1"), ("LSK L2", "L2"), ("LSK L3", "L3"),
    ("LSK L4", "L4"), ("LSK L5", "L5"), ("LSK L6", "L6"),
    ("LSK R1", "R1"), ("LSK R2", "R2"), ("LSK R3", "R3"),
    ("LSK R4", "R4"), ("LSK R5", "R5"), ("LSK R6", "R6"),
    ("MENU", "MENU"), ("CLR", "CLR"), ("OVFY", "OVFY"),
    ("DIR", "DIR"), ("PROG", "PROG"), ("FPLN", "FPLN"),
    ("INIT", "INIT"), ("PERF", "PERF"), ("DATA", "DATA"), ("AIRPORT", "AIRPORT"),
]


def output_event(idx):
    """MCDU key name for an output index (what the box sends as 'mcdukey <idx>'),
    or None if out of range or a box-local output."""
    return MCDU_OUTPUTS[idx][1] if 0 <= idx < len(MCDU_OUTPUTS) else None


# Printable PC keys -> event name when the MCDU pane is focused (letters/digits map
# to themselves). app.py handles non-printable Qt keys (Backspace->CLR, arrows->slew).
KEYBOARD_CHAR_MAP = {".": "DOT", "/": "DIV", " ": "SP", "+": "PLUSMINUS", "-": "PLUSMINUS"}
for _c in LETTERS + DIGITS:
    KEYBOARD_CHAR_MAP[_c] = _c
    KEYBOARD_CHAR_MAP[_c.lower()] = _c


if __name__ == "__main__":   # quick self-test, no sim needed
    sample = "update:" + json.dumps({"left": {
        "title": "{white}      INIT{end}",
        "scratchpad": "{amber}ESSA/ESGG{end}",
        "lines": [
            ["{small}CO RTE", "{small}FROM/TO", ""],
            ["{cyan}[☐☐☐]", "{amber}ESSA/ESGG", ""],
            ["{small}ALTN/CO RTE", "", ""],
            ["{amber}----/----------", "", ""],
            ["", "", "{green}REQUEST{sp}*"],
            ["", "", ""], ["", "", ""], ["", "", ""],
            ["", "", ""], ["", "", ""], ["", "", ""], ["", "", ""],
        ],
    }})
    out = parse_update(sample)
    assert out is not None, "parse failed"
    for i, r in enumerate(out["rows"]):
        assert len(r) <= COLS, f"row {i} too wide: {r!r}"
        assert "{" not in r and "}" not in r, f"row {i} has markup: {r!r}"
        assert all(32 <= ord(c) < 127 for c in r), f"row {i} non-ascii: {r!r}"
        print(f"{i:2} |{r:<24}|")
    assert len(MCDU_OUTPUTS) == 25, "MCDU_OUTPUTS must match firmware MCDU_OUTPUT_COUNT"
    assert output_event(3) == "L1" and output_event(0) is None
    print("OK:", len(KEY_EVENTS), "key events;", len(MCDU_OUTPUTS), "outputs")
