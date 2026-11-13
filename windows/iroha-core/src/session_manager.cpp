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
    bool TypoCorrectionAvailable() override { return manager->backend_->TypoCorrectionAvailable(); }
    void RequestAI(uint64_t generation, size_t preset, const std::u32string& text) override {
        manager->EnqueueAI(AIJob{id, generation, preset, text});
    }
    void CancelAI() override { manager->CancelAIJobs(id); }
    void RequestTypoCorrection(uint64_t generation, const std::u32string& reading, int delayMs, double threshold,
                               size_t minimumLength) override {
        manager->EnqueueTypo(id, TypoJob{TypoJob::Kind::Typo, generation, reading, U"", threshold, minimumLength,
                                         std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs)});
    }
    void RequestAIFixCheck(uint64_t generation, const std::u32string& reading, const std::u32string& context,
                           int delayMs) override {
        manager->EnqueueTypo(id, TypoJob{TypoJob::Kind::AIFixCheck, generation, reading, context, 0, 1,
                                         std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs)});
    }
    void CancelAIFixCheck() override { manager->CancelTypo(id, TypoJob::Kind::AIFixCheck); }
    void RequestAIFix(uint64_t generation, const AIRequest& request) override {
        manager->EnqueueAI(AIJob{id, generation, 0, U"", request});
    }
    void CancelTypoCorrection() override { manager->CancelTypo(id); }
    std::optional<TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                              size_t minimumLength) override {
        return manager->backend_->CorrectTypo(reading, threshold, minimumLength);
    }
    // 「こえ」モード（stateMutex_ を持った状態で呼ばれる。知らせは裏のスレッドから届くので、渡すときに鍵を取る）
    std::u32string RequestVoiceStart(uint64_t generation) override {
        SessionManager* owner = manager;
        const uint64_t sessionId = id;
        const std::u32string error =
            manager->backend_->StartVoice([owner, sessionId, generation](const InputSession::VoiceEvent& event) {
                std::lock_guard<std::mutex> lock(owner->stateMutex_);
                const auto found = owner->sessions_.find(sessionId);
                if (found != owner->sessions_.end()) found->second->session.HandleVoiceEvent(generation, event);
            });
        if (error.empty()) manager->voiceSessionId_ = id;
        return error;
    }
    void RequestVoiceStop(uint64_t generation) override {
        (void)generation;
        if (manager->voiceSessionId_ == id) manager->backend_->StopVoice();
    }
    void CancelVoice() override {
        if (manager->voiceSessionId_ != id) return;
        manager->voiceSessionId_ = 0;
        manager->backend_->CancelVoice();
    }
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
    typoWorker_ = std::thread([this] { TypoWorkerLoop(); });
    aiWorker_ = std::thread([this] { AIWorkerLoop(); });
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
    {
        std::lock_guard<std::mutex> lock(typoMutex_);
        typoStopping_ = true;
        typoJobs_.clear();
    }
    typoCondition_.notify_all();
    if (typoWorker_.joinable()) typoWorker_.join();
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiStopping_ = true;
        aiJobs_.clear();
        if (aiCancel_) aiCancel_->Cancel();
    }
    aiCondition_.notify_all();
    if (aiWorker_.joinable()) aiWorker_.join();
}

SessionManager::Entry& SessionManager::EntryFor(uint64_t sessionId) {
    auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) {
        while (sessions_.size() >= kMaxSessions) {
            auto oldest = std::min_element(
                sessions_.begin(), sessions_.end(),
                [](const auto& a, const auto& b) { return a.second->lastUsed < b.second->lastUsed; });
            CancelJobs(oldest->first);
            CancelTypo(oldest->first);
            CancelTypo(oldest->first, TypoJob::Kind::AIFixCheck);
            CancelAIJobs(oldest->first);
            if (voiceSessionId_ == oldest->first) {
                voiceSessionId_ = 0;
                backend_->CancelVoice();
            }
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
    if (output) *output = found->second->session.TakePolledOutput();
    return true;
}

void SessionManager::End(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    CancelJobs(sessionId);
    CancelTypo(sessionId);
    CancelTypo(sessionId, TypoJob::Kind::AIFixCheck);
    CancelAIJobs(sessionId);
    if (voiceSessionId_ == sessionId) {
        voiceSessionId_ = 0;
        backend_->CancelVoice();
    }
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

bool SessionManager::AnyComposing() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (const auto& item : sessions_) {
        if (item.second->session.IsComposing()) return true;
    }
    return false;
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

void SessionManager::EnqueueTypo(uint64_t sessionId, TypoJob job) {
    {
        std::lock_guard<std::mutex> lock(typoMutex_);
        const auto key = std::make_pair(sessionId, job.kind);
        typoJobs_[key] = std::move(job);
    }
    typoCondition_.notify_one();
}

void SessionManager::CancelTypo(uint64_t sessionId, TypoJob::Kind kind) {
    std::lock_guard<std::mutex> lock(typoMutex_);
    typoJobs_.erase(std::make_pair(sessionId, kind));
}

// 休止の時間が来た依頼から順に訂正する。訂正の最中は stateMutex_ を持たない
// （結果を渡すときだけ取る。Space の訂正がモデルを待っている間に、この結果が届いても詰まらない）
void SessionManager::TypoWorkerLoop() {
    for (;;) {
        uint64_t sessionId = 0;
        TypoJob job;
        {
            std::unique_lock<std::mutex> lock(typoMutex_);
            for (;;) {
                if (typoStopping_) return;
                if (typoJobs_.empty()) {
                    typoCondition_.wait(lock);
                    continue;
                }
                auto next = std::min_element(typoJobs_.begin(), typoJobs_.end(),
                                             [](const auto& a, const auto& b) { return a.second.due < b.second.due; });
                if (next->second.due <= std::chrono::steady_clock::now()) {
                    sessionId = next->first.first;
                    job = std::move(next->second);
                    typoJobs_.erase(next);
                    break;
                }
                typoCondition_.wait_until(lock, next->second.due);
            }
        }
        if (job.kind == TypoJob::Kind::AIFixCheck) {
            // モデル単体でもう一度変換する。進行中のライブ変換は止めずに、終わるのを待つ
            // （止めると、表示と比べるライブ変換の結果が届かなくなる）
            Backend::ScoredText scored;
            {
                std::lock_guard<std::mutex> engine(engineMutex_);
                scored = backend_->ScoreConversion(job.reading, job.context);
            }
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto found = sessions_.find(sessionId);
            if (found != sessions_.end()) {
                found->second->session.CompleteAIFixCheck(job.generation, job.reading, scored.text, scored.margins);
            }
            continue;
        }
        const auto correction = backend_->CorrectTypo(job.reading, job.threshold, job.minimumLength);
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto found = sessions_.find(sessionId);
        if (found != sessions_.end()) {
            found->second->session.CompleteTypoCorrection(job.generation, job.reading, correction);
        }
    }
}

void SessionManager::EnqueueAI(AIJob job) {
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        aiJobs_.erase(std::remove_if(aiJobs_.begin(), aiJobs_.end(),
                                     [&job](const AIJob& queued) { return queued.sessionId == job.sessionId; }),
                      aiJobs_.end());
        aiJobs_.push_back(std::move(job));
    }
    aiCondition_.notify_one();
}

void SessionManager::CancelAIJobs(uint64_t sessionId) {
    std::lock_guard<std::mutex> lock(aiMutex_);
    aiJobs_.erase(std::remove_if(aiJobs_.begin(), aiJobs_.end(),
                                 [sessionId](const AIJob& queued) { return queued.sessionId == sessionId; }),
                  aiJobs_.end());
    if (aiRunning_ && aiRunning_->sessionId == sessionId && aiCancel_) aiCancel_->Cancel();
}

// AI変換を 1 件ずつ行う。途中経過と結果を渡すときだけ stateMutex_ を取る
void SessionManager::AIWorkerLoop() {
    for (;;) {
        AIJob job;
        std::shared_ptr<CancelToken> cancel;
        {
            std::unique_lock<std::mutex> lock(aiMutex_);
            aiCondition_.wait(lock, [this] { return aiStopping_ || !aiJobs_.empty(); });
            if (aiStopping_) return;
            job = aiJobs_.front();
            aiJobs_.erase(aiJobs_.begin());
            aiRunning_ = job;
            cancel = std::make_shared<CancelToken>();
            aiCancel_ = cancel;
        }
        if (job.fix) {
            // AIで直す: 途中経過は出さない。10 秒何も届かなければ打ち切る（macOS版の aiFixTimeout）
            const AIOutcome outcome = backend_->RunAIRequest(*job.fix, 10, cancel.get());
            {
                std::lock_guard<std::mutex> lock(aiMutex_);
                aiRunning_.reset();
                aiCancel_.reset();
            }
            if (cancel->Cancelled()) continue;
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto found = sessions_.find(job.sessionId);
            if (found != sessions_.end()) found->second->session.CompleteAIFix(job.generation, outcome);
            continue;
        }
        const auto onPartial = [this, &job](const std::u32string& partial) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto found = sessions_.find(job.sessionId);
            if (found != sessions_.end()) found->second->session.UpdateAIPartial(job.generation, partial);
        };
        const auto output = backend_->RunAI(job.preset, job.text, onPartial, cancel.get());
        {
            std::lock_guard<std::mutex> lock(aiMutex_);
            aiRunning_.reset();
            aiCancel_.reset();
        }
        if (cancel->Cancelled()) continue;
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto found = sessions_.find(job.sessionId);
        if (found != sessions_.end()) found->second->session.CompleteAI(job.generation, output);
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
