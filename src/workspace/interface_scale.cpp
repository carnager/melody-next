// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/interface_scale.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QSettings>

#include <cmath>
#include <string_view>

namespace trackknife::bench {

QList<double> interfaceScales() { return {1.0, 0.75, 0.9, 1.1, 1.25, 1.5, 1.75, 2.0}; }

double chosenInterfaceScale() {
    // The application's own settings file: as it names itself once it
    // exists, and before then as the window will name it.
    const bool named =
        QCoreApplication::instance() != nullptr && !QCoreApplication::organizationName().isEmpty();
    QSettings settings{QSettings::defaultFormat(), QSettings::UserScope,
                       named ? QCoreApplication::organizationName() : QStringLiteral("trackknife"),
                       named ? QCoreApplication::applicationName() : QStringLiteral("trackknife")};
    bool ok = false;
    const auto value = settings.value(QLatin1String{interface_scale_key}, 1.0).toDouble(&ok);
    for (const auto offered : interfaceScales()) {
        if (ok && std::abs(value - offered) < 0.001) {
            return offered;
        }
    }
    return 1.0;
}

void applyInterfaceScale(const int argc, char** argv) {
    // Screenshots are of the window as designed, at the test's own scale.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--screenshot" || argument == "--grab") {
            return;
        }
    }
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        return;
    }
    const auto scale = chosenInterfaceScale();
    if (scale != 1.0) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(scale));
    }
}

} // namespace trackknife::bench
