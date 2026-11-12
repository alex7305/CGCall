/**
 * @file mainwindow.cpp
 * @brief source file that implements the JS8Call user interface
 *   executes member functions of the UI_Constructor class that provide
 *   all functionality of the JS8call main window
 */

#include "mainwindow.h"

#include "moc_mainwindow.cpp"
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

static char message[29];
static char msgsent[29];
static int msgibits;

// How many milliseconds to wait before releasing PTT at end of transmission.
constexpr int TX_SWITCHOFF_DELAY = 200;

int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols
struct dec_data dec_data;            // for sharing with Fortran
struct specData specData;            // Used by plotter
std::mutex fftw_mutex;

namespace {
int ms_minute_error() {
    auto const now = DriftingDateTime::currentDateTimeLocal();
    auto const time = now.time();
    auto const second = time.second();

    return now.msecsTo(now.addSecs(second > 30 ? 60 - second : -second)) -
           time.msec();
}

namespace State {
constexpr QStringView Ready = u"Ready";
constexpr QStringView Send = u"TX";
constexpr QStringView Sending = u"Sending";
constexpr QStringView Tuning = u"TX";

QString timed(QStringView const state, int const delay) {
    auto time = std::div(delay, 60);

    if (time.quot && time.rem)
        return QString("%1 (%2m %3s)").arg(state).arg(time.quot).arg(time.rem);
    else if (time.quot)
        return QString("%1 (%2m)").arg(state).arg(time.quot);
    else
        return QString("%1 (%2s)").arg(state).arg(time.rem);
}
} // namespace State

int roundUp(int numToRound, int multiple) {
    if (multiple == 0) {
        return numToRound;
    }

    int roundDown = (numToRound / multiple) * multiple;
    return roundDown + multiple;
}

// Copy at most size bytes into the array, filling any unused size
// with spaces if less than size bytes were available to copy. For
// convenience, return a one past the end iterator, i.e., equal to
// array + size.

auto copyByteData(QByteArrayView const bytes, char *const array,
                  qsizetype const size) {
    return std::fill_n(
        std::copy_n(bytes.begin(), std::min(size, bytes.size()), array),
        size - bytes.size(), ' ');
}

// Copy at most size bytes into the array, padding out the message
// with spaces if less than size bytes were available to copy, and
// null-terminate it. Caller is responsible for ensuring that at
// least (size + 1) bytes of space are available.

void copyMessage(QStringView const string, char *const array,
                 qsizetype const size = 28) {
    *copyByteData(string.toLocal8Bit(), array, size) = '\0';
}

} // namespace

void UI_Constructor::checkStartupWarnings() {
    //if (m_config.check_for_updates()) {
    //    checkVersion(false);
    //}
    ensureCallsignSet(false);
}
