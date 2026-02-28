from __future__ import annotations

import os
from typing import Dict, Tuple

import pandas as pd


def write_workbook(
    out_path: str,
    tables: Dict[str, Tuple[pd.DataFrame, pd.DataFrame, pd.Series]],
    report_date_label: str,
    color_map: Dict[int, str],
    scoring_range: Tuple[int, int],
):
    """tables: category -> (raw_matrix, score_matrix, totals_series)

    raw_matrix/score_matrix: index=metric, columns=company_display
    totals_series: index aligned with columns
    """
    import xlsxwriter

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    min_score, max_score = scoring_range

    with pd.ExcelWriter(out_path, engine="xlsxwriter") as writer:
        wb = writer.book

        fmt_title = wb.add_format({"bold": True, "font_size": 14})
        fmt_note = wb.add_format({"font_size": 10, "font_color": "#666666"})
        fmt_metric = wb.add_format({"bold": True, "border": 1})

        for cat, (raw_mx, score_mx, totals) in tables.items():
            # score sheet
            sheet_scores = f"{cat}-得分"
            score_mx.to_excel(writer, sheet_name=sheet_scores, startrow=4, startcol=1)
            ws = writer.sheets[sheet_scores]

            ws.write(0, 1, f"网络安全公司横向对比（{cat}）- 评分表", fmt_title)
            ws.write(1, 1, f"报告期: {report_date_label}；评分: {max_score}..{min_score}；缺失值按最低分处理。", fmt_note)
            ws.write(2, 1, "提示：本表仅用于研究，不构成投资建议。", fmt_note)

            # totals row (below table)
            header_row = 4
            data_start_row = header_row + 1
            data_end_row = data_start_row + len(score_mx.index) - 1
            total_row = data_end_row + 1

            ws.write(total_row, 1, "总分", fmt_metric)
            for j, col in enumerate(score_mx.columns, start=2):
                ws.write_number(total_row, j, int(totals[col]), wb.add_format({"border": 1, "bold": True}))

            ws.freeze_panes(data_start_row, 2)
            ws.set_column(1, 1, 24)
            ws.set_column(2, 2 + len(score_mx.columns), 16)

            # conditional formatting for score cells
            first_row = data_start_row
            last_row = data_end_row
            first_col = 2
            last_col = first_col + len(score_mx.columns) - 1

            rng = xlsxwriter.utility.xl_range(first_row, first_col, last_row, last_col)
            for sc in range(min_score, max_score + 1):
                bg = color_map.get(sc)
                if not bg:
                    continue
                ws.conditional_format(rng, {
                    "type": "cell",
                    "criteria": "==",
                    "value": sc,
                    "format": wb.add_format({"bg_color": bg, "border": 1})
                })

            # raw sheet
            sheet_raw = f"{cat}-原值"
            raw_mx.to_excel(writer, sheet_name=sheet_raw, startrow=3, startcol=1)
            ws2 = writer.sheets[sheet_raw]
            ws2.write(0, 1, f"网络安全公司横向对比（{cat}）- 原始指标", fmt_title)
            ws2.write(1, 1, f"报告期: {report_date_label}；来源: 东方财富(akshare)。", fmt_note)
            ws2.freeze_panes(4, 2)
            ws2.set_column(1, 1, 24)
            ws2.set_column(2, 2 + len(raw_mx.columns), 16)

        # readme sheet
        ws = wb.add_worksheet("说明")
        ws.write(0, 0, "说明", fmt_title)
        ws.write(2, 0, "1) 每个类别内按每个科目排序打分；得分范围由 config.scoring.max_score/min_score 控制。", fmt_note)
        ws.write(3, 0, "2) 颜色映射由 config.color_map 控制。", fmt_note)
        ws.write(4, 0, "3) 如需更换指标口径/数据源：修改 config.metrics[*].source_col 或替换 fetch 程序。", fmt_note)
