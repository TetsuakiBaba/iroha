#include "iroha/typo_normalizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#define IROHA_TYPO_X64 1
#endif

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

bool IsAsciiDigit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool IsAsciiLetter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

// ---- 行列の計算 ----
//
// batch=1 の推論だけに使う。macOS版は Accelerate の cblas_sgemm を使う。ここでは内積を SIMD でまとめて足す
// （足す順が違うので、値は macOS版と最後の桁がずれる。許容幅は macOS版と同じくロジット 1e-3）

// 4 行ぶんの内積 a_r · b（r = 0..3）
using Dot4Function = void (*)(const float* const a[4], const float* b, int k, float out[4]);

void Dot4Scalar(const float* const a[4], const float* b, int k, float out[4]) {
    for (int r = 0; r < 4; ++r) {
        float sum = 0;
        for (int p = 0; p < k; ++p) sum += a[r][p] * b[p];
        out[r] = sum;
    }
}

#ifdef IROHA_TYPO_X64
float HorizontalSum128(__m128 v) {
    __m128 shuffled = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(v, shuffled);
    shuffled = _mm_movehl_ps(shuffled, sums);
    sums = _mm_add_ss(sums, shuffled);
    return _mm_cvtss_f32(sums);
}

void Dot4Sse(const float* const a[4], const float* b, int k, float out[4]) {
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps(), s2 = _mm_setzero_ps(), s3 = _mm_setzero_ps();
    int p = 0;
    for (; p + 4 <= k; p += 4) {
        const __m128 vb = _mm_loadu_ps(b + p);
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a[0] + p), vb));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a[1] + p), vb));
        s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_loadu_ps(a[2] + p), vb));
        s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_loadu_ps(a[3] + p), vb));
    }
    float sums[4] = {HorizontalSum128(s0), HorizontalSum128(s1), HorizontalSum128(s2), HorizontalSum128(s3)};
    for (; p < k; ++p) {
        for (int r = 0; r < 4; ++r) sums[r] += a[r][p] * b[p];
    }
    for (int r = 0; r < 4; ++r) out[r] = sums[r];
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
float HorizontalSum256(__m256 v) {
    return HorizontalSum128(_mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1)));
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,fma")))
#endif
void Dot4Avx2(const float* const a[4], const float* b, int k, float out[4]) {
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps(), s2 = _mm256_setzero_ps(), s3 = _mm256_setzero_ps();
    int p = 0;
    for (; p + 8 <= k; p += 8) {
        const __m256 vb = _mm256_loadu_ps(b + p);
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a[0] + p), vb, s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a[1] + p), vb, s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a[2] + p), vb, s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a[3] + p), vb, s3);
    }
    float sums[4] = {HorizontalSum256(s0), HorizontalSum256(s1), HorizontalSum256(s2), HorizontalSum256(s3)};
    for (; p < k; ++p) {
        for (int r = 0; r < 4; ++r) sums[r] += a[r][p] * b[p];
    }
    for (int r = 0; r < 4; ++r) out[r] = sums[r];
}

bool CpuHasAvx2Fma() {
#ifdef _MSC_VER
    int info[4] = {};
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool fma = (info[2] & (1 << 12)) != 0;
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false; // OS が YMM レジスタを退避するか
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}
#endif

struct Kernel {
    Dot4Function dot4;
    const char* name;
};

const Kernel& SelectedKernel() {
    static const Kernel kernel = [] {
#ifdef IROHA_TYPO_X64
        if (CpuHasAvx2Fma()) return Kernel{&Dot4Avx2, "avx2"};
        return Kernel{&Dot4Sse, "sse2"};
#else
        return Kernel{&Dot4Scalar, "scalar"};
#endif
    }();
    return kernel;
}

// C(m×n) = alpha * A(m×k) · B(n×k)ᵀ（B は行ごとに連続。線形層の重みと、注意の K）
void GemmNT(int m, int n, int k, float alpha, const float* a, int lda, const float* b, int ldb, float* c, int ldc) {
    const Dot4Function dot4 = SelectedKernel().dot4;
    for (int i = 0; i < m; i += 4) {
        const int rows = std::min(4, m - i);
        // 4 行に満たない端は、最後の行を繰り返して埋める（結果は捨てる）
        const float* aRows[4];
        for (int r = 0; r < 4; ++r) aRows[r] = a + static_cast<size_t>(i + std::min(r, rows - 1)) * lda;
        for (int j = 0; j < n; ++j) {
            float dots[4];
            dot4(aRows, b + static_cast<size_t>(j) * ldb, k, dots);
            for (int r = 0; r < rows; ++r) c[static_cast<size_t>(i + r) * ldc + j] = alpha * dots[r];
        }
    }
}

// C(m×n) = A(m×k) · B(k×n)（注意の重み × V）
void GemmNN(int m, int n, int k, const float* a, int lda, const float* b, int ldb, float* c, int ldc) {
    for (int i = 0; i < m; ++i) {
        float* cRow = c + static_cast<size_t>(i) * ldc;
        std::fill(cRow, cRow + n, 0.0f);
        const float* aRow = a + static_cast<size_t>(i) * lda;
        for (int p = 0; p < k; ++p) {
            const float weight = aRow[p];
            const float* bRow = b + static_cast<size_t>(p) * ldb;
            for (int j = 0; j < n; ++j) cRow[j] += weight * bRow[j];
        }
    }
}

// 行優先の小さな行列。rows は伸ばせる（KV キャッシュは確保だけ先にして 1 行ずつ足す）
struct Mat {
    int rows = 0;
    int cols = 0;
    int capacityRows = 0;
    std::vector<float> data;

    Mat(int rowCount, int colCount, int capacity = -1)
        : rows(rowCount), cols(colCount), capacityRows(capacity < 0 ? rowCount : capacity),
          data(static_cast<size_t>(std::max(1, capacityRows * colCount)), 0.0f) {}
    float* Row(int row) { return data.data() + static_cast<size_t>(row) * cols; }
    const float* Row(int row) const { return data.data() + static_cast<size_t>(row) * cols; }
    float* AppendRows(int count) {
        float* start = Row(rows);
        rows += count;
        return start;
    }
};

// PyTorch の nn.Linear(in, out)。weight は [out, in] の行優先
struct Linear {
    const float* weight = nullptr;
    const float* bias = nullptr;
    int in = 0;
    int out = 0;
};

// PyTorch の nn.LayerNorm(dim)（eps 1e-5）
struct Norm {
    const float* weight = nullptr;
    const float* bias = nullptr;
    int dim = 0;
};

// out = x · weightᵀ + bias（out は任意の場所。KV キャッシュの末尾など）
void ApplyLinear(const Linear& w, const Mat& x, float* out, int ldOut) {
    GemmNT(x.rows, w.out, w.in, 1.0f, x.data.data(), x.cols, w.weight, w.in, out, ldOut);
    for (int row = 0; row < x.rows; ++row) {
        float* r = out + static_cast<size_t>(row) * ldOut;
        for (int j = 0; j < w.out; ++j) r[j] += w.bias[j];
    }
}

Mat Project(const Linear& w, const Mat& x) {
    Mat out(x.rows, w.out);
    ApplyLinear(w, x, out.data.data(), out.cols);
    return out;
}

// LayerNorm（分散は PyTorch と同じ不偏でないほう）
Mat LayerNorm(const Norm& w, const Mat& x) {
    Mat out(x.rows, x.cols);
    const float n = static_cast<float>(w.dim);
    for (int row = 0; row < x.rows; ++row) {
        const float* source = x.Row(row);
        float* destination = out.Row(row);
        float mean = 0;
        for (int j = 0; j < w.dim; ++j) mean += source[j];
        mean /= n;
        float variance = 0;
        for (int j = 0; j < w.dim; ++j) {
            const float d = source[j] - mean;
            variance += d * d;
        }
        variance /= n;
        const float inverseStd = 1.0f / std::sqrt(variance + 1e-5f);
        for (int j = 0; j < w.dim; ++j) {
            destination[j] = (source[j] - mean) * inverseStd * w.weight[j] + w.bias[j];
        }
    }
    return out;
}

// PyTorch 既定の GELU（erf を使う正確版）
void Gelu(Mat* x) {
    const float inverseSqrt2 = 0.70710678118654752440f;
    for (float& v : x->data) v = 0.5f * v * (1.0f + std::erf(v * inverseSqrt2));
}

void SoftmaxRows(float* data, int rows, int cols, int stride) {
    for (int i = 0; i < rows; ++i) {
        float* r = data + static_cast<size_t>(i) * stride;
        float maximum = -std::numeric_limits<float>::max();
        for (int j = 0; j < cols; ++j) maximum = std::max(maximum, r[j]);
        float sum = 0;
        for (int j = 0; j < cols; ++j) {
            const float e = std::exp(r[j] - maximum);
            r[j] = e;
            sum += e;
        }
        const float inverse = 1.0f / sum;
        for (int j = 0; j < cols; ++j) r[j] *= inverse;
    }
}

// 1 行の log_softmax のうち index の値（和は double で取る。macOS版と同じ）
double LogSoftmaxValue(const float* row, int count, int index) {
    float maximum = -std::numeric_limits<float>::max();
    for (int j = 0; j < count; ++j) maximum = std::max(maximum, row[j]);
    double sum = 0;
    for (int j = 0; j < count; ++j) sum += static_cast<double>(std::exp(row[j] - maximum));
    return static_cast<double>(row[index]) - (static_cast<double>(maximum) + std::log(sum));
}

int Argmax(const float* row, int count) {
    int best = 0;
    float bestValue = row[0];
    for (int j = 1; j < count; ++j) {
        if (row[j] > bestValue) {
            bestValue = row[j];
            best = j;
        }
    }
    return best;
}

void AddInPlace(Mat* x, const Mat& delta) {
    for (size_t i = 0; i < static_cast<size_t>(x->rows) * x->cols; ++i) x->data[i] += delta.data[i];
}

bool ReadBinaryFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

} // namespace

// ---- 重みの形式 ----

namespace TypoWeightConversion {

float HalfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    const int exponent = (half >> 10) & 0x1F;
    const uint32_t mantissa = half & 0x03FF;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign; // ±0
        } else {
            // 非正規化数: 先頭の 1 が隠れビットの位置に来るまで左へ寄せ、そのぶん指数を下げる
            uint32_t m = mantissa;
            int e = 0;
            do {
                m <<= 1;
                e -= 1;
            } while ((m & 0x0400) == 0);
            m &= 0x03FF;
            bits = sign | static_cast<uint32_t>(127 - 15 + e + 1) << 23 | (m << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13); // Inf / NaN
    } else {
        bits = sign | static_cast<uint32_t>(exponent - 15 + 127) << 23 | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint16_t FloatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mantissa = bits & 0x007FFFFF;
    if (((bits >> 23) & 0xFF) == 0xFF) return sign | 0x7C00 | (mantissa == 0 ? 0 : 0x0200); // Inf / NaN
    if (exponent >= 0x1F) return sign | 0x7C00;                                             // あふれ → Inf
    if (exponent <= 0) {
        if (exponent < -10) return sign; // アンダーフロー → ±0
        const uint32_t m = (mantissa | 0x00800000) >> static_cast<uint32_t>(1 - exponent);
        const uint32_t rounded = (m + 0x0FFF + ((m >> 13) & 1)) >> 13; // 最近接偶数
        return static_cast<uint16_t>(sign | static_cast<uint16_t>(rounded));
    }
    const uint32_t combined = static_cast<uint32_t>(exponent) << 23 | mantissa;
    const uint32_t half = (combined + 0x0FFF + ((combined >> 13) & 1)) >> 13;
    return static_cast<uint16_t>(sign | static_cast<uint16_t>(half));
}

bool IsQ6Target(const std::string& name, const std::vector<int64_t>& shape) {
    if (shape.size() != 2 || shape[1] % static_cast<int64_t>(kQ6Block) != 0) return false;
    for (const char* prefix : {"emb.", "pos_src.", "pos_tgt.", "head."}) {
        if (name.rfind(prefix, 0) == 0) return false;
    }
    return true;
}

// 32 個ずつの区切りに float16 の倍率 d を 1 つ持ち、各値を -32〜31 の整数 q にする（x ≈ q × d）。
// 1 区切り = 倍率 2 バイト + 6 ビット × 32 = 26 バイト（macOS版と同じ）
std::vector<uint8_t> QuantizeQ6(const float* values, size_t count) {
    std::vector<uint8_t> out;
    out.reserve(Q6Bytes(count));
    for (size_t start = 0; start + kQ6Block <= count; start += kQ6Block) {
        float peak = 0;
        for (size_t i = 0; i < kQ6Block; ++i) {
            if (std::fabs(values[start + i]) > std::fabs(peak)) peak = values[start + i];
        }
        const uint16_t scaleHalf = FloatToHalf(peak / -32.0f);
        const float scale = HalfToFloat(scaleHalf);
        const float divisor = scale == 0 ? 1.0f : scale;
        out.push_back(static_cast<uint8_t>(scaleHalf & 0xFF));
        out.push_back(static_cast<uint8_t>(scaleHalf >> 8));
        uint32_t packed[kQ6Block];
        for (size_t i = 0; i < kQ6Block; ++i) {
            const float q = std::min(31.0f, std::max(-32.0f, std::nearbyint(values[start + i] / divisor)));
            packed[i] = static_cast<uint32_t>(static_cast<int>(q) + 32);
        }
        for (size_t i = 0; i < kQ6Block; i += 4) {
            const uint32_t word = packed[i] | packed[i + 1] << 6 | packed[i + 2] << 12 | packed[i + 3] << 18;
            out.push_back(static_cast<uint8_t>(word & 0xFF));
            out.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
        }
    }
    return out;
}

std::vector<float> DequantizeQ6(const uint8_t* bytes, size_t count) {
    std::vector<float> result(count, 0.0f);
    size_t position = 0;
    for (size_t block = 0; block < count / kQ6Block; ++block) {
        const float scale = HalfToFloat(static_cast<uint16_t>(bytes[position] | bytes[position + 1] << 8));
        position += 2;
        for (size_t group = 0; group < kQ6Block / 4; ++group) {
            const uint32_t word = static_cast<uint32_t>(bytes[position]) |
                                  static_cast<uint32_t>(bytes[position + 1]) << 8 |
                                  static_cast<uint32_t>(bytes[position + 2]) << 16;
            position += 3;
            for (size_t k = 0; k < 4; ++k) {
                const int q = static_cast<int>((word >> (6 * k)) & 0x3F) - 32;
                result[block * kQ6Block + group * 4 + k] = static_cast<float>(q) * scale;
            }
        }
    }
    return result;
}

} // namespace TypoWeightConversion

// ---- 訂正の位置 ----

TypoCorrection::Edit TypoCorrection::GetEdit() const {
    const std::u32string& original = reading;
    const std::u32string& fixed = corrected;
    size_t prefix = 0;
    while (prefix < original.size() && prefix < fixed.size() && original[prefix] == fixed[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < original.size() - prefix && suffix < fixed.size() - prefix &&
           original[original.size() - 1 - suffix] == fixed[fixed.size() - 1 - suffix]) {
        ++suffix;
    }
    return Edit{prefix, original.size() - suffix, fixed.substr(prefix, fixed.size() - suffix - prefix)};
}

bool TypoCorrection::IsTrailingInsertionOnly() const {
    const Edit edit = GetEdit();
    return edit.start == edit.end && edit.start == reading.size() && !edit.replacement.empty();
}

std::optional<std::u32string> TypoCorrection::CorrectedSegment(size_t start, size_t end) const {
    if (end > reading.size() || start >= end) return std::nullopt;
    const Edit edit = GetEdit();
    if (edit.start < start || edit.end > end) return std::nullopt;
    const std::u32string result =
        reading.substr(start, edit.start - start) + edit.replacement + reading.substr(edit.end, end - edit.end);
    if (result == reading.substr(start, end - start)) return std::nullopt;
    return result;
}

std::optional<TypoCorrection::Placement> TypoCorrection::PlacementIn(const std::vector<std::u32string>& readings) const {
    std::u32string joined;
    for (const auto& segment : readings) joined += segment;
    if (joined != reading) return std::nullopt;
    size_t start = 0;
    for (size_t index = 0; index < readings.size(); ++index) {
        const size_t end = start + readings[index].size();
        if (auto fixed = CorrectedSegment(start, end)) return Placement{index, std::move(*fixed)};
        start = end;
    }
    return std::nullopt;
}

std::optional<size_t> TypoCorrection::SegmentOffset(const std::vector<std::u32string>& readings) const {
    for (size_t offset = 0; offset < readings.size(); ++offset) {
        std::u32string tail;
        for (size_t i = offset; i < readings.size(); ++i) tail += readings[i];
        if (tail == reading) return offset;
    }
    // 文節が無く読みも空のとき（macOS版と同じく 0）
    if (readings.empty() && reading.empty()) return size_t(0);
    return std::nullopt;
}

std::optional<TypoCorrection::Placement> TypoCorrection::PlacementWithFixedPrefix(
    const std::vector<std::u32string>& readings) const {
    for (size_t offset = 0; offset < readings.size(); ++offset) {
        const std::vector<std::u32string> tail(readings.begin() + static_cast<std::ptrdiff_t>(offset), readings.end());
        std::u32string joined;
        for (const auto& segment : tail) joined += segment;
        if (joined != reading) continue;
        const auto placement = PlacementIn(tail);
        if (!placement) return std::nullopt;
        return Placement{placement->index + offset, placement->correctedReading};
    }
    return std::nullopt;
}

std::optional<TypoCorrectionFeedback> TypoCorrectionFeedback::Make(const TypoCorrection& correction, size_t context,
                                                                   size_t maxChanged) {
    const TypoCorrection::Edit edit = correction.GetEdit();
    if (edit.start == edit.end && edit.replacement.empty()) return std::nullopt;
    const std::u32string& characters = correction.reading;
    std::u32string prefix = characters.substr(0, edit.start);
    if (prefix.size() > context) prefix = U"…" + prefix.substr(prefix.size() - context);
    std::u32string suffix = characters.substr(edit.end);
    if (suffix.size() > context) suffix = suffix.substr(0, context) + U"…";
    const auto abbreviate = [maxChanged](const std::u32string& text) {
        return text.size() > maxChanged ? text.substr(0, maxChanged) + U"…" : text;
    };
    return TypoCorrectionFeedback{prefix, abbreviate(characters.substr(edit.start, edit.end - edit.start)),
                                  abbreviate(edit.replacement), suffix};
}

// ---- 数字の置き換え ----

namespace {
const std::u32string kNumberPlaceholder = U"なん";

std::vector<size_t> PlaceholderOffsets(const std::u32string& text) {
    std::vector<size_t> offsets;
    size_t index = 0;
    while (index + kNumberPlaceholder.size() <= text.size()) {
        if (text.compare(index, kNumberPlaceholder.size(), kNumberPlaceholder) == 0) {
            offsets.push_back(index);
            index += kNumberPlaceholder.size();
        } else {
            ++index;
        }
    }
    return offsets;
}
} // namespace

NumberMask::NumberMask(const std::u32string& reading) {
    const auto isLetterAt = [&reading](std::ptrdiff_t index) {
        return index >= 0 && index < static_cast<std::ptrdiff_t>(reading.size()) && IsAsciiLetter(reading[index]);
    };
    for (size_t i = 0; i < reading.size(); ++i) {
        if (IsAsciiDigit(reading[i]) && (isLetterAt(static_cast<std::ptrdiff_t>(i) - 1) ||
                                         isLetterAt(static_cast<std::ptrdiff_t>(i) + 1))) {
            digitIsTypo_ = true;
        }
    }
    if (digitIsTypo_) {
        masked_ = reading;
        return;
    }
    std::map<size_t, std::u32string> numbers; // masked の中で置き換えた「なん」が始まる位置 → 元の数字
    size_t index = 0;
    while (index < reading.size()) {
        if (!IsAsciiDigit(reading[index])) {
            masked_ += reading[index];
            ++index;
            continue;
        }
        // 数字、または数字に挟まれた「.」「,」までを 1 つの並びにする
        size_t end = index + 1;
        while (end < reading.size()) {
            if (IsAsciiDigit(reading[end])) {
                ++end;
            } else if ((reading[end] == U'.' || reading[end] == U',') && end + 1 < reading.size() &&
                       IsAsciiDigit(reading[end + 1])) {
                end += 2;
            } else {
                break;
            }
        }
        numbers[masked_.size()] = reading.substr(index, end - index);
        masked_ += kNumberPlaceholder;
        index = end;
    }
    for (size_t offset : PlaceholderOffsets(masked_)) {
        const auto found = numbers.find(offset);
        occurrences_.push_back(found == numbers.end() ? std::nullopt : std::optional<std::u32string>(found->second));
    }
}

std::optional<std::u32string> NumberMask::Restore(const std::u32string& corrected) const {
    if (std::none_of(occurrences_.begin(), occurrences_.end(), [](const auto& item) { return item.has_value(); })) {
        return corrected;
    }
    const std::vector<size_t> offsets = PlaceholderOffsets(corrected);
    if (offsets.size() != occurrences_.size()) return std::nullopt;
    std::u32string result;
    size_t position = 0;
    for (size_t i = 0; i < offsets.size(); ++i) {
        result += corrected.substr(position, offsets[i] - position);
        result += occurrences_[i] ? *occurrences_[i] : kNumberPlaceholder;
        position = offsets[i] + kNumberPlaceholder.size();
    }
    result += corrected.substr(position);
    return result;
}

// ---- モデル ----

// 語彙とモデルをまとめた実体（manifest.json / weights.bin を読む。macOS版 Runtime・TypoNormalizerModel）
class TypoNormalizer::Runtime {
public:
    static constexpr int kPad = 0;
    static constexpr int kBos = 1;
    static constexpr int kEos = 2;
    static constexpr int kUnknown = 3;

    struct Attention {
        Linear q, k, v, o;
    };
    struct FeedForward {
        Linear up, down;
    };
    struct EncoderLayer {
        Norm n1, n2;
        Attention attention;
        FeedForward ffn;
    };
    struct DecoderLayer {
        Norm n1, n2, n3;
        Attention selfAttention, crossAttention;
        FeedForward ffn;
    };
    // デコーダの KV キャッシュ。self は 1 ステップごとに 1 行足し、cross は最初の 1 回だけ作る
    struct DecoderCache {
        std::vector<Mat> selfKeys, selfValues, crossKeys, crossValues;
    };

    bool Load(const std::filesystem::path& directory, std::string* error);

    bool Supports(const std::u32string& reading) const {
        if (reading.empty() || reading.size() > kMaxReadingLength) return false;
        return std::all_of(reading.begin(), reading.end(), [this](char32_t c) { return stoi_.count(c) != 0; });
    }

    // モデルが出力できず、打ち間違いの印でもない文字（数字・ASCII の記号・ヵヶ）。ローマ字は直す対象として通す
    bool IsProtected(char32_t c) const {
        const auto found = stoi_.find(c);
        if (found == stoi_.end() || found->second < outputSize_) return false;
        return !IsAsciiLetter(c);
    }

    std::vector<int> Encode(const std::u32string& text, bool eos) const {
        std::vector<int> ids;
        for (char32_t c : text) {
            const auto found = stoi_.find(c);
            ids.push_back(found == stoi_.end() ? kUnknown : found->second);
        }
        if (eos) ids.push_back(kEos);
        return ids;
    }

    std::u32string Decode(const std::vector<int>& ids) const {
        std::u32string result;
        for (int id : ids) {
            if (id == kPad || id == kBos) continue;
            if (id == kEos) break;
            result += id < static_cast<int>(itos_.size()) ? itos_[id] : U"?";
        }
        return result;
    }

    Mat EncodeSource(const std::vector<int>& source) const;
    DecoderCache MakeCache(const Mat& memory) const;
    Mat DecodeTokens(const std::vector<int>& tokens, DecoderCache* cache, int offset) const;

    // greedy + KV キャッシュ。生成長は読みの長さ + 9 まで（学習時と同じ）
    std::optional<std::u32string> Generate(const std::vector<int>& source, const Mat& memory) const {
        if (static_cast<int>(source.size()) > maxLength_) return std::nullopt;
        DecoderCache cache = MakeCache(memory);
        const int maxNew = std::min(maxLength_ - 1, static_cast<int>(source.size()) + 8);
        std::vector<int> generated;
        int token = kBos;
        for (int step = 0; step < maxNew; ++step) {
            const Mat logits = DecodeTokens({token}, &cache, step);
            token = Argmax(logits.Row(logits.rows - 1), logits.cols);
            if (token == kEos) break;
            generated.push_back(token);
        }
        return Decode(generated);
    }

    // teacher forcing の前向き 1 回で求める（出力できない文字を含む列の確率は 0 → −∞）
    double LogProbability(const std::u32string& target, const std::vector<int>& source, const Mat& memory) const {
        std::vector<int> targetIds = {kBos};
        const std::vector<int> encoded = Encode(target, true);
        targetIds.insert(targetIds.end(), encoded.begin(), encoded.end());
        if (static_cast<int>(source.size()) > maxLength_ || static_cast<int>(targetIds.size()) - 1 > maxLength_) {
            return -std::numeric_limits<double>::infinity();
        }
        for (size_t i = 1; i < targetIds.size(); ++i) {
            if (targetIds[i] >= outputSize_) return -std::numeric_limits<double>::infinity();
        }
        DecoderCache cache = MakeCache(memory);
        const Mat logits = DecodeTokens(std::vector<int>(targetIds.begin(), targetIds.end() - 1), &cache, 0);
        double total = 0;
        for (int step = 0; step < logits.rows; ++step) {
            total += LogSoftmaxValue(logits.Row(step), logits.cols, targetIds[static_cast<size_t>(step) + 1]);
        }
        return total;
    }

    int MaxLength() const { return maxLength_; }

private:
    Mat Embed(const std::vector<int>& tokens, const float* positions, int offset) const;
    Mat AttentionBlock(const Attention& a, const Mat& query, const Mat& keys, const Mat& values,
                       std::optional<int> causalPrefix) const;
    Mat FeedForwardBlock(const FeedForward& f, const Mat& x) const;

    std::vector<float> weights_;
    int dModel_ = 0;
    int nHead_ = 0;
    int headDim_ = 0;
    int dFF_ = 0;
    int maxLength_ = 0;
    int vocabSize_ = 0;
    int outputSize_ = 0;
    float embeddingScale_ = 1;
    const float* embedding_ = nullptr;
    const float* positionSource_ = nullptr;
    const float* positionTarget_ = nullptr;
    std::vector<EncoderLayer> encoderLayers_;
    std::vector<DecoderLayer> decoderLayers_;
    Norm encoderNorm_;
    Norm decoderNorm_;
    Linear head_;
    std::vector<std::u32string> itos_;
    std::unordered_map<char32_t, int> stoi_;
};

bool TypoNormalizer::Runtime::Load(const std::filesystem::path& directory, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    std::string manifestText;
    std::string weightBytes;
    if (!ReadBinaryFile(directory / "manifest.json", &manifestText) ||
        !ReadBinaryFile(directory / "weights.bin", &weightBytes)) {
        return fail("打ち間違い訂正のモデルがありません: " + PathToUtf8(directory));
    }
    json::Value manifest;
    if (!json::Parse(manifestText, &manifest) || manifest.type != json::Value::Type::Object) {
        return fail("manifest.json を読めません");
    }
    const auto number = [](const json::Value* value, int64_t* out) {
        if (!value || value->type != json::Value::Type::Number) return false;
        *out = static_cast<int64_t>(value->number);
        return true;
    };
    const json::Value* config = manifest.Find("config");
    int64_t dModel = 0, nHead = 0, encLayers = 0, decLayers = 0, dFF = 0, maxLength = 0;
    int64_t vocabSize = 0, outputSize = 0, totalFloats = 0;
    if (!config || !number(config->Find("d_model"), &dModel) || !number(config->Find("nhead"), &nHead) ||
        !number(config->Find("enc_layers"), &encLayers) || !number(config->Find("dec_layers"), &decLayers) ||
        !number(config->Find("d_ff"), &dFF) || !number(config->Find("max_len"), &maxLength) ||
        !number(manifest.Find("vocab_size"), &vocabSize) || !number(manifest.Find("n_out"), &outputSize) ||
        !number(manifest.Find("total_floats"), &totalFloats) || nHead <= 0 || dModel % nHead != 0) {
        return fail("manifest.json の設定が足りません");
    }
    const json::Value* vocab = manifest.Find("vocab");
    const json::Value* tensors = manifest.Find("tensors");
    if (!vocab || vocab->type != json::Value::Type::Array || !tensors || tensors->type != json::Value::Type::Array) {
        return fail("manifest.json に語彙かテンソルの一覧がありません");
    }
    const std::string dtype = manifest.Find("dtype") ? manifest.Find("dtype")->AsString() : "";

    // 重みを float32 に展開する（計算は常に float32）
    struct TensorInfo {
        std::vector<int64_t> shape;
        int64_t offset = 0;
        int64_t count = 0;
    };
    std::map<std::string, TensorInfo> index;
    weights_.assign(static_cast<size_t>(totalFloats), 0.0f);
    if (dtype == "float32-le") {
        if (weightBytes.size() != static_cast<size_t>(totalFloats) * 4) return fail("weights.bin の大きさが manifest と合いません");
        std::memcpy(weights_.data(), weightBytes.data(), weightBytes.size());
    } else if (dtype == "float16-le") {
        if (weightBytes.size() != static_cast<size_t>(totalFloats) * 2) return fail("weights.bin の大きさが manifest と合いません");
        const auto* halves = reinterpret_cast<const unsigned char*>(weightBytes.data());
        for (size_t i = 0; i < weights_.size(); ++i) {
            weights_[i] = TypoWeightConversion::HalfToFloat(static_cast<uint16_t>(halves[2 * i] | halves[2 * i + 1] << 8));
        }
    } else if (dtype != "mixed") {
        return fail("対応していない dtype です: " + dtype);
    }
    for (const json::Value& tensor : tensors->array) {
        TensorInfo info;
        const std::string name = tensor.Find("name") ? tensor.Find("name")->AsString() : "";
        const json::Value* shape = tensor.Find("shape");
        if (name.empty() || !shape || shape->type != json::Value::Type::Array ||
            !number(tensor.Find("offset"), &info.offset) || !number(tensor.Find("count"), &info.count)) {
            return fail("manifest.json のテンソルの記述が足りません");
        }
        for (const json::Value& dimension : shape->array) info.shape.push_back(static_cast<int64_t>(dimension.number));
        if (info.offset < 0 || info.count < 0 || info.offset + info.count > totalFloats) {
            return fail("テンソル " + name + " が weights.bin の外を指しています");
        }
        if (dtype == "mixed") {
            const std::string format = tensor.Find("format") ? tensor.Find("format")->AsString() : "";
            int64_t byteOffset = 0, bytes = 0;
            if (!number(tensor.Find("byte_offset"), &byteOffset) || !number(tensor.Find("bytes"), &bytes) ||
                byteOffset < 0 || bytes < 0 || static_cast<size_t>(byteOffset + bytes) > weightBytes.size()) {
                return fail("テンソル " + name + " が weights.bin の外を指しています");
            }
            const auto* chunk = reinterpret_cast<const unsigned char*>(weightBytes.data()) + byteOffset;
            float* destination = weights_.data() + info.offset;
            if (format == "float16") {
                if (bytes != info.count * 2) return fail("テンソル " + name + " の大きさが合いません");
                for (int64_t i = 0; i < info.count; ++i) {
                    destination[i] = TypoWeightConversion::HalfToFloat(
                        static_cast<uint16_t>(chunk[2 * i] | chunk[2 * i + 1] << 8));
                }
            } else if (format == "q6") {
                if (static_cast<size_t>(bytes) != TypoWeightConversion::Q6Bytes(static_cast<size_t>(info.count))) {
                    return fail("テンソル " + name + " の大きさが合いません");
                }
                const std::vector<float> values = TypoWeightConversion::DequantizeQ6(chunk, static_cast<size_t>(info.count));
                std::copy(values.begin(), values.end(), destination);
            } else {
                return fail("対応していない形式です: " + format);
            }
        }
        index[name] = std::move(info);
    }

    std::string missing;
    const auto tensor = [&](const std::string& name, std::vector<int64_t> shape) -> const float* {
        const auto found = index.find(name);
        if (found == index.end() || found->second.shape != shape) {
            if (missing.empty()) missing = name;
            return nullptr;
        }
        return weights_.data() + found->second.offset;
    };
    const auto norm = [&](const std::string& prefix) {
        return Norm{tensor(prefix + ".weight", {dModel}), tensor(prefix + ".bias", {dModel}), static_cast<int>(dModel)};
    };
    const auto linear = [&](const std::string& prefix, int64_t in, int64_t out) {
        return Linear{tensor(prefix + ".weight", {out, in}), tensor(prefix + ".bias", {out}), static_cast<int>(in),
                      static_cast<int>(out)};
    };
    const auto attention = [&](const std::string& prefix) {
        return Attention{linear(prefix + ".q", dModel, dModel), linear(prefix + ".k", dModel, dModel),
                         linear(prefix + ".v", dModel, dModel), linear(prefix + ".o", dModel, dModel)};
    };
    // FFN は nn.Sequential なので重みの名前は net.0 / net.3（1 は GELU、2 は Dropout）
    const auto feedForward = [&](const std::string& prefix) {
        return FeedForward{linear(prefix + ".net.0", dModel, dFF), linear(prefix + ".net.3", dFF, dModel)};
    };
    dModel_ = static_cast<int>(dModel);
    nHead_ = static_cast<int>(nHead);
    headDim_ = static_cast<int>(dModel / nHead);
    dFF_ = static_cast<int>(dFF);
    maxLength_ = static_cast<int>(maxLength);
    vocabSize_ = static_cast<int>(vocabSize);
    outputSize_ = static_cast<int>(outputSize);
    embeddingScale_ = static_cast<float>(std::sqrt(static_cast<double>(dModel)));
    embedding_ = tensor("emb.weight", {vocabSize, dModel});
    positionSource_ = tensor("pos_src.weight", {maxLength, dModel});
    positionTarget_ = tensor("pos_tgt.weight", {maxLength, dModel});
    for (int64_t i = 0; i < encLayers; ++i) {
        const std::string prefix = "enc." + std::to_string(i);
        encoderLayers_.push_back({norm(prefix + ".n1"), norm(prefix + ".n2"), attention(prefix + ".attn"),
                                  feedForward(prefix + ".ffn")});
    }
    for (int64_t i = 0; i < decLayers; ++i) {
        const std::string prefix = "dec." + std::to_string(i);
        decoderLayers_.push_back({norm(prefix + ".n1"), norm(prefix + ".n2"), norm(prefix + ".n3"),
                                  attention(prefix + ".self_attn"), attention(prefix + ".cross_attn"),
                                  feedForward(prefix + ".ffn")});
    }
    encoderNorm_ = norm("n_enc");
    decoderNorm_ = norm("n_dec");
    head_ = linear("head", dModel, outputSize);
    if (!missing.empty()) return fail("manifest にテンソル " + missing + " が無いか、形が違います");

    // 語彙: 先頭 4 つは <pad> <s> </s> <unk>、以降は 1 文字ずつ
    for (const json::Value& item : vocab->array) itos_.push_back(Utf8ToUtf32(item.AsString()));
    for (size_t i = 4; i < itos_.size(); ++i) {
        if (itos_[i].size() == 1) stoi_[itos_[i][0]] = static_cast<int>(i);
    }
    return true;
}

Mat TypoNormalizer::Runtime::Embed(const std::vector<int>& tokens, const float* positions, int offset) const {
    Mat x(static_cast<int>(tokens.size()), dModel_);
    for (size_t i = 0; i < tokens.size(); ++i) {
        float* destination = x.Row(static_cast<int>(i));
        const float* source = embedding_ + static_cast<size_t>(tokens[i]) * dModel_;
        const float* position = positions + (static_cast<size_t>(offset) + i) * dModel_;
        for (int j = 0; j < dModel_; ++j) destination[j] = source[j] * embeddingScale_ + position[j];
    }
    return x;
}

// causalPrefix があれば因果マスク: クエリ行 i はキー行 causalPrefix + i までを見る
Mat TypoNormalizer::Runtime::AttentionBlock(const Attention& a, const Mat& query, const Mat& keys, const Mat& values,
                                            std::optional<int> causalPrefix) const {
    const Mat queries = Project(a.q, query);
    const int t = queries.rows;
    const int s = keys.rows;
    const float scale = static_cast<float>(1.0 / std::sqrt(static_cast<double>(headDim_)));
    Mat scores(t, s);
    Mat context(t, dModel_);
    for (int head = 0; head < nHead_; ++head) {
        const int shift = head * headDim_;
        GemmNT(t, s, headDim_, scale, queries.data.data() + shift, dModel_, keys.data.data() + shift, dModel_,
               scores.data.data(), s);
        if (causalPrefix && t > 1) {
            for (int i = 0; i < t; ++i) {
                float* row = scores.Row(i);
                for (int j = *causalPrefix + i + 1; j < s; ++j) row[j] = -std::numeric_limits<float>::max();
            }
        }
        SoftmaxRows(scores.data.data(), t, s, s);
        GemmNN(t, headDim_, s, scores.data.data(), s, values.data.data() + shift, dModel_,
               context.data.data() + shift, dModel_);
    }
    return Project(a.o, context);
}

Mat TypoNormalizer::Runtime::FeedForwardBlock(const FeedForward& f, const Mat& x) const {
    Mat hidden = Project(f.up, x);
    Gelu(&hidden);
    return Project(f.down, hidden);
}

// 読みの id 列（末尾に EOS）→ memory（S×d_model）。構成は pre-norm
Mat TypoNormalizer::Runtime::EncodeSource(const std::vector<int>& source) const {
    Mat x = Embed(source, positionSource_, 0);
    for (const EncoderLayer& layer : encoderLayers_) {
        const Mat h = LayerNorm(layer.n1, x);
        const Mat keys = Project(layer.attention.k, h);
        const Mat values = Project(layer.attention.v, h);
        AddInPlace(&x, AttentionBlock(layer.attention, h, keys, values, std::nullopt));
        AddInPlace(&x, FeedForwardBlock(layer.ffn, LayerNorm(layer.n2, x)));
    }
    return LayerNorm(encoderNorm_, x);
}

TypoNormalizer::Runtime::DecoderCache TypoNormalizer::Runtime::MakeCache(const Mat& memory) const {
    DecoderCache cache;
    for (const DecoderLayer& layer : decoderLayers_) {
        // cross の k/v は memory からの射影なので 1 回で終わり
        cache.crossKeys.push_back(Project(layer.crossAttention.k, memory));
        cache.crossValues.push_back(Project(layer.crossAttention.v, memory));
        cache.selfKeys.emplace_back(0, dModel_, maxLength_);
        cache.selfValues.emplace_back(0, dModel_, maxLength_);
    }
    return cache;
}

// tokens を 1 回デコードしてロジット（T×n_out）を返す。offset は位置埋め込みの添字（生成では step と同じ）
Mat TypoNormalizer::Runtime::DecodeTokens(const std::vector<int>& tokens, DecoderCache* cache, int offset) const {
    Mat x = Embed(tokens, positionTarget_, offset);
    for (size_t index = 0; index < decoderLayers_.size(); ++index) {
        const DecoderLayer& layer = decoderLayers_[index];
        const Mat h = LayerNorm(layer.n1, x);
        // self: 新しい行ぶんの k/v をキャッシュへ足してから、貯まっている全部を見る
        Mat& keyCache = cache->selfKeys[index];
        Mat& valueCache = cache->selfValues[index];
        const int prefix = keyCache.rows;
        ApplyLinear(layer.selfAttention.k, h, keyCache.AppendRows(h.rows), dModel_);
        ApplyLinear(layer.selfAttention.v, h, valueCache.AppendRows(h.rows), dModel_);
        AddInPlace(&x, AttentionBlock(layer.selfAttention, h, keyCache, valueCache, prefix));
        const Mat h2 = LayerNorm(layer.n2, x);
        AddInPlace(&x, AttentionBlock(layer.crossAttention, h2, cache->crossKeys[index], cache->crossValues[index],
                                      std::nullopt));
        AddInPlace(&x, FeedForwardBlock(layer.ffn, LayerNorm(layer.n3, x)));
    }
    return Project(head_, LayerNorm(decoderNorm_, x));
}

// ---- TypoNormalizer ----

TypoNormalizer::TypoNormalizer(std::filesystem::path directory) : directory_(std::move(directory)) {}
TypoNormalizer::~TypoNormalizer() = default;

bool TypoNormalizer::IsModelDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / "manifest.json", error) &&
           std::filesystem::is_regular_file(directory / "weights.bin", error);
}

const char* TypoNormalizer::KernelName() { return SelectedKernel().name; }

TypoNormalizer::Runtime* TypoNormalizer::LoadLocked(std::string* error) {
    if (runtime_) return runtime_.get();
    if (loadFailure_) {
        if (error) *error = *loadFailure_;
        return nullptr;
    }
    auto runtime = std::make_unique<Runtime>();
    std::string message;
    if (!runtime->Load(directory_, &message)) {
        loadFailure_ = message;
        if (error) *error = message;
        return nullptr;
    }
    runtime_ = std::move(runtime);
    return runtime_.get();
}

bool TypoNormalizer::Load(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadLocked(error) != nullptr;
}

std::vector<TypoNormalizer::ProtectedSegment> TypoNormalizer::ProtectedSegments(
    const std::u32string& reading, const std::function<bool(char32_t)>& isProtected) {
    std::vector<ProtectedSegment> segments;
    for (char32_t c : reading) {
        const bool protectedCharacter = isProtected(c);
        if (!segments.empty() && segments.back().isProtected == protectedCharacter) {
            segments.back().text += c;
        } else {
            segments.push_back({std::u32string(1, c), protectedCharacter});
        }
    }
    return segments;
}

std::optional<TypoCorrection> TypoNormalizer::Correction(const std::u32string& reading, double threshold,
                                                         size_t minimumSegmentLength, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Runtime* runtime = LoadLocked(error);
    if (!runtime || !runtime->Supports(reading)) return std::nullopt;
    const NumberMask mask(reading);
    const std::vector<ProtectedSegment> segments =
        mask.DigitIsTypo() ? std::vector<ProtectedSegment>{{reading, false}}
                           : ProtectedSegments(mask.Masked(), [runtime](char32_t c) { return runtime->IsProtected(c); });
    std::u32string corrected;
    std::optional<double> margin;
    for (const ProtectedSegment& segment : segments) {
        std::optional<std::u32string> fixed;
        double segmentMargin = 0;
        if (!segment.isProtected && segment.text.size() >= minimumSegmentLength) {
            // 同じ入力の生成と 2 つの logP は、エンコーダの結果を使い回す（値は変わらない）
            const std::vector<int> source = runtime->Encode(segment.text, true);
            if (static_cast<int>(source.size()) <= runtime->MaxLength()) {
                const Mat memory = runtime->EncodeSource(source);
                const auto generated = runtime->Generate(source, memory);
                if (generated && *generated != segment.text) {
                    segmentMargin = runtime->LogProbability(*generated, source, memory) -
                                    runtime->LogProbability(segment.text, source, memory);
                    if (segmentMargin > threshold) fixed = generated;
                }
            }
        }
        if (fixed) {
            corrected += *fixed;
            margin = margin ? std::min(*margin, segmentMargin) : segmentMargin;
        } else {
            corrected += segment.text;
        }
    }
    if (!margin) return std::nullopt;
    const auto restored = mask.Restore(corrected);
    if (!restored || *restored == reading) return std::nullopt;
    return TypoCorrection{reading, *restored, *margin};
}

std::optional<std::u32string> TypoNormalizer::Generate(const std::u32string& reading, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Runtime* runtime = LoadLocked(error);
    if (!runtime) return std::nullopt;
    const std::vector<int> source = runtime->Encode(reading, true);
    if (static_cast<int>(source.size()) > runtime->MaxLength()) return std::nullopt;
    return runtime->Generate(source, runtime->EncodeSource(source));
}

double TypoNormalizer::LogProbability(const std::u32string& target, const std::u32string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    Runtime* runtime = LoadLocked(nullptr);
    if (!runtime) return -std::numeric_limits<double>::infinity();
    const std::vector<int> sourceIds = runtime->Encode(source, true);
    if (static_cast<int>(sourceIds.size()) > runtime->MaxLength()) return -std::numeric_limits<double>::infinity();
    return runtime->LogProbability(target, sourceIds, runtime->EncodeSource(sourceIds));
}

std::vector<std::vector<float>> TypoNormalizer::Logits(const std::u32string& source, const std::u32string& target,
                                                       size_t steps) {
    std::lock_guard<std::mutex> lock(mutex_);
    Runtime* runtime = LoadLocked(nullptr);
    if (!runtime) return {};
    const std::vector<int> sourceIds = runtime->Encode(source, true);
    std::vector<int> targetIds = {Runtime::kBos};
    const std::vector<int> encoded = runtime->Encode(target, true);
    targetIds.insert(targetIds.end(), encoded.begin(), encoded.end());
    if (static_cast<int>(sourceIds.size()) > runtime->MaxLength() ||
        static_cast<int>(targetIds.size()) - 1 > runtime->MaxLength()) {
        return {};
    }
    auto cache = runtime->MakeCache(runtime->EncodeSource(sourceIds));
    const Mat logits = runtime->DecodeTokens(std::vector<int>(targetIds.begin(), targetIds.end() - 1), &cache, 0);
    std::vector<std::vector<float>> rows;
    for (int step = 0; step < std::min(static_cast<int>(steps), logits.rows); ++step) {
        rows.emplace_back(logits.Row(step), logits.Row(step) + logits.cols);
    }
    return rows;
}

bool TypoNormalizer::Supports(const std::u32string& reading) {
    std::lock_guard<std::mutex> lock(mutex_);
    Runtime* runtime = LoadLocked(nullptr);
    return runtime && runtime->Supports(reading);
}

} // namespace iroha
