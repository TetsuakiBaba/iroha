import pytest
import yaml

from iroha_dataset import cli
from iroha_dataset.config import DEFAULT_CONFIG_PATH, load_config
from iroha_dataset.paths import paths_from_config
from iroha_dataset.sources import adapter_names, all_source_info
from iroha_dataset.typo.errors import ERROR_TYPES


def test_default_config_loads():
    cfg = load_config()
    assert cfg["seed"] == 42
    assert cfg.get("typo.clean_ratio") == 0.25
    assert cfg.get("context.max_context_chars") == 256
    assert cfg.get("context.max_previous_sentences") == 2
    assert cfg.get("chunk.min_chars") == 2
    assert cfg.get("chunk.max_chars") == 40
    assert cfg.get("typo.max_errors_per_sample") == 2


def test_split_defaults_match_the_spec():
    cfg = load_config()
    assert (cfg["split.train"], cfg["split.validation"], cfg["split.test"]) == (0.98, 0.01, 0.01)


def test_smoke_config_overlays_default():
    cfg = load_config("config/smoke.yaml")
    assert cfg.get("sources.tatoeba.max_sentences") == 1000
    # 上書きしていない値は default のまま
    assert cfg.get("chunk.max_chars") == 40


def test_overrides_are_parsed_as_yaml():
    cfg = load_config(overrides=["typo.clean_ratio=0.5", "sources.kaken.enabled=false",
                                 "typo.units=[chunk]"])
    assert cfg["typo.clean_ratio"] == 0.5
    assert cfg["sources.kaken.enabled"] is False
    assert cfg["typo.units"] == ["chunk"]


def test_bad_override_raises():
    with pytest.raises(ValueError):
        load_config(overrides=["nonsense"])


def test_missing_key_raises_keyerror():
    with pytest.raises(KeyError):
        load_config()["no.such.key"]


def test_every_registered_source_has_config_defaults():
    data = yaml.safe_load(DEFAULT_CONFIG_PATH.read_text(encoding="utf-8"))
    for name in adapter_names():
        assert name in data["sources"], f"{name} の既定値が default.yaml に無い"
        assert "enabled" in data["sources"][name]


def test_every_source_declares_license_info():
    for info in all_source_info():
        assert info.license and info.attribution and info.homepage
        assert info.name in adapter_names()


def test_config_error_types_cover_the_spec():
    """仕様で挙がっている typo カテゴリがすべて実装・設定されている。"""
    cfg = load_config()
    configured = set(cfg["typo.error_types"])
    required = {"deletion", "insertion", "substitution", "transposition", "repeated_key",
                "missing_double_consonant", "excessive_double_consonant", "mixed_input",
                "weak_finger_omission"}
    assert required <= configured
    assert required <= set(ERROR_TYPES)


def test_paths_layout():
    cfg = load_config(overrides=["data_dir=/tmp/x", "raw_dir=/tmp/y"])
    p = paths_from_config(cfg)
    assert p.kkc.name == "kkc" and p.typo.name == "typo"
    assert p.canonical.name == "canonical" and p.samples.name == "samples"
    assert p.raw == p.raw_root
    assert p.stats_json.name == "stats.json" and p.report_md.name == "REPORT.md"


def test_cli_parser_accepts_all_commands():
    parser = cli.build_parser()
    for command in cli.COMMANDS:
        args = parser.parse_args([command])
        assert args.command == command


def test_cli_sources_command_runs(capsys):
    assert cli.main(["sources"]) == 0
    out = capsys.readouterr().out
    for name in adapter_names():
        assert name in out


def test_cli_rejects_unknown_source():
    with pytest.raises(SystemExit):
        cli.main(["preprocess", "--source", "nonexistent"])
