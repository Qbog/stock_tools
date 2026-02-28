from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Dict, Optional, Tuple

import pandas as pd

try:
    import akshare as ak
except Exception as e:
    raise RuntimeError(f"akshare import failed: {e}")


def to_em_symbol(code6: str) -> str:
    """Convert 6-digit A-share code to Eastmoney format used by akshare: 300454.SZ, 688xxx.SH"""
    code6 = str(code6).zfill(6)
    if code6.startswith(("60", "68")):
        return f"{code6}.SH"
    return f"{code6}.SZ"


@dataclass
class FetchResult:
    values: Dict[str, float]
    used_report_date: str


def _pick_row(df: pd.DataFrame, report_date: Optional[str]) -> pd.Series:
    if df is None or df.empty:
        raise ValueError("empty dataframe")

    d = df.copy()
    if not pd.api.types.is_datetime64_any_dtype(d["REPORT_DATE"]):
        d["REPORT_DATE"] = pd.to_datetime(d["REPORT_DATE"], errors="coerce")

    if report_date:
        # D=强制不会退：只接受精确命中该报告期
        target = pd.to_datetime(report_date)
        hit = d[d["REPORT_DATE"] == target]
        if hit.empty:
            raise KeyError("report_date_not_found")
        return hit.sort_values("NOTICE_DATE", ascending=False).iloc[0]

    # default: latest full-year if present else latest
    full_year = d[d["REPORT_DATE"].dt.month.eq(12) & d["REPORT_DATE"].dt.day.eq(31)]
    if not full_year.empty:
        return full_year.sort_values("REPORT_DATE", ascending=False).iloc[0]
    return d.sort_values("REPORT_DATE", ascending=False).iloc[0]


def safe_float(x):
    try:
        if x is None:
            return float("nan")
        if isinstance(x, str) and x.strip() == "":
            return float("nan")
        return float(x)
    except Exception:
        return float("nan")


def fetch_em_indicator_row(
    code6: str,
    report_date: Optional[str],
    columns: Dict[str, str],
    indicator: str = "按报告期",
    pause_s: float = 0.25,
    retries: int = 2,
) -> FetchResult:
    """Fetch one row of Eastmoney '财务分析-主要指标' via akshare.

    columns: mapping metric_name -> source_col.
    """
    symbol = to_em_symbol(code6)

    last_err = None
    for attempt in range(retries + 1):
        try:
            df = ak.stock_financial_analysis_indicator_em(symbol=symbol, indicator=indicator)
            try:
                row = _pick_row(df, report_date)
            except KeyError as _:
                # 强制不回退：没有该期就按缺失处理（由上游给 0 分）
                out = {k: float("nan") for k in columns.keys()}
                time.sleep(pause_s)
                return FetchResult(values=out, used_report_date="")

            used = pd.to_datetime(row["REPORT_DATE"]).strftime("%Y-%m-%d")
            out = {k: safe_float(row.get(v)) for k, v in columns.items()}
            time.sleep(pause_s)
            return FetchResult(values=out, used_report_date=used)
        except Exception as e:
            last_err = e
            time.sleep(pause_s * (attempt + 1))

    raise RuntimeError(f"fetch failed for {code6} ({symbol}): {last_err}")
