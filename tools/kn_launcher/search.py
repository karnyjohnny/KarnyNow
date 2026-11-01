# search.py — the smart title matcher.
#
# Requirement (operator, verbatim example): typing "endfield" must find
# "Arknights: Endfield". Tiers, best first:
#   0  substring in the title (case/diacritic-folded) — word-start and
#      earlier matches rank higher inside the tier
#   1  every query token is a prefix of some title token ("ark end" ->
#      "Arknights: Endfield")
#   2  the folded query is a subsequence of the folded title ("akend" ->
#      "Arknights: Endfield"), word-start hits ranked higher
# Empty query = the full (pre-sorted) list. Results are capped.

from __future__ import annotations

import unicodedata

CAP = 100


def fold(s: str) -> str:
    """lowercase + strip diacritics (the catalog carries names like
    "Wiedźmin" and operators type "wiedzmin")."""
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if not unicodedata.combining(c)).lower()


def _tokens(s: str) -> list[str]:
    out, cur = [], []
    for ch in s:
        if ch.isalnum():
            cur.append(ch)
        elif cur:
            out.append("".join(cur))
            cur = []
    if cur:
        out.append("".join(cur))
    return out


def _subseq_rank(q: str, t: str) -> int:
    """0 = q is a subsequence of t; the score rewards word-start hits
    (lower is better); -1 = not a subsequence."""
    score, ti, at_word_start = 0, 0, True
    for qc in q:
        found = False
        while ti < len(t):
            c = t[ti]
            ti += 1
            if c == qc:
                if not at_word_start:
                    score += 1
                found = True
                at_word_start = False
                break
            if not c.isalnum():
                at_word_start = True
            else:
                at_word_start = False
        if not found:
            return -1
    return score


def smart_filter(games: list[dict], query: str, cap: int = CAP) -> list[dict]:
    q = fold(query.strip())
    if not q:
        return games[:cap]
    qt = _tokens(q)
    scored = []
    for g in games:
        title = fold(g.get("title", ""))
        dev = fold(g.get("developer", ""))
        rank, key = None, 0
        pos = title.find(q)
        if pos >= 0:
            rank = 0
            word_start = pos == 0 or not title[pos - 1].isalnum()
            key = (0 if word_start else 1, pos)
        elif dev.find(q) >= 0:
            rank = 0
            key = (2, dev.find(q))
        elif qt and all(any(w.startswith(tok) for w in _tokens(title))
                        for tok in qt):
            rank = 1
            key = (0, 0)
        else:
            sq = _subseq_rank(q.replace(" ", ""), title.replace(" ", ""))
            if sq >= 0:
                rank = 2
                key = (sq, 0)
        if rank is not None:
            scored.append((rank, key, g))
    scored.sort(key=lambda r: (r[0], r[1], r[2].get("title", "").lower()))
    return [g for _, _, g in scored[:cap]]
