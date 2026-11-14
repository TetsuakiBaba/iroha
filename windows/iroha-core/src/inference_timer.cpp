#include "iroha/inference_timer.h"

namespace iroha::inference_timer {

namespace {
thread_local InferenceStats* g_current = nullptr;
}

InferenceStats* Current() { return g_current; }

Scope::Scope(InferenceStats* stats) : previous_(g_current) { g_current = stats; }

Scope::~Scope() { g_current = previous_; }

Measure::Measure(Kind kind) : kind_(kind), stats_(g_current) {
    if (stats_) start_ = std::chrono::steady_clock::now();
}

Measure::~Measure() {
    if (!stats_) return;
    const int64_t micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count();
    if (kind_ == Kind::NeuralNetwork) {
        stats_->neuralNetworkMicros += micros;
        ++stats_->neuralNetworkCalls;
    } else {
        stats_->modelLoadMicros += micros;
    }
}

void Note(InferenceStats::Shortcut shortcut) {
    if (g_current) g_current->shortcuts |= shortcut;
}

} // namespace iroha::inference_timer
