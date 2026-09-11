#!/usr/bin/env python3
"""iroha用かな漢字変換モデル（文字単位・エンコーダ／デコーダ）のフルスクラッチ学習。

設計（2026-09-11、M1 Max上のllama-bench較正に基づく）:
- 生成1ステップの時間は層数でほぼ決まり、幅（512↔768）は効かない。
  一方、入力64トークンの一括処理は12層×768でも7ms。
  → 深いエンコーダ（読み＋左文脈を1回だけ双方向に読む）＋浅いデコーダ（2層、0.8ms/文字）
- 文字単位トークナイザ（build_tokenizer.py）: 読みと出力が単調な文字対応になる
- アーキテクチャはT5（HF T5ForConditionalGeneration）。llama.cppがt5として推論できる
  （convert_hf_to_gguf.py → ZenzEngine が llama_encode + デコーダ生成で動かす）

データは prepare_data.py の出力（zenz形式の1行）をそのまま使う:
    [U+EE02左文脈]U+EE00カタカナ読みU+EE01変換結果
  エンコーダ入力 = U+EE01まで（タグ込み）+ </s>、デコーダ目標 = U+EE01の後ろ + </s>

使い方:
    python3 train_t5.py --data ../train-full.txt --tokenizer ./tokenizer.model --out ./iroha-t5-e12d2 \
        --enc-layers 12 --dec-layers 2 --d-model 768 --epochs 1 --batch-size 64 --grad-accum 4
    （実効バッチ = batch-size × grad-accum × GPU数。--eval-samples で末尾を検証用に切り出し
      eval loss を記録する。ベンチ（AJIMEE 200件）のノイズと学習の進みを切り分けるため）
"""
import argparse
import json
import os
import shutil

OUTPUT_TAG = "\uee01"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--tokenizer", default=os.path.join(os.path.dirname(__file__), "tokenizer.model"))
    parser.add_argument("--out", default="./iroha-t5")
    parser.add_argument("--enc-layers", type=int, default=12)
    parser.add_argument("--dec-layers", type=int, default=2)
    parser.add_argument("--d-model", type=int, default=768)
    parser.add_argument("--d-ff", type=int, default=2048, help="gated-gelu のFFN幅（T5 v1.1流。768幅なら2048）")
    parser.add_argument("--heads", type=int, default=12)
    parser.add_argument("--dropout", type=float, default=0.0, help="1エポック未満の大規模データなら0で良い")
    parser.add_argument("--epochs", type=float, default=1.0)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--grad-accum", type=int, default=4)
    parser.add_argument("--lr", type=float, default=5e-4, help="フルスクラッチのAdamW。1e-4は事前学習済み向け")
    parser.add_argument("--warmup-steps", type=int, default=2000)
    parser.add_argument("--weight-decay", type=float, default=0.01)
    parser.add_argument("--max-src-len", type=int, default=160, help="文脈40＋読み＋タグ。文字数")
    parser.add_argument("--max-tgt-len", type=int, default=128)
    parser.add_argument("--save-steps", type=int, default=2000)
    parser.add_argument("--eval-samples", type=int, default=10000, help="データ末尾から検証用に切り出す件数（0=なし）")
    parser.add_argument("--eval-steps", type=int, default=2000)
    parser.add_argument("--max-samples", type=int, default=0, help="先頭N件だけ使う（疎通確認用。0=全部）")
    parser.add_argument("--num-proc", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("--resume", action="store_true", help="出力先の最新チェックポイントから同一ランを再開")
    args = parser.parse_args()

    import sentencepiece as spm
    import torch
    from datasets import load_dataset
    from transformers import T5Config, T5ForConditionalGeneration, Trainer, TrainingArguments

    sp = spm.SentencePieceProcessor(model_file=args.tokenizer)
    pad_id, eos_id, unk_id = sp.pad_id(), sp.eos_id(), sp.unk_id()
    assert (pad_id, eos_id, unk_id) == (0, 1, 2), "build_tokenizer.py の既定ID（pad=0 eos=1 unk=2）を前提にしている"
    vocab_size = sp.get_piece_size()
    # 埋め込み行数は64の倍数に丸める（行列演算の効率。余剰行は未使用）
    padded_vocab = (vocab_size + 63) // 64 * 64

    config = T5Config(
        vocab_size=padded_vocab,
        d_model=args.d_model, d_kv=args.d_model // args.heads, d_ff=args.d_ff,
        num_layers=args.enc_layers, num_decoder_layers=args.dec_layers, num_heads=args.heads,
        relative_attention_num_buckets=32, relative_attention_max_distance=128,
        dropout_rate=args.dropout, layer_norm_epsilon=1e-6,
        feed_forward_proj="gated-gelu", is_encoder_decoder=True, use_cache=True,
        tie_word_embeddings=False,
        pad_token_id=pad_id, eos_token_id=eos_id, decoder_start_token_id=pad_id,
        n_positions=1024,  # 相対位置なので学習上は無関係。GGUFの context_length として記録される
    )
    model = T5ForConditionalGeneration(config)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"params: {n_params/1e6:.1f}M  vocab={vocab_size} (padded {padded_vocab})  "
          f"enc={args.enc_layers} dec={args.dec_layers} d={args.d_model} ff={args.d_ff}")

    dataset = load_dataset("text", data_files=args.data, split="train")
    if args.max_samples:
        dataset = dataset.select(range(min(args.max_samples, len(dataset))))
    eval_dataset = None
    if args.eval_samples and len(dataset) > args.eval_samples * 2:
        split_at = len(dataset) - args.eval_samples
        eval_dataset = dataset.select(range(split_at, len(dataset)))
        dataset = dataset.select(range(split_at))

    def tokenize(batch):
        src_ids, tgt_ids = [], []
        for text in batch["text"]:
            head, sep, tail = text.partition(OUTPUT_TAG)
            if not sep:
                head, tail = text, ""
            # エンコーダ入力の末尾に </s>（T5の慣習。推論側 ZenzEngine も明示的に付ける）
            src = sp.encode(head + OUTPUT_TAG)[: args.max_src_len - 1] + [eos_id]
            tgt = sp.encode(tail)[: args.max_tgt_len - 1] + [eos_id]
            src_ids.append(src)
            tgt_ids.append(tgt)
        return {"input_ids": src_ids, "labels": tgt_ids}

    tokenized = dataset.map(tokenize, batched=True, num_proc=args.num_proc, remove_columns=["text"])
    tokenized_eval = (eval_dataset.map(tokenize, batched=True, num_proc=args.num_proc, remove_columns=["text"])
                      if eval_dataset is not None else None)

    def collate(features):
        src_len = max(len(f["input_ids"]) for f in features)
        tgt_len = max(len(f["labels"]) for f in features)
        input_ids, attention, labels = [], [], []
        for f in features:
            s, t = f["input_ids"], f["labels"]
            input_ids.append(s + [pad_id] * (src_len - len(s)))
            attention.append([1] * len(s) + [0] * (src_len - len(s)))
            labels.append(t + [-100] * (tgt_len - len(t)))
        # decoder_input_ids は labels から自動生成される（先頭に decoder_start_token_id、-100はpadに置換）
        return {"input_ids": torch.tensor(input_ids), "attention_mask": torch.tensor(attention),
                "labels": torch.tensor(labels)}

    use_bf16 = torch.cuda.is_available() and torch.cuda.is_bf16_supported(including_emulation=False)
    training_args = TrainingArguments(
        output_dir=args.out,
        num_train_epochs=args.epochs,
        per_device_train_batch_size=args.batch_size,
        per_device_eval_batch_size=args.batch_size,
        gradient_accumulation_steps=args.grad_accum,
        learning_rate=args.lr,
        weight_decay=args.weight_decay,
        lr_scheduler_type="cosine",
        warmup_steps=args.warmup_steps,
        logging_steps=100,
        save_steps=args.save_steps,
        save_total_limit=3,
        eval_strategy="steps" if tokenized_eval is not None else "no",
        eval_steps=args.eval_steps,
        bf16=use_bf16,
        dataloader_num_workers=2,
        report_to=[],
    )
    trainer = Trainer(model=model, args=training_args, train_dataset=tokenized,
                      eval_dataset=tokenized_eval, data_collator=collate)
    trainer.train(resume_from_checkpoint=args.resume or None)
    trainer.save_model(args.out)

    # GGUF変換（convert_hf_to_gguf.py の T5 経路）は tokenizer.model を直接読む
    shutil.copy(args.tokenizer, os.path.join(args.out, "tokenizer.model"))
    with open(os.path.join(args.out, "iroha-t5.json"), "w", encoding="utf-8") as f:
        json.dump({"tokenizer": os.path.basename(args.tokenizer), "vocab_size": vocab_size,
                   "padded_vocab": padded_vocab, "params": n_params, "args": vars(args)},
                  f, ensure_ascii=False, indent=2)
    print(f"saved to {args.out}")


if __name__ == "__main__":
    main()
