/**
 * @file DecodedText.cpp
 * @brief Implementation of DecodedText class
 */

#include "DecodedText.h"
#include "JS8_Include/commons.h"
#include <JS8_Main/Varicode.h>

#include <QStringBuilder>

/******************************************************************************/
// Constants
/******************************************************************************/

namespace {
// Quality level below which we'll consider a decode to be suspect;
// the UI will generally enclose the decode within [] characters to
// denote it as being sketchy.

constexpr auto QUALITY_THRESHOLD = 0.17f;
} // namespace

/******************************************************************************/
// Local Routines
/******************************************************************************/

namespace {
// Translation of standard submode IDs to their character equivalents.
// this is only used when writing out to ALL.TXT, so we've defined it
// here, but arguably it should be part of JS8::Submode or Varicode.

QChar submodeChar(int const submode) {
    switch (submode) {
    case Varicode::SubmodeType::JS8CallNormal:
        return 'A';
    case Varicode::SubmodeType::JS8CallFast:
        return 'B';
    case Varicode::SubmodeType::JS8CallTurbo:
        return 'C';
    case Varicode::SubmodeType::JS8CallSlow:
        return 'E';
    case Varicode::SubmodeType::JS8CallUltra:
        return 'I';
    default:
        return '~';
    }
}

// Create and return a potentially compound call from the provided
// parts; the parts are at this point guaranteed to be at least of
// size 2, but any part might be empty.

// Format signed tenths of an arc-minute as DD°MM.m'H (or DDD for longitude).
QString formatDmm(int const tenths, int const degWidth, QChar const pos,
                  QChar const neg) {
    int const mag = qAbs(tenths);
    int const deg = mag / 600;
    int const minTenths = mag % 600;

    return QStringLiteral("%1%2%3.%4'%5")
        .arg(deg, degWidth, 10, QChar('0'))
        .arg(QChar(0x00B0))
        .arg(minTenths / 10, 2, 10, QChar('0'))
        .arg(minTenths % 10)
        .arg(tenths < 0 ? neg : pos);
}
} // namespace

/******************************************************************************/
// Private Implementation
/******************************************************************************/

// Core constructor, called by the two public constructors.
// Attempts to unpack, using the unpack strategies defined
// in the order of the unpack strategies array, until one
// of them works or all of them have failed.

/**
 * @private
 * @brief Construct a new Decoded Text:: Decoded Text object (private)
 *
 * @param frame
 * @param bits
 * @param submode
 * @param isLowConfidence
 * @param time
 * @param frequencyOffset
 * @param snr
 * @param dt
 */
DecodedText::DecodedText(QString const &frame, int bits, int submode,
                         bool isLowConfidence, int time, int frequencyOffset,
                         float snr, float dt)
    : frameType_(Varicode::FrameUnknown), frame_(frame), isAlt_(false),
      isHeartbeat_(false), isLowConfidence_(isLowConfidence), message_(frame_),
      bits_(bits), submode_(submode), time_(time),
      frequencyOffset_(frequencyOffset), snr_(snr), dt_(dt) {
    auto const m = message().trimmed();

    if (m.length() < 12 || m.contains(' '))
        return;

    for (auto unpack : unpackStrategies) {
        if ((this->*unpack)(m))
            break;
    }
}

/**
 * @private
 * @brief Try to unpack fast data message (private)
 *
 * @param m
 * @return true
 * @return false
 */
bool DecodedText::tryUnpackFastData(QString const &m) {
    if ((bits_ & Varicode::JS8CallData) != Varicode::JS8CallData)
        return false;

    if (auto const data = Varicode::unpackFastDataMessage(m); data.isEmpty()) {
        return false;
    } else {
        message_ = data;
        frameType_ = Varicode::FrameData;

        return true;
    }
}

/**
 * @private
 * @brief Try to unpack data message (private)
 *
 * @param m
 * @return true
 * @return false
 */
bool DecodedText::tryUnpackData(QString const &m) {
    if ((bits_ & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    if (auto const data = Varicode::unpackDataMessage(m); data.isEmpty()) {
        return false;
    } else {
        message_ = data;
        frameType_ = Varicode::FrameData;

        return true;
    }
}

/**
 * @private
 * @brief Try to unpack heartbeat message (private)
 *
 * @param m
 * @return true
 * @return false
 */
/**
 * @private
 * @brief Try to unpack OPS message (private)
 */
bool DecodedText::tryUnpackOps(QString const &m) {
    if ((bits_ & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    quint32 id = 0;
    int lat = 0, lon = 0, course = -1, speed = 0;

    if (!Varicode::unpackOpsFrame(m, &id, &lat, &lon, &course, &speed))
        return false;

    frameType_ = Varicode::FrameOps;

    message_ = QStringLiteral("ID %1 | %2 %3 | CRS %4 | SPD %5 kt ")
                   .arg(id, 5, 10, QChar('0'))
                   .arg(formatDmm(lat, 2, QChar('N'), QChar('S')))
                   .arg(formatDmm(lon, 3, QChar('E'), QChar('W')))
                   .arg(course < 0 ? QStringLiteral("---")
                                   : QStringLiteral("%1").arg(course, 3, 10,
                                                              QChar('0')))
                   .arg(speed);

    return true;
}

/******************************************************************************/
// Public Implementation
/******************************************************************************/

// Main constructor, used to interpret Decoded events emitted by the JS8
// decoder. This function used to be handled via parsing strings issued by
// the Fortran decoder.
//
// Of note here is the quality check; that was present in the previous code,
// but did not seem to be looking in the right place for the annotation that
// the Fortran decoded emitted.

/**
 * @brief Construct a new Decoded Text:: Decoded Text object
 *
 * @param decoded
 */
DecodedText::DecodedText(JS8::Event::Decoded const &decoded)
    : DecodedText(QString::fromStdString(decoded.data), decoded.type,
                  decoded.mode, decoded.quality < QUALITY_THRESHOLD,
                  decoded.utc, decoded.frequency, decoded.snr, decoded.xdt) {}

// Constructor used internally; we're basically taking advantage of the ability
// of this class to unpack, and as such this probably doesn't belong here, but
// keeping it aligned with the previous code for now.

/**
 * @brief Construct a new Decoded Text:: Decoded Text object
 *
 * @param frame
 * @param bits
 * @param submode
 */
DecodedText::DecodedText(QString const &frame, int const bits,
                         int const submode)
    : DecodedText(frame, bits, submode, false, 0, 0, 0.0f, 0.0f) {}

// Simple word split for free text messages; preallocate memory for
// efficiency; add whole message as item 0 to mimic regular expression
// capture list.

/**
 * @brief Get the message words
 *
 * @return QStringList
 */
QStringList DecodedText::messageWords() const {
    QStringList words;

    words.reserve(message_.count(' ') + 2);
    words.append(message_);
    words.append(message_.split(' ', Qt::SkipEmptyParts));

    return words;
}

// Format as a string suitable for appending to ALL.TXT. Original
// code has no space between time and SNR; matching that here.

/**
 * @brief Get the string representation suitable for ALL.TXT
 *
 * @return QString
 */
QString DecodedText::string() const {
    struct hour_minute_second hms = decode_time(time_);

    return QStringLiteral("%1:%2:%3%4 %5 %6 %7  %8         %9   ")
        .arg(hms.hour, 2, 10, QChar('0'))   // Right-aligned integer with 2
                                            // characters, padded with zeroes.
        .arg(hms.minute, 2, 10, QChar('0')) // Right-aligned integer with 2
                                            // characters, padded with zeroes.
        .arg(hms.second, 2, 10, QChar('0')) // Right-aligned integer with 2
                                            // characters, padded with zeroes.
        .arg(snr_, 3, 10, QChar(' '))       // Right-aligned integer with 3
                                            // characters, padded with spaces
        .arg(dt_, 4, 'f', 1) // Right-aligned float with 1 decimal point
        .arg(frequencyOffset_, 4, 10,
             QChar(' ')) // Right-aligned integer with 4 characters, passed with
                         // spaces
        .arg(submodeChar(submode_)) // Single character
        .arg(frame_)                // Fixed string, 12 characters
        .arg(bits_);                // Single 3-bit integer
}

/******************************************************************************/
