#!/usr/bin/env python3
"""かな漢字変換モデルの学習（自作モデルの出発点）。

llm-jp/llm-jp-3-150m（Llama系・SentencePiece Unigram）を既定の基盤モデルとして、
prepare_data.py で作った学習テキストでファインチューニングする。
ku-nlp/gpt2-small-japanese-char 等、AutoModelForCausalLM で読めるモデルなら --base で差し替え可。

使い方:
    pip install torch transformers datasets accelerate
    # 基盤モデルの準備（tokenizer.model はGGUF変換に必須。HFリポジトリに無いので別途取得）
    huggingface-cli download llm-jp/llm-jp-3-150m --local-dir ./base/llm-jp-3-150m
    curl -sL -o ./base/llm-jp-3-150m/tokenizer.model \
      https://github.com/llm-jp/llm-jp-tokenizer/raw/main/models/ver3.0/llm-jp-tokenizer-100k.ver3.0b1.model
    python3 train.py --data train.txt --out ./iroha-llmjp-150m

- 損失は出力部（U+EE01以降）だけに掛ける（zenzと同じ方針）
- CUDA GPU推奨。MPS（Apple Silicon）でも小規模なら動く
- --from-scratch でランダム初期化からのフルスクラッチ学習（要・大規模データ）
"""
import argparse
import json
import os
import shutil

OUTPUT_TAG = "\uee01"
SPECIAL_TOKENS = ["\uee00", "\uee01", "\uee02"]  # 入力タグ・出力タグ・文脈タグ


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", default="train.txt")
    parser.add_argument("--out", default="./iroha-model")
    parser.add_argument("--base", default="./base/llm-jp-3-150m",
                        help="基盤モデル（ローカルディレクトリ推奨。tokenizer.modelを同梱すること）")
    parser.add_argument("--from-scratch", action="store_true",
                        help="事前学習済み重みを使わずランダム初期化から学習する")
    parser.add_argument("--epochs", type=float, default=2.0)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--grad-accum", type=int, default=8,
                        help="勾配累積（実効バッチ = batch-size × grad-accum）")
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--max-length", type=int, default=128)
    parser.add_argument("--save-steps", type=int, default=2000)
    parser.add_argument("--warmup-steps", type=int, default=250)
    parser.add_argument("--num-proc", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                        help="トークナイズの並列プロセス数")
    parser.add_argument("--resume", action="store_true",
                        help="出力ディレクトリの最新チェックポイントから同一ランを再開する"
                             "（中断・クラッシュからの復帰用。設定やデータを変えたランには使わない）")
    parser.add_argument("--llama-vocab", default=None,
                        help="vocab-only GGUFのパス（make-vocab-gguf.shで生成）。指定すると"
                             "llama.cppのトークナイザで学習データをトークン化し、iroha推論時"
                             "（llama.cpp）とトークン列を完全一致させる。HFのUnigram Viterbiと"
                             "llama.cppのバイグラム結合近似は同じ語彙でも分割が食い違い、"
                             "不一致プロンプトでは変換精度が大きく落ちるため、常用を推奨。"
                             "要: pip install llama-cpp-python")
    args = parser.parse_args()

    import torch
    from datasets import load_dataset
    from transformers import (
        AutoConfig,
        AutoModelForCausalLM,
        AutoTokenizer,
        Trainer,
        TrainingArguments,
    )

    tokenizer = AutoTokenizer.from_pretrained(args.base)
    if args.from_scratch:
        config = AutoConfig.from_pretrained(args.base)
        model = AutoModelForCausalLM.from_config(config)
    else:
        model = AutoModelForCausalLM.from_pretrained(args.base)

    # zenzの特殊トークン（私用領域）を1トークンとして追加。
    # add_special_tokens でないと Unigram 系トークナイザでバイト列に分割されてしまう。
    tokenizer.add_special_tokens({"additional_special_tokens": SPECIAL_TOKENS})
    # 語彙がembedding行数を超えたときだけ拡張する（llm-jp-3はconfig側に余剰行が
    # あるため不要。縮小方向のresizeはGGUF変換時の不整合の元なので行わない）
    embedding_rows = model.get_input_embeddings().num_embeddings
    if len(tokenizer) > embedding_rows:
        model.resize_token_embeddings(len(tokenizer))
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token
    output_tag_id = tokenizer.convert_tokens_to_ids(OUTPUT_TAG)

    dataset = load_dataset("text", data_files=args.data, split="train")

    def encode_batch_hf(texts):
        return tokenizer(
            [text + tokenizer.eos_token for text in texts],
            truncation=True,
            max_length=args.max_length,
        )["input_ids"]

    def encode_batch_llama(texts):
        # llama.cpp本体のトークナイザを使う（iroha推論時と同一実装・同一分割）。
        # datasets.mapのワーカープロセスごとに1回だけロードする
        global _LLAMA_VOCAB
        try:
            _LLAMA_VOCAB
        except NameError:
            from llama_cpp import Llama
            _LLAMA_VOCAB = Llama(model_path=args.llama_vocab, vocab_only=True,
                                 verbose=False)
        eos_id = _LLAMA_VOCAB.token_eos()
        result = []
        for text in texts:
            # add_bos=True・special=True は ZenzEngine の tokenize(addSpecial: true) と同じ
            ids = _LLAMA_VOCAB.tokenize(text.encode("utf-8"), add_bos=True, special=True)
            result.append(ids[: args.max_length - 1] + [eos_id])
        return result

    def tokenize(batch):
        # attention_mask と labels は input_ids から導出できるため保存しない
        # （collateで生成する。1億行規模ではキャッシュが3倍差＝100GB級の差になる）
        if args.llama_vocab:
            return {"input_ids": encode_batch_llama(batch["text"])}
        return {"input_ids": encode_batch_hf(batch["text"])}

    # llama.cppトークナイザはfork安全でない（macOSでは子プロセスがデッドロックする）ため
    # メインプロセスでトークン化する。実測 約3.5万行/s なので全件1.29億行でも約1時間
    map_num_proc = None if args.llama_vocab else args.num_proc
    tokenized = dataset.map(tokenize, batched=True, num_proc=map_num_proc,
                            remove_columns=["text"])

    # 動的パディング（バッチ内最長に揃える。固定max_lengthより大幅に速い）
    pad_id = tokenizer.pad_token_id

    def collate(features):
        longest = max(len(f["input_ids"]) for f in features)
        batch = {"input_ids": [], "attention_mask": [], "labels": []}
        for f in features:
            ids = f["input_ids"]
            n = len(ids)
            # U+EE01より前（プロンプト部分）は損失計算から除外する
            labels = []
            seen_output_tag = False
            for token_id in ids:
                labels.append(token_id if seen_output_tag else -100)
                if token_id == output_tag_id:
                    seen_output_tag = True
            batch["input_ids"].append(ids + [pad_id] * (longest - n))
            batch["attention_mask"].append([1] * n + [0] * (longest - n))
            batch["labels"].append(labels + [-100] * (longest - n))
        return {k: torch.tensor(v) for k, v in batch.items()}

    training_args = TrainingArguments(
        output_dir=args.out,
        num_train_epochs=args.epochs,
        per_device_train_batch_size=args.batch_size,
        gradient_accumulation_steps=args.grad_accum,
        learning_rate=args.lr,
        lr_scheduler_type="cosine",
        warmup_steps=args.warmup_steps,
        logging_steps=100,
        save_steps=args.save_steps,
        save_total_limit=3,
        bf16=torch.cuda.is_available()
        and torch.cuda.is_bf16_supported(including_emulation=False),
        dataloader_num_workers=2,
        report_to=[],
    )

    trainer = Trainer(model=model, args=training_args,
                      train_dataset=tokenized, data_collator=collate)
    trainer.train(resume_from_checkpoint=args.resume or None)
    trainer.save_model(args.out)
    tokenizer.save_pretrained(args.out)

    # GGUF変換（convert_hf_to_gguf.py のSentencePiece経路）に必要な tokenizer.model を
    # 基盤モデルディレクトリから引き継ぐ（fast tokenizerのsave_pretrainedは保存しない）
    src = os.path.join(args.base, "tokenizer.model")
    if os.path.isfile(src):
        shutil.copy(src, os.path.join(args.out, "tokenizer.model"))

    # transformers 5.x は追加トークンを tokenizer.json にしか書かないが、
    # convert_hf_to_gguf.py のSPM経路は added_tokens.json / added_tokens_decoder しか
    # 読まない。added_tokens.json を明示的に書き出して特殊トークンをGGUFに反映させる
    # （USER_DEFINED扱いになり、llama.cpp側で1トークンとして認識される）
    added = {t: tokenizer.convert_tokens_to_ids(t) for t in SPECIAL_TOKENS}
    with open(os.path.join(args.out, "added_tokens.json"), "w", encoding="utf-8") as f:
        json.dump(added, f, ensure_ascii=False, indent=2)

    print(f"saved to {args.out}")


if __name__ == "__main__":
    main()
