from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict, List


class ConfigError(RuntimeError):
    pass


@dataclass(frozen=True)
class Company:
    code: str
    name: str

    @property
    def display(self) -> str:
        return f"{self.name}({self.code})"


@dataclass(frozen=True)
class Category:
    name: str
    companies: List[Company]


@dataclass(frozen=True)
class Metric:
    group: str
    name: str
    source_col: str
    direction: str  # asc|desc

    @property
    def higher_better(self) -> bool:
        if self.direction not in ("asc", "desc"):
            raise ConfigError(f"metric.direction must be asc|desc, got: {self.direction}")
        return self.direction == "desc"


@dataclass(frozen=True)
class Scoring:
    max_score: int
    min_score: int
    missing_value_score: int
    ties: str  # competition|dense
    score_rule: str  # clamp|linear


@dataclass(frozen=True)
class Config:
    data_source: Dict[str, Any]
    scoring: Scoring
    color_map: Dict[int, str]
    categories: List[Category]
    metrics: List[Metric]


def load_config(path: str) -> Config:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)

    try:
        scoring_raw = raw["scoring"]
        scoring = Scoring(
            max_score=int(scoring_raw["max_score"]),
            min_score=int(scoring_raw["min_score"]),
            missing_value_score=int(scoring_raw.get("missing_value_score", scoring_raw["min_score"])),
            ties=str(scoring_raw.get("ties", "competition")),
            score_rule=str(scoring_raw.get("score_rule", "clamp")),
        )

        color_map_raw = raw.get("color_map", {})
        color_map = {int(k): str(v) for k, v in color_map_raw.items()}

        categories: List[Category] = []
        for c in raw["categories"]:
            comps = [Company(code=str(x["code"]).zfill(6), name=str(x["name"])) for x in c["companies"]]
            categories.append(Category(name=str(c["name"]), companies=comps))

        metrics: List[Metric] = []
        for m in raw["metrics"]:
            metrics.append(
                Metric(
                    group=str(m["group"]),
                    name=str(m["name"]),
                    source_col=str(m["source_col"]),
                    direction=str(m["direction"]),
                )
            )

        cfg = Config(
            data_source=dict(raw.get("data_source", {})),
            scoring=scoring,
            color_map=color_map,
            categories=categories,
            metrics=metrics,
        )

    except KeyError as e:
        raise ConfigError(f"missing config key: {e}")

    validate_config(cfg)
    return cfg


def validate_config(cfg: Config) -> None:
    if cfg.scoring.max_score <= 0:
        raise ConfigError("max_score must be > 0")
    if cfg.scoring.min_score < 0:
        raise ConfigError("min_score must be >= 0")
    if cfg.scoring.min_score > cfg.scoring.max_score:
        raise ConfigError("min_score must be <= max_score")
    if cfg.scoring.ties not in ("competition", "dense"):
        raise ConfigError("scoring.ties must be competition|dense")
    if cfg.scoring.score_rule not in ("clamp", "linear"):
        raise ConfigError("scoring.score_rule must be clamp|linear")

    if not cfg.categories:
        raise ConfigError("no categories")
    if not cfg.metrics:
        raise ConfigError("no metrics")

    # ensure unique company codes per category
    for cat in cfg.categories:
        seen = set()
        for co in cat.companies:
            if co.code in seen:
                raise ConfigError(f"duplicate company code in category {cat.name}: {co.code}")
            seen.add(co.code)

    # ensure metric names unique
    mnames = [m.name for m in cfg.metrics]
    if len(set(mnames)) != len(mnames):
        raise ConfigError("metric names must be unique (used as row keys)")

    # ensure color map covers the *positive* score levels if provided.
    # If min_score=0, we allow 0 to be uncolored.
    if cfg.color_map:
        start = max(1, cfg.scoring.min_score)
        for s in range(start, cfg.scoring.max_score + 1):
            if s not in cfg.color_map:
                raise ConfigError(f"color_map missing score {s}")
