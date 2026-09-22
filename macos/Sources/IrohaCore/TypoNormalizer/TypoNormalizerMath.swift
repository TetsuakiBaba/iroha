import Foundation
#if canImport(Accelerate)
import Accelerate
#endif

/// batch=1 の推論だけに使う行優先の小さな行列。
///
/// 最大でも 96×768（= 約 300KB）なので、確保のしかたを凝るより読めるほうを取る。
/// `rows` は書き換えられる: KV キャッシュは確保だけ先にして 1 行ずつ伸ばす
final class Mat {
    let capacityRows: Int
    let cols: Int
    private(set) var rows: Int
    let data: UnsafeMutablePointer<Float>

    init(rows: Int, cols: Int, capacityRows: Int? = nil) {
        self.capacityRows = capacityRows ?? rows
        self.cols = cols
        self.rows = rows
        data = UnsafeMutablePointer<Float>.allocate(capacity: max(1, self.capacityRows * cols))
        data.initialize(repeating: 0, count: max(1, self.capacityRows * cols))
    }

    deinit { data.deallocate() }

    subscript(row: Int) -> UnsafeMutablePointer<Float> { data.advanced(by: row * cols) }

    /// 末尾に `count` 行ぶん場所を空けて、その先頭を返す（KV キャッシュの追記用）
    func appendRows(_ count: Int) -> UnsafeMutablePointer<Float> {
        precondition(rows + count <= capacityRows, "KVキャッシュの容量を超えました")
        let start = data.advanced(by: rows * cols)
        rows += count
        return start
    }
}

/// PyTorch の `nn.Linear(inDim, outDim)`。weight は [outDim, inDim] の行優先
struct LinearWeights {
    let weight: UnsafePointer<Float>
    let bias: UnsafePointer<Float>
    let inDim: Int
    let outDim: Int
}

/// PyTorch の `nn.LayerNorm(dim)`（eps は既定の 1e-5）
struct LayerNormWeights {
    let weight: UnsafePointer<Float>
    let bias: UnsafePointer<Float>
    let dim: Int
}

enum TypoMath {

    /// C(m×n) = alpha * A(m×k) * op(B) + beta * C。A / B / C の行の間隔（leading dimension）は
    /// 別に渡せる: ヘッドに分けた Q/K/V は 192 列の行列の一部を 48 列ぶんだけ見るため
    static func gemm(
        m: Int, n: Int, k: Int,
        alpha: Float,
        a: UnsafePointer<Float>, lda: Int,
        b: UnsafePointer<Float>, ldb: Int, transposeB: Bool,
        beta: Float,
        c: UnsafeMutablePointer<Float>, ldc: Int
    ) {
        #if canImport(Accelerate)
        cblas_sgemm(
            CblasRowMajor, CblasNoTrans, transposeB ? CblasTrans : CblasNoTrans,
            Int32(m), Int32(n), Int32(k), alpha,
            a, Int32(lda), b, Int32(ldb), beta, c, Int32(ldc))
        #else
        // Accelerate が無い環境（将来の Windows 移植）向けの素の実装
        for i in 0..<m {
            let aRow = a.advanced(by: i * lda)
            let cRow = c.advanced(by: i * ldc)
            for j in 0..<n {
                var sum: Float = 0
                if transposeB {
                    let bRow = b.advanced(by: j * ldb)
                    for p in 0..<k { sum += aRow[p] * bRow[p] }
                } else {
                    for p in 0..<k { sum += aRow[p] * b[p * ldb + j] }
                }
                cRow[j] = alpha * sum + beta * cRow[j]
            }
        }
        #endif
    }

    /// out(T×outDim) = x(T×inDim) · weightᵀ + bias
    static func linear(_ w: LinearWeights, x: Mat, out: Mat) {
        precondition(x.cols == w.inDim && out.cols == w.outDim && out.rows == x.rows)
        gemm(m: x.rows, n: w.outDim, k: w.inDim, alpha: 1,
             a: x.data, lda: x.cols, b: w.weight, ldb: w.inDim, transposeB: true,
             beta: 0, c: out.data, ldc: out.cols)
        for row in 0..<out.rows {
            let r = out[row]
            for j in 0..<w.outDim { r[j] += w.bias[j] }
        }
    }

    /// 同上だが、出力を任意のポインタ（KVキャッシュの末尾など）へ書く
    static func linear(_ w: LinearWeights, x: Mat, into out: UnsafeMutablePointer<Float>, ldOut: Int) {
        precondition(x.cols == w.inDim)
        gemm(m: x.rows, n: w.outDim, k: w.inDim, alpha: 1,
             a: x.data, lda: x.cols, b: w.weight, ldb: w.inDim, transposeB: true,
             beta: 0, c: out, ldc: ldOut)
        for row in 0..<x.rows {
            let r = out.advanced(by: row * ldOut)
            for j in 0..<w.outDim { r[j] += w.bias[j] }
        }
    }

    /// LayerNorm（分散は PyTorch と同じ不偏でないほう、eps=1e-5）
    static func layerNorm(_ w: LayerNormWeights, x: Mat, out: Mat) {
        precondition(x.cols == w.dim && out.cols == w.dim && out.rows == x.rows)
        let n = Float(w.dim)
        for row in 0..<x.rows {
            let source = x[row], destination = out[row]
            var mean: Float = 0
            for j in 0..<w.dim { mean += source[j] }
            mean /= n
            var variance: Float = 0
            for j in 0..<w.dim {
                let d = source[j] - mean
                variance += d * d
            }
            variance /= n
            let inverseStd = 1 / (variance + 1e-5).squareRoot()
            for j in 0..<w.dim {
                destination[j] = (source[j] - mean) * inverseStd * w.weight[j] + w.bias[j]
            }
        }
    }

    /// PyTorch 既定の GELU（tanh 近似ではなく erf を使う正確版）
    static func gelu(_ x: Mat) {
        let inverseSqrt2 = Float(0.70710678118654752440)
        for row in 0..<x.rows {
            let r = x[row]
            for j in 0..<x.cols {
                let v = r[j]
                r[j] = 0.5 * v * (1 + erff(v * inverseSqrt2))
            }
        }
    }

    /// 行ごとの softmax（in-place、最大値を引いてから）
    static func softmaxRows(_ data: UnsafeMutablePointer<Float>, rows: Int, cols: Int, stride: Int) {
        for i in 0..<rows {
            let r = data.advanced(by: i * stride)
            var maximum = -Float.greatestFiniteMagnitude
            for j in 0..<cols where r[j] > maximum { maximum = r[j] }
            var sum: Float = 0
            for j in 0..<cols {
                let e = expf(r[j] - maximum)
                r[j] = e
                sum += e
            }
            let inverse = 1 / sum
            for j in 0..<cols { r[j] *= inverse }
        }
    }

    /// 行ごとの log_softmax（logP を足し合わせるのに使う。double で受ける）
    static func logSoftmax(_ row: UnsafePointer<Float>, count: Int) -> [Double] {
        var maximum = -Float.greatestFiniteMagnitude
        for j in 0..<count where row[j] > maximum { maximum = row[j] }
        var sum: Double = 0
        for j in 0..<count { sum += Double(expf(row[j] - maximum)) }
        let logSum = Double(maximum) + Foundation.log(sum)
        return (0..<count).map { Double(row[$0]) - logSum }
    }

    /// 1行だけの log_softmax のうち、ある id の値（全次元を作らずに済ませる）
    static func logSoftmaxValue(_ row: UnsafePointer<Float>, count: Int, at index: Int) -> Double {
        var maximum = -Float.greatestFiniteMagnitude
        for j in 0..<count where row[j] > maximum { maximum = row[j] }
        var sum: Double = 0
        for j in 0..<count { sum += Double(expf(row[j] - maximum)) }
        return Double(row[index]) - (Double(maximum) + Foundation.log(sum))
    }

    static func argmax(_ row: UnsafePointer<Float>, count: Int) -> Int {
        var best = 0
        var bestValue = row[0]
        for j in 1..<count where row[j] > bestValue {
            bestValue = row[j]
            best = j
        }
        return best
    }
}
