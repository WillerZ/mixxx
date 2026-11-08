#include "util/threadpin.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <algorithm>

#include "moc_threadpin.cpp"
#include "preferences/configobject.h"

#ifdef Q_OS_LINUX
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {
constexpr char kThreadPinGroup[] = "[ThreadPin]";
constexpr int kScanIntervalMs = 1000;
} // namespace

namespace mixxx {

struct ThreadPin::Impl {
#ifdef Q_OS_LINUX
    struct Mask {
        bool unrestricted = true; // "all": leave the kernel's freedom alone
        QVector<int> cpus;        // sorted, unique; empty when unrestricted
        QString display;          // e.g. "0-3,5" or "all cores"
    };

    QHash<QString, Mask> masks; // normalized thread name -> mask
    Mask defaultMask;
    cpu_set_t allCores{}; // this process's starting affinity = "everywhere"
    int mainTid = 0;

    QHash<int, QString> lastPinByTid; // tid -> display, last logged
    QSet<int> warnedTids;
    bool firstScanDone = false;

    std::unique_ptr<QTimer> pTimer;

    static QString normalizeName(const QString& name) {
        QString out;
        out.reserve(name.size());
        for (const QChar c : name) {
            if (c.isLetterOrNumber()) {
                out.append(c.toLower());
            }
        }
        // Friendly alias for the general thread pool, whose worker name
        // "Thread (pooled)" is awkward to write as an INI key.
        if (out == QLatin1String("pool")) {
            out = QStringLiteral("threadpooled");
        }
        return out;
    }

    static QString describeCpus(const QVector<int>& cpus) {
        QString out;
        int i = 0;
        while (i < cpus.size()) {
            int j = i;
            while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) {
                j++;
            }
            if (!out.isEmpty()) {
                out += QLatin1Char(',');
            }
            if (j == i) {
                out += QString::number(cpus[i]);
            } else {
                out += QStringLiteral("%1-%2").arg(cpus[i]).arg(cpus[j]);
            }
            i = j + 1;
        }
        return out;
    }

    static bool parseCpuList(const QString& value, Mask* pMask, QString* pError) {
        QString spec = value.trimmed();
        // Be forgiving about hand edits like "key = value" that leave a
        // stray '=' glued to the value by the INI key tokenizer.
        if (spec.startsWith(QLatin1Char('='))) {
            spec = spec.mid(1).trimmed();
        }
        const QString lowered = spec.toLower();
        if (lowered == QLatin1String("all") || lowered == QLatin1String("*") ||
                lowered == QLatin1String("everywhere")) {
            pMask->unrestricted = true;
            pMask->cpus.clear();
            pMask->display = QStringLiteral("all cores");
            return true;
        }

        QVector<int> cpus;
        static const QRegularExpression tokenRe(
                QStringLiteral("^\\s*(\\d+)(?:\\s*-\\s*(\\d+))?\\s*$"));
        const QStringList tokens = spec.split(QLatin1Char(','));
        for (const QString& token : tokens) {
            const QRegularExpressionMatch match = tokenRe.match(token);
            if (!match.hasMatch()) {
                *pError = QStringLiteral("invalid CPU list \"%1\"").arg(spec);
                return false;
            }
            bool ok = false;
            const int first = match.captured(1).toInt(&ok);
            if (!ok || first < 0 || first >= CPU_SETSIZE) {
                *pError = QStringLiteral("CPU number out of range in \"%1\"").arg(spec);
                return false;
            }
            int last = first;
            if (!match.captured(2).isEmpty()) {
                last = match.captured(2).toInt();
                if (last < first || last >= CPU_SETSIZE) {
                    *pError = QStringLiteral("CPU range out of order or too large in \"%1\"")
                                      .arg(spec);
                    return false;
                }
            }
            for (int cpu = first; cpu <= last; cpu++) {
                cpus.append(cpu);
            }
        }
        std::sort(cpus.begin(), cpus.end());
        QVector<int> unique;
        unique.reserve(cpus.size());
        int prev = -1;
        for (const int cpu : cpus) {
            if (cpu != prev) {
                unique.append(cpu);
                prev = cpu;
            }
        }
        pMask->cpus = unique;
        pMask->unrestricted = false;
        pMask->display = describeCpus(unique);
        return true;
    }

    void loadConfig(UserSettingsPointer pConfig) {
        const QList<ConfigKey> keys =
                pConfig->getKeysWithGroup(QString::fromLatin1(kThreadPinGroup));
        for (const ConfigKey& key : keys) {
            Mask mask;
            QString error;
            // mixxx.cfg stores only the first whitespace-free token as the
            // key and the rest of the line as the value. An ini style line
            // like "Engine=4,7" therefore ends up entirely in the key; split
            // it here so both "Engine 4,7" and "Engine = 4,7" styles work.
            QString item = key.item;
            QString value = pConfig->getValue<QString>(key);
            const int eq = item.indexOf(QLatin1Char('='));
            if (eq != -1) {
                if (value.isEmpty()) {
                    value = item.mid(eq + 1);
                }
                item = item.left(eq);
            }
            if (value.startsWith(QLatin1Char('='))) {
                value = value.mid(1);
            }
            value = value.trimmed();
            if (!parseCpuList(value, &mask, &error)) {
                qWarning() << "ThreadPin: ignoring key"
                           << QStringLiteral("%1,%2").arg(key.group, item)
                           << "-" << error;
                continue;
            }
            const QString name = normalizeName(item);
            if (name == QLatin1String("default")) {
                defaultMask = mask;
            } else {
                masks.insert(name, mask);
            }
        }
    }

    const Mask& maskForName(const QString& normalizedName) const {
        auto it = masks.constFind(normalizedName);
        return it != masks.constEnd() ? it.value() : defaultMask;
    }

    void toCpuSet(const Mask& mask, cpu_set_t* pSet) const {
        if (mask.unrestricted) {
            *pSet = allCores;
            return;
        }
        CPU_ZERO(pSet);
        for (const int cpu : mask.cpus) {
            CPU_SET(cpu, pSet);
        }
    }

    void logState(int tid, const QString& name, const QString& display) {
        qInfo() << "ThreadPin:" << name << QStringLiteral("(tid %1)").arg(tid)
                << "->" << display;
        lastPinByTid.insert(tid, display);
    }

    void scan() {
        const QDir taskDir(QStringLiteral("/proc/self/task"));
        const QFileInfoList entries = taskDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QSet<int> liveTids;

        for (const QFileInfo& entry : entries) {
            bool ok = false;
            const int tid = entry.fileName().toInt(&ok);
            if (!ok) {
                continue;
            }
            liveTids.insert(tid);

            QString name;
            if (tid == mainTid) {
                // The main thread keeps the program name in comm; refer to
                // it by its QThread objectName instead, like the logs do.
                name = QStringLiteral("Main");
            } else {
                QFile commFile(entry.filePath() + QStringLiteral("/comm"));
                if (!commFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    continue; // thread is gone
                }
                name = QString::fromUtf8(commFile.readAll()).trimmed();
                if (name.isEmpty()) {
                    continue;
                }
            }

            const Mask& mask = maskForName(normalizeName(name));
            cpu_set_t desired;
            toCpuSet(mask, &desired);

            cpu_set_t current;
            CPU_ZERO(&current);
            if (sched_getaffinity(tid, sizeof(current), &current) != 0) {
                continue; // thread is gone
            }

            if (memcmp(&current, &desired, sizeof(current)) != 0) {
                if (sched_setaffinity(tid, sizeof(desired), &desired) != 0) {
                    if (!warnedTids.contains(tid)) {
                        warnedTids.insert(tid);
                        qWarning() << "ThreadPin: failed to pin thread" << name
                                   << QStringLiteral("(tid %1) to %2: %3")
                                              .arg(tid)
                                              .arg(mask.display,
                                                      QString::fromLatin1(
                                                              strerror(errno)));
                    }
                    continue;
                }
                logState(tid, name, mask.display);
            } else if (!firstScanDone) {
                // Record the starting pin state of every thread once.
                logState(tid, name, mask.display);
            }
        }

        // Threads that exited: forget them so recycled tids start fresh.
        for (auto it = lastPinByTid.begin(); it != lastPinByTid.end();) {
            if (!liveTids.contains(it.key())) {
                it = lastPinByTid.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = warnedTids.begin(); it != warnedTids.end();) {
            if (!liveTids.contains(*it)) {
                it = warnedTids.erase(it);
            } else {
                ++it;
            }
        }
        firstScanDone = true;
    }
#endif // Q_OS_LINUX
};

ThreadPin::ThreadPin(UserSettingsPointer pConfig, QObject* parent)
        : QObject(parent),
          m_pImpl(std::make_unique<Impl>()) {
#ifdef Q_OS_LINUX
    sched_getaffinity(0, sizeof(m_pImpl->allCores), &m_pImpl->allCores);
    m_pImpl->mainTid = static_cast<int>(getpid());
    m_pImpl->loadConfig(pConfig);

    const bool anyPinning = !m_pImpl->masks.isEmpty() ||
            !m_pImpl->defaultMask.unrestricted;
    if (anyPinning) {
        m_pImpl->pTimer = std::make_unique<QTimer>(this);
        connect(m_pImpl->pTimer.get(), &QTimer::timeout, this, [this]() {
            m_pImpl->scan();
        });
    }
#else
    Q_UNUSED(pConfig);
#endif
}

ThreadPin::~ThreadPin() = default;

void ThreadPin::start() {
#ifdef Q_OS_LINUX
    if (!m_pImpl->pTimer) {
        qInfo() << "ThreadPin: no [ThreadPin] pinning configured;"
                   " every thread may run on any core";
        return;
    }
    qInfo() << "ThreadPin: applying [ThreadPin] configuration";
    // Apply immediately so early threads are pinned without waiting for the
    // first timer tick, then keep watching for threads that appear later.
    m_pImpl->scan();
    m_pImpl->pTimer->start(kScanIntervalMs);
#else
    qInfo() << "ThreadPin: CPU core pinning is not supported on this platform";
#endif
}

} // namespace mixxx
