// TEMP diagnostics (2026-11-16): host thread naming, all-thread RIP sampler (MCLA_TEMP_SAMPLER_ALL=1)
// and container growth reporter (MCLA_TEMP_GROWTH=1). Both are opt-in; naming is always on (cheap).
#pragma once
#include <functional>
#include <string>
namespace mcla::diag {
void NameThisThread(const char* name);                 // SetThreadDescription on the calling thread
void RegisterGrowth(const char* name, std::function<std::string()> fn);  // fn returns "n=... mb=..."
void StartDiagThreads();                               // idempotent; starts sampler/growth if env set
}
