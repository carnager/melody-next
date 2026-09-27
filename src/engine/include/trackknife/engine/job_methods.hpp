// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <filesystem>
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
// Takes a LocalCatalogue, not the interface: scanning is a local act by
// definition -- an engine scans the files it can see, and a client asking a
// remote engine to scan is submitting a job to that engine, not proxying one.
void register_catalogue_jobs(JobCatalog& jobs, LocalCatalogue& catalogue);

// ADR-0237: file work in the engine.
//
// loudness.scan measures what the ReplayGain tools ask for -- {items,
// options} as file_work_wire encodes them -- on the engine's own bounded
// pool, reporting each finished item, and finishes with {result} or {error}.
// It reads files and writes nothing.
//
// metadata.apply writes a reviewed plan -- {plan} -- journaled in the
// engine's database (`database`), each written file refreshed in its library
// in the same commit, reporting each file; it finishes with {result} or
// {error}. A plan with blocking issues is refused at submit: what was
// previewed must be clean. Files outside the library are written too.
//
// artwork.apply {plan} writes a reviewed artwork plan the same way: embedded
// pictures and folder images, journaled and refreshed. Its images are ones
// the engine holds -- its own files, or ones staged (artwork.stage).
void register_file_work_jobs(JobCatalog& jobs, std::filesystem::path database,
                             LocalCatalogue& catalogue);

} // namespace trackknife::engine
