// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace trackknife::engine {

// What a client may ask to be run as a job. A factory validates the submitted
// parameters and returns the work, so a bad request fails at submit time with
// a useful error rather than starting a job that immediately gives up.
class JobCatalog final {
  public:
    using Factory = std::function<core::Result<JobRegistry::Work>(const protocol::Json& params)>;

    void on(std::string name, Factory factory);
    [[nodiscard]] bool knows(std::string_view name) const;
    [[nodiscard]] core::Result<JobRegistry::Work> build(std::string_view name,
                                                        const protocol::Json& params) const;

  private:
    std::map<std::string, Factory, std::less<>> factories_;
};

// Binds job.submit and job.cancel. The registry and catalogue must outlive the
// dispatcher.
void register_job_methods(protocol::Dispatcher& dispatcher, JobRegistry& registry,
                          const JobCatalog& catalogue);

// Registers catalogue.scan, the first real job: long, mutating, cancellable
// and progress-reporting.
void register_catalogue_jobs(JobCatalog& jobs, Catalogue& catalogue);

} // namespace trackknife::engine
