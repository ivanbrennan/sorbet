#include "main/cache/cache.h"
#include "common/kvstore/KeyValueStore.h"
#include "main/options/options.h"
#include "main/pipeline/pipeline.h"
#include "payload/payload.h"
#include "sorbet_version/sorbet_version.h"

using namespace std;

namespace sorbet::realmain::cache {
unique_ptr<OwnedKeyValueStore> maybeCreateKeyValueStore(shared_ptr<::spdlog::logger> logger,
                                                        const options::Options &opts) {
    if (opts.cacheDir.empty()) {
        return nullptr;
    }
    // Despite being called "experimental," this feature is actually stable. We just didn't want to
    // bust all existing caches when we promoted the experimental-at-the-time incremental fast path
    // to the stable version.
    auto flavor = "experimentalfastpath";
    return make_unique<OwnedKeyValueStore>(make_unique<KeyValueStore>(logger, sorbet_full_version_string, opts.cacheDir,
                                                                      move(flavor), opts.maxCacheSizeBytes));
}

unique_ptr<OwnedKeyValueStore> ownIfUnchanged(const core::GlobalState &gs, unique_ptr<KeyValueStore> kvstore) {
    if (kvstore == nullptr) {
        return nullptr;
    }

    auto ownedKvstore = make_unique<OwnedKeyValueStore>(move(kvstore));
    if (payload::kvstoreUnchangedSinceGsCreation(gs, ownedKvstore)) {
        return ownedKvstore;
    }

    // Some other process has written to kvstore; don't use.
    return nullptr;
}

unique_ptr<KeyValueStore> maybeCacheGlobalStateAndFiles(unique_ptr<KeyValueStore> kvstore, const options::Options &opts,
                                                        core::GlobalState &gs, WorkerPool &workers,
                                                        const vector<ast::ParsedFile> &indexed) {
    if (kvstore == nullptr) {
        return kvstore;
    }
    auto ownedKvstore = make_unique<OwnedKeyValueStore>(move(kvstore));
    // TODO: Move these methods into this file.
    payload::retainGlobalState(gs, opts, ownedKvstore);
    pipeline::cacheTreesAndFiles(gs, workers, indexed, ownedKvstore);
    auto sizeBytes = ownedKvstore->cacheSize();
    kvstore = OwnedKeyValueStore::bestEffortCommit(gs.tracer(), move(ownedKvstore));
    prodCounterInc("cache.committed");

    size_t usedPercent = round((sizeBytes * 100.0) / opts.maxCacheSizeBytes);
    prodCounterSet("cache.used_bytes", sizeBytes);
    prodCounterSet("cache.used_percent", usedPercent);
    gs.tracer().debug("sorbet_version={} cache_used_bytes={} cache_used_percent={}", sorbet_full_version_string,
                      sizeBytes, usedPercent);

    return kvstore;
}

} // namespace sorbet::realmain::cache
