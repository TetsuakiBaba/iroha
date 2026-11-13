#include "iroha/session_manager.h"

#include <algorithm>
#include <utility>

namespace iroha {

// エンジンを取る。待っている間は進行中のライブ変換をやめさせる
class SessionManager::EngineLock {
public:
    explicit EngineLock(SessionManager* manager) : manager_(manager) {
        ++manager_->engineWaiters_;
        manager_->engineMutex_.lock();
        --manager_->engineWaiters_;
    }
    ~EngineLock() { manager_->engineMutex_.unlock(); }
    EngineLock(const EngineLock&) = delete;
    EngineLock& operator=(const EngineLock&) = delete;

private:
    SessionManager* manager_;
};

// セッション 1 つと、そのセッションから見た変換の呼び出し口
struct SessionManager::Entry : InputSession::Host {
    Entry(SessionManager* manager, uint64_t id) : manager(manager), id(id), session(this) {}

    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override {
        EngineLock lock(manager);
        return manager->backend_->Convert(reading, context, candidateCount, results, error);
    }
    void RequestLiveConversion(uint64_t generation, const std::u32string& reading,
                               const std::u32string& context) override {
        manager->Enqueue(id, generation, reading, context);
    }
    void CancelLiveConversion() override { manager->CancelJobs(id); }
    void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                      const std::u32string& baseline, const std::u32string& committed) override {
        EngineLock lock(manager);
        manager->backend_->RecordCommit(reading, engineBaseline, baseline, committed);
    }
    std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                               const std::u32string& shown) override {
        EngineLock lock(manager);
        return manager->backend_->EngineResult(reading, shown);
    }
    void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                        const std::u32string& shown) override {
        EngineLock lock(manager);
        manager->backend_->ForgetLearning(reading, engineResult, shown);
    }
    std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) override {
        return manager->backend_->CandidateOnlyWords(reading);
    }
    std::vector<InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) override {
        return manager->backend_->PredictionCandidates(prefix);
    }
    void RecordInputHistory(const std::u32string& reading, const std::u32string& text) override {
        manager->backend_->RecordInputHistory(reading, text);
    }
    void RecordConversion(const ConversionLogEntry& entry) override { manager->backend_->RecordConversion(entry); }
    // プライベートモードは、ほかのセッションにも効かせて設定に保存する（stateMutex_ を持った状態で呼ばれる）
    void PrivateModeChanged(bool enabled) override {
        manager->settings_.privateMode = enabled;
        for (auto& item : manager->sessions_) {
            if (item.first != id) item.second->session.SetSettings(manager->settings_);
        }
        manager->backend_->PrivateModeChanged(enabled);
    }
    // 句読点の切り替えは、ほかのセッションにも効かせて設定に保存する（stateMutex_ を持った状態で呼ばれる）
    void PunctuationChanged(const std::u32string& punctuation) override {
        manager->settings_.punctuation = punctuation;
        for (auto& item : manager->sessions_) {
            if (item.first != id) item.second->session.SetSettings(manager->settings_);
        }
        manager->backend_->PunctuationChanged(punctuation);
    }

    SessionManager* manager;
    uint64_t id;
    InputSession session;
    uint64_t lastUsed = 0;
};

SessionManager::SessionManager(Backend* backend) : backend_(backend) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

SessionManager::~SessionManager() {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        stopping_ = true;
        pendingJob_.reset();
    }
    stoppingFlag_ = true;
    jobCondition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

SessionManager::Entry& SessionManager::EntryFor(uint64_t sessionId) {
    auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) {
        while (sessions_.size() >= kMaxSessions) {
            auto oldest = std::min_element(
                sessions_.begin(), sessions_.end(),
                [](const auto& a, const auto& b) { return a.second->lastUsed < b.second->lastUsed; });
            CancelJobs(oldest->first);
            oldest->second->session.Flush();
            sessions_.erase(oldest);
        }
        auto entry = std::make_unique<Entry>(this, sessionId);
        entry->session.SetSettings(settings_);
        entry->session.SetSymbolTable(symbolTable_);
        entry->session.SetTermLookup(termLookup_);
        entry->session.SetRewriteRules(rewriteRules_);
        found = sessions_.emplace(sessionId, std::move(entry)).first;
    }
    found->second->lastUsed = ++useCounter_;
    return *found->second;
}

SessionOutput SessionManager::HandleKey(uint64_t sessionId, SessionKey key, char32_t character,
                                        bool shift, const std::u32string* leftContext) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return EntryFor(sessionId).session.Handle(key, character, shift, leftContext);
}

bool SessionManager::Poll(uint64_t sessionId, uint64_t revision, SessionOutput* output) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto found = sessions_.find(sessionId);
    // 知らないセッション（変換サーバが起動し直した）は「変わっていない」と答える。
    // 空の表示を返すと、TIP が表示中の未確定文字列を消してしまう
    if (found == sessions_.end()) return false;
    found->second->lastUsed = ++useCounter_;
    if (found->second->session.Revision() == revision) return false;
    if (output) *output = found->second->session.Current();
    return true;
}

void SessionManager::End(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    CancelJobs(sessionId);
    const auto found = sessions_.find(sessionId);
    if (found != sessions_.end()) {
        found->second->session.Flush();
        sessions_.erase(found);
    }
}

void SessionManager::WithEngine(const std::function<void()>& work) {
    EngineLock lock(this);
    work();
}

void SessionManager::SetSettings(const SessionSettings& settings) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    settings_ = settings;
    for (auto& item : sessions_) item.second->session.SetSettings(settings_);
}

void SessionManager::SetLiveConversionEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    settings_.liveConversion = enabled;
    for (auto& item : sessions_) item.second->session.SetSettings(settings_);
}

void SessionManager::SetSymbolTable(std::shared_ptr<const SymbolCandidateTable> table) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    symbolTable_ = std::move(table);
    for (auto& item : sessions_) item.second->session.SetSymbolTable(symbolTable_);
}

void SessionManager::SetTermLookup(std::shared_ptr<const TermDictionaryLookup> lookup) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    termLookup_ = std::move(lookup);
    for (auto& item : sessions_) item.second->session.SetTermLookup(termLookup_);
}

void SessionManager::SetRewriteRules(std::shared_ptr<const UserRewriteRuleSet> rules) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    rewriteRules_ = std::move(rules);
    for (auto& item : sessions_) item.second->session.SetRewriteRules(rewriteRules_);
}

size_t SessionManager::SessionCount() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return sessions_.size();
}

// stateMutex_ を持った状態で呼ばれる（InputSession::Handle の中から）
void SessionManager::Enqueue(uint64_t sessionId, uint64_t generation,
                             const std::u32string& reading, const std::u32string& context) {
    std::optional<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        // 待たせてあった別のセッションの依頼は行われなくなる
        if (pendingJob_ && pendingJob_->sessionId != sessionId) dropped = std::move(pendingJob_);
        pendingJob_ = Job{++jobSerial_, sessionId, generation, reading, context};
    }
    jobCondition_.notify_one();
    if (dropped) {
        // 待ちを解いておく（そのセッションは次の入力でまた頼む）
        const auto found = sessions_.find(dropped->sessionId);
        if (found != sessions_.end()) {
            found->second->session.CompleteLiveConversion(dropped->generation, dropped->reading,
                                                          nullptr);
        }
    }
}

// stateMutex_ を持った状態で呼ばれる
void SessionManager::CancelJobs(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (pendingJob_ && pendingJob_->sessionId == sessionId) pendingJob_.reset();
    if (runningSerial_ != 0 && runningSessionId_ == sessionId) {
        abortedSerial_ = std::max(abortedSerial_.load(), runningSerial_);
    }
}

void SessionManager::WorkerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            jobCondition_.wait(lock, [this] { return stopping_ || pendingJob_.has_value(); });
            if (stopping_) return;
            job = std::move(*pendingJob_);
            pendingJob_.reset();
            runningSerial_ = job.serial;
            runningSessionId_ = job.sessionId;
        }

        std::vector<std::u32string> results;
        bool converted = false;
        {
            std::lock_guard<std::mutex> engine(engineMutex_);
            // やめるのは、キーの処理がエンジンを待っているとき・依頼が要らなくなったとき・終了するとき
            bool cancelled = false;
            const auto shouldCancel = [this, serial = job.serial, &cancelled] {
                if (engineWaiters_.load() > 0 || abortedSerial_.load() >= serial ||
                    stoppingFlag_.load()) {
                    cancelled = true;
                }
                return cancelled;
            };
            if (!shouldCancel()) {
                std::string error;
                backend_->SetCancelCheck(shouldCancel);
                converted = backend_->Convert(job.reading, job.context, 1, &results, &error);
                backend_->SetCancelCheck(nullptr);
                // 途中でやめた変換は、エンジンの外側の層が代わりの結果で成功にしていることがある
                if (cancelled) converted = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            runningSerial_ = 0;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto found = sessions_.find(job.sessionId);
            if (found != sessions_.end()) {
                found->second->session.CompleteLiveConversion(
                    job.generation, job.reading,
                    converted && !results.empty() ? &results.front() : nullptr);
            }
        }
    }
}

} // namespace iroha
