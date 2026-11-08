#pragma once

#include <QObject>
#include <memory>

#include "preferences/usersettings.h"

namespace mixxx {

// Pins Mixxx threads to CPU cores according to the [ThreadPin] group in
// mixxx.cfg.
//
// Keys are thread names as shown by `ps -Lo tid,comm` (Qt sets a thread's
// OS name from its QThread objectName; the main thread appears as "Main").
// Matching is case-insensitive and ignores spaces, dashes and underscores,
// so "SoundIODevice", "sound_io_device" and "soundiodevice" are the same
// key. The keys "main" and "default" have fixed meanings; "default" applies
// to every thread that has no key of its own.
//
// Values are CPU lists such as "0", "0-3" or "0,2-4". The value "all"
// imposes no restriction. With no [ThreadPin] configuration at all every
// thread stays free to run on every core, which is the default.
//
//   [ThreadPin]
//   default=0-3
//   Engine=4
//   SoundIODevice=5
//   pool=6-7
//
// A periodic pass scans /proc/self/task, matches each thread by name and
// applies the resulting affinity with sched_setaffinity(). Threads created
// later in the session (analyzers, library jobs, general thread pool
// workers) are picked up without touching their creation sites. Every pin
// state change is logged at info level.
//
// Linux only; on other platforms this logs once and does nothing.
class ThreadPin : public QObject {
    Q_OBJECT
  public:
    explicit ThreadPin(UserSettingsPointer pConfig,
            QObject* parent = nullptr);
    ~ThreadPin() override;

    // Reads the configuration, applies it to the threads that already
    // exist and logs their pin state, then begins a periodic scan that
    // follows threads as they come and go. With nothing configured this
    // logs a single info line and does nothing further.
    void start();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
};

} // namespace mixxx
