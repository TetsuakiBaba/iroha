import Foundation
import CLlama

/// モデルの語彙だけを読み込んだトークナイザ（重みは読まない。追加学習のデータ作成用）。
///
/// 学習データは推論（`ZenzEngine`）と同じ llama.cpp のトークナイザで分割する
/// （`training/train.py --llama-vocab` と同じ理由: 別のトークナイザだと境界が食い違い、
/// 学習したものと推論で見るものがずれる）
public final class VocabTokenizer: @unchecked Sendable {

    private let model: OpaquePointer
    private let vocab: OpaquePointer
    /// `general.architecture`（"gpt2" / "llama" / "t5" など）
    public let architecture: String

    public init(modelPath: String) throws {
        guard FileManager.default.fileExists(atPath: modelPath) else {
            throw ConversionError.modelNotFound(modelPath)
        }
        llama_backend_init()
        var params = llama_model_default_params()
        params.vocab_only = true
        guard let model = llama_model_load_from_file(modelPath, params) else {
            throw ConversionError.modelLoadFailed(modelPath)
        }
        guard let vocab = llama_model_get_vocab(model) else {
            llama_model_free(model)
            throw ConversionError.modelLoadFailed("vocabの取得に失敗")
        }
        self.model = model
        self.vocab = vocab
        var buffer = [CChar](repeating: 0, count: 256)
        let length = llama_model_meta_val_str(model, "general.architecture", &buffer, buffer.count)
        architecture = length > 0 ? String(cString: buffer) : ""
    }

    deinit {
        llama_model_free(model)
    }

    /// エンコーダ・デコーダ型（T5系）か。`ZenzEngine` と同じ判定
    public var isEncoderDecoder: Bool { llama_model_has_encoder(model) && llama_model_has_decoder(model) }

    /// デコーダの開始トークン（エンコーダ・デコーダ型）。無ければ BOS（`ZenzEngine` と同じ）。
    /// 語彙だけの読み込みではハイパーパラメータが入らず `llama_model_decoder_start_token` が -1 を返すので、
    /// メタデータ（`<arch>.decoder_start_token_id`）を直接読む
    public var decoderStartToken: Int32 {
        var buffer = [CChar](repeating: 0, count: 32)
        if llama_model_meta_val_str(model, "\(architecture).decoder_start_token_id", &buffer, buffer.count) > 0,
           let token = Int32(String(cString: buffer)) {
            return token
        }
        let token = llama_model_decoder_start_token(model)
        return token == LLAMA_TOKEN_NULL ? bos : token
    }

    public var vocabSize: Int { Int(llama_vocab_n_tokens(vocab)) }
    public var eos: Int32 { llama_vocab_eos(vocab) }
    public var bos: Int32 { llama_vocab_bos(vocab) }

    /// モデルが出力の終わりに出すトークン。GGUF の `eos_token_id` は信用しない
    /// （zenz-v3 は eos_token_id=2 だが 2 は `<s>`、実際に生成されるのは 3 の `</s>`。
    /// 2 を終端として学習させると終端が壊れて余計な文字を生成し続ける）。
    /// `</s>` があればそれ、無ければ `llama_vocab_eos`
    public lazy var terminator: Int32 = {
        for token in 0..<Int32(vocabSize) where piece(token) == "</s>" { return token }
        return eos
    }()

    /// 出力部の開始タグ（U+EE01）のトークン列。特殊トークンを持つ語彙（T5・llm-jp 系）では 1 トークン、
    /// zenz（gpt2 バイトフォールバック）では UTF-8 の 3 バイトぶんの 3 トークンになる
    public var outputTagTokens: [Int32] {
        tokenize("\u{EE01}", addSpecial: false)
    }

    /// `ZenzEngine.tokenize` と同じ規則（特殊トークンを解釈する。`addSpecial` はモデルの add_bos 設定に従う）
    public func tokenize(_ text: String, addSpecial: Bool = true) -> [Int32] {
        let utf8 = Array(text.utf8)
        var tokens = [llama_token](repeating: 0, count: utf8.count + 8)
        var count = utf8.withUnsafeBufferPointer { buffer in
            buffer.baseAddress!.withMemoryRebound(to: CChar.self, capacity: utf8.count) { pointer in
                llama_tokenize(vocab, pointer, Int32(utf8.count), &tokens, Int32(tokens.count), addSpecial, true)
            }
        }
        if count < 0 {
            // バッファ不足（負数は必要な数）。取り直す
            tokens = [llama_token](repeating: 0, count: Int(-count))
            count = utf8.withUnsafeBufferPointer { buffer in
                buffer.baseAddress!.withMemoryRebound(to: CChar.self, capacity: utf8.count) { pointer in
                    llama_tokenize(vocab, pointer, Int32(utf8.count), &tokens, Int32(tokens.count), addSpecial, true)
                }
            }
        }
        guard count >= 0 else { return [] }
        return Array(tokens.prefix(Int(count)))
    }

    /// トークンの文字列表現（デバッグ・テスト用）
    public func piece(_ token: Int32) -> String {
        var buffer = [CChar](repeating: 0, count: 64)
        let length = llama_token_to_piece(vocab, token, &buffer, Int32(buffer.count), 0, true)
        guard length > 0 else { return "" }
        return String(decoding: buffer.prefix(Int(length)).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}
