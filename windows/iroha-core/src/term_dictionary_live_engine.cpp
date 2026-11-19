#include "iroha/term_dictionary_live_engine.h"

#include "iroha/user_dictionary_engine.h"

namespace iroha {

TermDictionaryLiveEngine::TermDictionaryLiveEngine(ConversionEngine* base, LookupProvider lookup,
                                                   LatticeConverter* lattice)
    : base_(base), lookup_(std::move(lookup)), lattice_(lattice) {}

bool TermDictionaryLiveEngine::Convert(const std::u32string& reading, const std::u32string& context,
                                       int candidateCount, std::vector<std::u32string>* results,
                                       std::string* error) {
    const std::shared_ptr<const TermDictionaryLookup> lookup = lookup_ ? lookup_() : nullptr;
    UserDictionary dictionary = lookup ? lookup->LiveDictionary(reading) : UserDictionary();
    if (dictionary.IsEmpty()) return base_->Convert(reading, context, candidateCount, results, error);
    UserDictionaryEngine engine(base_, [&dictionary] { return dictionary; }, lattice_);
    return engine.Convert(reading, context, candidateCount, results, error);
}

} // namespace iroha
