// SPDX-License-Identifier: GPL-3.0-only

#include "quick/quick_replaygain.hpp"

namespace trackknife::quick {

QuickReplayGain::QuickReplayGain(bench::ReplayGainJob* job, QObject* parent)
    : QObject(parent), job_(job) {
    job_->setParent(this);
    connect(job_, &bench::ReplayGainJob::changed, this, &QuickReplayGain::changed);
}

QVariantMap QuickReplayGain::state() const {
    const auto& job = *job_;
    return {{QStringLiteral("grouping"), job.grouping()},
            {QStringLiteral("expression"), job.expression()},
            {QStringLiteral("sidecarOnly"), job.sidecarOnly()},
            {QStringLiteral("truePeak"), job.truePeak()},
            {QStringLiteral("groups"), job.groups()},
            {QStringLiteral("status"), job.status()},
            {QStringLiteral("problems"), job.problems().join(QStringLiteral("\n"))},
            {QStringLiteral("running"), job.running()},
            {QStringLiteral("canStop"), job.canStop()},
            {QStringLiteral("progress"), job.progressValue()},
            {QStringLiteral("progressMaximum"), job.progressMaximum()}};
}

} // namespace trackknife::quick
