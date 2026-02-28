from __future__ import annotations

import math
from typing import Dict, Iterable, List, Tuple

import pandas as pd

from lib.config import Scoring


def _is_missing(v) -> bool:
    try:
        return v is None or (isinstance(v, float) and math.isnan(v))
    except Exception:
        return True


def rank_scores(
    items: List[Tuple[str, float]],
    higher_better: bool,
    scoring: Scoring,
) -> Dict[str, Tuple[int, int]]:
    """Return mapping company->(rank, score).

    Ranking methods:
    - competition: 1,2,2,4...
    - dense: 1,2,2,3...

    score_rule:
    - clamp: score = max(max_score + 1 - rank, min_score), then clamp to [min_score, max_score]
    - linear: same formula, but no clamp above max_score (still clamped) and ranks>max_score continue decreasing
      (note: if min_score=1 then ranks >= max_score will go 1,0,-1... and we clamp to min_score anyway)

    Missing values are excluded from ranking; assigned missing_value_score.
    """
    # filter missing
    present = [(k, v) for k, v in items if not _is_missing(v)]

    out: Dict[str, Tuple[int, int]] = {}
    for k, v in items:
        if _is_missing(v):
            out[k] = (10**9, scoring.missing_value_score)

    if not present:
        return out

    present_sorted = sorted(present, key=lambda kv: kv[1], reverse=higher_better)

    # compute ranks with ties
    last_val = None
    rank = 0
    dense_rank = 0
    seen = 0
    for k, v in present_sorted:
        seen += 1
        if last_val is None or v != last_val:
            # new value
            dense_rank += 1
            rank = seen
            last_val = v
        # choose rank method
        r = rank if scoring.ties == "competition" else dense_rank

        # score
        score = scoring.max_score + 1 - r
        if scoring.score_rule == "clamp":
            score = max(score, scoring.min_score)
        # always clamp to [min_score, max_score]
        score = max(scoring.min_score, min(scoring.max_score, score))

        out[k] = (r, int(score))

    return out


def tidy_score(
    df_raw: pd.DataFrame,
    scoring: Scoring,
) -> pd.DataFrame:
    """Input: tidy raw with columns [category, company_display, metric_name, value, higher_better]
    Output: add rank, score
    """

    required = {"category", "company", "metric", "value", "higher_better"}
    missing = required - set(df_raw.columns)
    if missing:
        raise ValueError(f"raw df missing columns: {sorted(missing)}")

    out_rows = []

    for (cat, metric), sub in df_raw.groupby(["category", "metric"], sort=False):
        higher_better = bool(sub["higher_better"].iloc[0])
        items = [(r["company"], r["value"]) for _, r in sub.iterrows()]
        rs = rank_scores(items, higher_better, scoring)

        for _, r in sub.iterrows():
            rk, sc = rs.get(r["company"], (10**9, scoring.missing_value_score))
            out_rows.append({
                **r.to_dict(),
                "rank": rk,
                "score": sc,
            })

    return pd.DataFrame(out_rows)
