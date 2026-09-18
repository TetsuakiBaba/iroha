"""候補選択専用の小型 Transformer encoder（context + reading + candidate → scalar）。

生成はしない。[CLS] 位置の隠れ状態を線形層でスカラーにする。
"""
from __future__ import annotations

from dataclasses import dataclass, asdict

import torch
import torch.nn as nn

from tokenizer import NUM_SEGMENTS, PAD


@dataclass
class RerankerConfig:
    vocab_size: int
    d_model: int = 512
    layers: int = 6
    heads: int = 8
    d_ff: int = 2048
    max_len: int = 320
    dropout: float = 0.1
    pool: str = "cls"  # cls | mean_cand

    def to_dict(self):
        return asdict(self)


# 目安: xs は疎通用、s ≒ 20M、m ≒ 50M、l ≒ 100M、xl ≒ 150M（埋め込み含む）
PRESETS: dict[str, dict] = {
    "xs": dict(d_model=256, layers=4, heads=4, d_ff=1024),
    "s": dict(d_model=512, layers=6, heads=8, d_ff=2048),
    "m": dict(d_model=768, layers=6, heads=12, d_ff=3072),
    "l": dict(d_model=768, layers=12, heads=12, d_ff=3072),
    "xl": dict(d_model=1024, layers=12, heads=16, d_ff=4096),
}


class CharReranker(nn.Module):
    def __init__(self, cfg: RerankerConfig):
        super().__init__()
        self.cfg = cfg
        self.tok_emb = nn.Embedding(cfg.vocab_size, cfg.d_model, padding_idx=PAD)
        self.pos_emb = nn.Embedding(cfg.max_len, cfg.d_model)
        self.seg_emb = nn.Embedding(NUM_SEGMENTS, cfg.d_model)
        self.emb_drop = nn.Dropout(cfg.dropout)
        layer = nn.TransformerEncoderLayer(
            d_model=cfg.d_model, nhead=cfg.heads, dim_feedforward=cfg.d_ff, dropout=cfg.dropout,
            activation="gelu", batch_first=True, norm_first=True)
        self.encoder = nn.TransformerEncoder(
            layer, num_layers=cfg.layers, norm=nn.LayerNorm(cfg.d_model), enable_nested_tensor=False)
        self.head = nn.Linear(cfg.d_model, 1)
        nn.init.normal_(self.tok_emb.weight, std=0.02)
        nn.init.normal_(self.pos_emb.weight, std=0.02)
        nn.init.normal_(self.seg_emb.weight, std=0.02)

    def forward(self, ids: torch.Tensor, segs: torch.Tensor) -> torch.Tensor:
        """ids, segs: [N, T]（PAD=0 で右詰めパディング）→ scores [N]"""
        pad_mask = ids.eq(PAD)  # True = パディング
        positions = torch.arange(ids.size(1), device=ids.device).unsqueeze(0)
        x = self.tok_emb(ids) + self.pos_emb(positions) + self.seg_emb(segs)
        x = self.emb_drop(x)
        h = self.encoder(x, src_key_padding_mask=pad_mask)
        if self.cfg.pool == "mean_cand":
            from tokenizer import SEG_CAND
            m = segs.eq(SEG_CAND).unsqueeze(-1).to(h.dtype)
            pooled = (h * m).sum(1) / m.sum(1).clamp(min=1.0)
        else:
            pooled = h[:, 0]  # [CLS]
        return self.head(pooled).squeeze(-1)

    def count_params(self) -> dict[str, int]:
        emb = sum(p.numel() for p in [self.tok_emb.weight, self.pos_emb.weight, self.seg_emb.weight])
        total = sum(p.numel() for p in self.parameters())
        return {"total": total, "embedding": emb, "non_embedding": total - emb}


def describe_params(model: CharReranker) -> str:
    c = model.count_params()
    return (f"パラメータ数: 合計 {c['total']/1e6:.2f}M（埋め込み {c['embedding']/1e6:.2f}M / "
            f"非埋め込み {c['non_embedding']/1e6:.2f}M）")


if __name__ == "__main__":
    for name, preset in PRESETS.items():
        cfg = RerankerConfig(vocab_size=6573, **preset)
        m = CharReranker(cfg)
        print(name, describe_params(m))
