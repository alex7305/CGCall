/**
 * @file Varicode.cpp
 * @brief implementation of encoder
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 *
 */

#include "Varicode.h"
#include "JS8_JSC/JSC.h"
#include "JS8_Mode/DecodedText.h"

#include <QLoggingCategory>
#include <QMap>
#include <QSet>
#include <boost/format.hpp>
#include <vendor/CRCpp/CRC.h>

#include <cmath>
#include <stdexcept>

#define CRCPP_INCLUDE_ESOTERIC_CRC_DEFINITIONS
#define CRCPP_USE_CPP11

Q_DECLARE_LOGGING_CATEGORY(varicode_js8)

const int nalphabet = 41;
QString alphabet = {
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"}; // alphabet to encode _into_
                                                  // for FT8 freetext
                                                  // transmission
QString alphabet72 = {
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?."};
QString grid_pattern = {
    R"((?<grid>[A-X]{2}[0-9]{2}(?:[A-X]{2}(?:[0-9]{2})?)*)+)"};
QString orig_compound_callsign_pattern = {
    R"((?<callsign>(\d|[A-Z])+\/?((\d|[A-Z]){2,})(\/(\d|[A-Z])+)?(\/(\d|[A-Z])+)?))"};
QString base_callsign_pattern = {
    R"((?<callsign>\b(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<portable>[/][P])?\b))"};
// QString compound_callsign_pattern =
// {R"((?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?)\b)"};
QString compound_callsign_pattern = {
    R"((?<callsign>(?:[@]?|\b)(?<extended>[A-Z0-9\/@][A-Z0-9\/]{0,2}[\/]?[A-Z0-9\/]{0,3}[\/]?[A-Z0-9\/]{0,3})\b))"};
QString pack_callsign_pattern = {
    R"(([0-9A-Z ])([0-9A-Z])([0-9])([A-Z ])([A-Z ])([A-Z ]))"};
QString alphanumeric = {
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"}; // callsign and grid alphabet

QMap<QString, int> directed_cmds = {
    // any changes here need to be made also in the directed regular xpression
    // for parsing
    // ?*^&@
    {" HEARTBEAT", -1}, // this is my heartbeat (unused except for faux
                        // processing of HBs as directed commands)
    {" HB", -1}, // this is my heartbeat (unused except for faux processing of
                 // HBs as directed commands)
    {" CQ", -1}, // this is my cq (unused except for faux processing of CQs as
                 // directed commands)

    {" SNR?", 0}, // query snr
    {"?", 0},     // compat

    {" DIT DIT", 1}, // two bits

    {" HEARING?", 3}, // query station calls heard

    {" GRID?", 4}, // query grid

    {">", 5}, // relay message

    {" STATUS?", 6}, // query idle message

    {" STATUS", 7}, // this is my status

    {" HEARING", 8}, // these are the stations i'm hearing

    {" MSG", 9}, // this is a complete message

    {" MSG TO:", 10}, // store message at a station

    {" QUERY", 11}, // generic query

    {" QUERY MSGS", 12},  // do you have any stored messages?
    {" QUERY MSGS?", 12}, // do you have any stored messages?

    {" QUERY CALL", 13}, // can you transmit a ping to callsign?

    // {" ",           14  }, // reserved

    {" GRID", 15}, // this is my current grid locator

    {" INFO?", 16}, // what is your info message?
    {" INFO", 17},  // this is my info message

    {" FB", 18},      // fine business
    {" HW CPY?", 19}, // how do you copy?
    {" SK", 20},      // end of contact
    {" RR", 21},      // roger roger

    {" QSL?", 22}, // do you copy?
    {" QSL", 23},  // i copy

    {" CMD", 24}, // command

    {" SNR", 25}, // seen a station at the provided snr
    {" NO", 26},  // negative confirm
    {" YES", 27}, // confirm
    {" 73", 28},  // best regards, end of contact

    {" NACK", 2}, // negative acknowledge
    {" ACK", 14}, // acknowledge (was reserved in 2.1)

    {" HEARTBEAT SNR", 29}, // (was ACK in 2.1, now deprecated)

    {" AGN?", 30}, // repeat message
    {"  ", 31},    // send freetext (weird artifact)
    {" ", 31},     // send freetext
};

// commands allowed to be processed
QSet<int> allowed_cmds = {-1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                          10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                          21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

// commands that result in an autoreply (which can be relayed)
QSet<int> autoreply_cmds = {0, 2, 3, 4, 6, 9, 10, 11, 12, 13, 14, 16, 30};

// commands that should be buffered
QSet<int> buffered_cmds = {5, 9, 10, 11, 12, 13, 15, 24};

// commands that may include an SNR value
QSet<int> snr_cmds = {25, 29};

// commands that are checksummed and their crc size
QMap<int, int> checksum_cmds = {{5, 16},  {9, 16},  {10, 16}, {11, 16},
                                {12, 16}, {13, 16}, {15, 0},  {24, 16}};

QString callsign_pattern = QString("(?<callsign>[@]?[A-Z0-9/]+)");
QString optional_cmd_pattern =
    QString("(?<cmd>\\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG "
            "TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY "
            "MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY "
            "MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT "
            "SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?");
QString optional_grid_pattern = QString("(?<grid>\\s?[A-R]{2}[0-9]{2})?");
QString optional_extended_grid_pattern =
    QString("^(?<grid>\\s?(?:[A-R]{2}[0-9]{2}(?:[A-X]{2}(?:[0-9]{2})?)*))?");
QString optional_num_pattern =
    QString("(?<num>(?<=SNR)\\s?[-+]?(?:3[01]|[0-2]?[0-9]))?");

QRegularExpression directed_re("^" + callsign_pattern + optional_cmd_pattern +
                               optional_num_pattern);

QRegularExpression heartbeat_re(
    R"(^\s*(?<callsign>[@](?:ALLCALL|HB)\s+)?(?<type>CQ CQ CQ|CQ DX|CQ QRP|CQ CONTEST|CQ FIELD|CQ FD|CQ CQ|CQ|HB|HEARTBEAT(?!\s+SNR))(?:\s(?<grid>[A-R]{2}[0-9]{2}))?\b)");

QRegularExpression
    compound_re("^\\s*[`]" + callsign_pattern + "(?<extra>" +
                optional_grid_pattern + // there's a reason this is first (see:
                                        // buildMessageFrames)
                optional_cmd_pattern + optional_num_pattern + ")");

QMap<QString, QString> hufftable = {
    // char   code                 weight
    {" ", "01"},       // 1.0
    {"E", "100"},      // 0.5
    {"T", "1101"},     // 0.333333333333
    {"A", "0011"},     // 0.25
    {"O", "11111"},    // 0.2
    {"I", "11100"},    // 0.166666666667
    {"N", "10111"},    // 0.142857142857
    {"S", "10100"},    // 0.125
    {"H", "00011"},    // 0.111111111111
    {"R", "00000"},    // 0.1
    {"D", "111011"},   // 0.0909090909091
    {"L", "110011"},   // 0.0833333333333
    {"C", "110001"},   // 0.0769230769231
    {"U", "101101"},   // 0.0714285714286
    {"M", "101011"},   // 0.0666666666667
    {"W", "001011"},   // 0.0625
    {"F", "001001"},   // 0.0588235294118
    {"G", "000101"},   // 0.0555555555556
    {"Y", "000011"},   // 0.0526315789474
    {"P", "1111011"},  // 0.05
    {"B", "1111001"},  // 0.047619047619
    {".", "1110100"},  // 0.0434782608696
    {"V", "1100101"},  // 0.0416666666667
    {"K", "1100100"},  // 0.04
    {"-", "1100001"},  // 0.0384615384615
    {"+", "1100000"},  // 0.037037037037
    {"?", "1011001"},  // 0.0344827586207
    {"!", "1011000"},  // 0.0333333333333
    {"\"", "1010101"}, // 0.0322580645161
    {"X", "1010100"},  // 0.03125
    {"0", "0010101"},  // 0.030303030303
    {"J", "0010100"},  // 0.0294117647059
    {"1", "0010001"},  // 0.0285714285714
    {"Q", "0010000"},  // 0.0277777777778
    {"2", "0001001"},  // 0.027027027027
    {"Z", "0001000"},  // 0.0263157894737
    {"3", "0000101"},  // 0.025641025641
    {"5", "0000100"},  // 0.025
    {"4", "11110101"}, // 0.0243902439024
    {"9", "11110100"}, // 0.0238095238095
    {"8", "11110001"}, // 0.0232558139535
    {"6", "11110000"}, // 0.0227272727273
    {"7", "11101011"}, // 0.0222222222222
    {"/", "11101010"}, // 0.0217391304348
};

QChar ESC = '\\';   // Escape char
QChar EOT = '\x04'; // EOT char

quint32 nbasecall = 37 * 36 * 10 * 27 * 27 * 27;
quint16 nbasegrid = 180 * 180;
quint16 nusergrid = nbasegrid + 10;
quint16 nmaxgrid = (1 << 15) - 1;

QMap<QString, quint32> basecalls = {
    {"<....>", nbasecall + 1},   // incomplete callsign
    {"@ALLCALL", nbasecall + 2}, // ALLCALL group
    {"@JS8NET", nbasecall + 3},  // JS8NET group

    // continental dx
    {"@DX/NA", nbasecall + 4},  // North America DX group
    {"@DX/SA", nbasecall + 5},  // South America DX group
    {"@DX/EU", nbasecall + 6},  // Europe DX group
    {"@DX/AS", nbasecall + 7},  // Asia DX group
    {"@DX/AF", nbasecall + 8},  // Africa DX group
    {"@DX/OC", nbasecall + 9},  // Oceania DX group
    {"@DX/AN", nbasecall + 10}, // Antarctica DX group

    // itu regions
    {"@REGION/1", nbasecall + 11}, // ITU Region 1
    {"@REGION/2", nbasecall + 12}, // ITU Region 2
    {"@REGION/3", nbasecall + 13}, // ITU Region 3

    // generic
    {"@GROUP/0", nbasecall + 14}, // Generic group
    {"@GROUP/1", nbasecall + 15}, // Generic group
    {"@GROUP/2", nbasecall + 16}, // Generic group
    {"@GROUP/3", nbasecall + 17}, // Generic group
    {"@GROUP/4", nbasecall + 18}, // Generic group
    {"@GROUP/5", nbasecall + 19}, // Generic group
    {"@GROUP/6", nbasecall + 20}, // Generic group
    {"@GROUP/7", nbasecall + 21}, // Generic group
    {"@GROUP/8", nbasecall + 22}, // Generic group
    {"@GROUP/9", nbasecall + 23}, // Generic group

    // ops
    {"@COMMAND", nbasecall + 24}, // Command group
    {"@CONTROL", nbasecall + 25}, // Control group
    {"@NET", nbasecall + 26},     // Net group
    {"@NTS", nbasecall + 27},     // NTS group

    // reserved groups
    {"@RESERVE/0", nbasecall + 28}, // Reserved
    {"@RESERVE/1", nbasecall + 29}, // Reserved
    {"@RESERVE/2", nbasecall + 30}, // Reserved
    {"@RESERVE/3", nbasecall + 31}, // Reserved
    {"@RESERVE/4", nbasecall + 32}, // Reserved

    // special groups
    {"@APRSIS", nbasecall + 33},   // APRS GROUP
    {"@RAGCHEW", nbasecall + 34},  // RAGCHEW GROUP
    {"@JS8", nbasecall + 35},      // JS8 GROUP
    {"@EMCOMM", nbasecall + 36},   // EMCOMM GROUP
    {"@ARES", nbasecall + 37},     // ARES GROUP
    {"@MARS", nbasecall + 38},     // MARS GROUP
    {"@AMRRON", nbasecall + 39},   // AMRRON GROUP
    {"@RACES", nbasecall + 40},    // RACES GROUP
    {"@RAYNET", nbasecall + 41},   // RAYNET GROUP
    {"@RADAR", nbasecall + 42},    // RADAR GROUP
    {"@SKYWARN", nbasecall + 43},  // SKYWARN GROUP
    {"@CQ", nbasecall + 44},       // CQ GROUP
    {"@HB", nbasecall + 45},       // HB GROUP
    {"@QSO", nbasecall + 46},      // QSO GROUP
    {"@QSOPARTY", nbasecall + 47}, // QSO PARTY GROUP
    {"@CONTEST", nbasecall + 48},  // CONTEST GROUP
    {"@FIELDDAY", nbasecall + 49}, // FIELD DAY GROUP
    {"@SOTA", nbasecall + 50},     // SOTA GROUP
    {"@IOTA", nbasecall + 51},     // IOTA GROUP
    {"@POTA", nbasecall + 52},     // POTA GROUP
    {"@QRP", nbasecall + 53},      // QRP GROUP
    {"@QRO", nbasecall + 54},      // QRO GROUP
};

QMap<quint32, QString> cqs = {
    {0, "CQ CQ CQ"}, {1, "CQ DX"}, {2, "CQ QRP"}, {3, "CQ CONTEST"},
    {4, "CQ FIELD"}, {5, "CQ FD"}, {6, "CQ CQ"},  {7, "CQ"},
};

// status flags in HB messages are deprecated as of 2.2, later versions will
// likely repurpose these flags keep in mind if you change any of these to not
// start with HB you'll have to address the packHeartbeatMessage and how the
// function computes the isAlt flag.
QMap<quint32, QString> hbs = {
    {0, "HB"}, // HB
    {1, "HB"}, // HB AUTO
    {2, "HB"}, // HB AUTO RELAY
    {3, "HB"}, // HB AUTO RELAY SPOT
    {4, "HB"}, // HB      RELAY
    {5, "HB"}, // HB      RELAY SPOT
    {6, "HB"}, // HB            SPOT
    {7, "HB"}, // HB AUTO       SPOT
};

QMap<int, int> dbm2mw = {
    {0, 1},        //   1mW
    {3, 2},        //   2mW
    {7, 5},        //   5mW
    {10, 10},      //  10mW
    {13, 20},      //  20mW
    {17, 50},      //  50mW
    {20, 100},     // 100mW
    {23, 200},     // 200mW
    {27, 500},     // 500mW
    {30, 1000},    //    1W
    {33, 2000},    //    2W
    {37, 5000},    //    5W
    {40, 10000},   //   10W
    {43, 20000},   //   20W
    {47, 50000},   //   50W
    {50, 100000},  //  100W
    {53, 200000},  //  200W
    {57, 500000},  //  500W
    {60, 1000000}, // 1000W
};

/*
 * UTILITIES
 */

int mwattsToDbm(int mwatts) {
    int dbm = 0;
    auto values = dbm2mw.values();
    std::sort(values.begin(), values.end());
    foreach (auto mw, values) {
        if (mw < mwatts) {
            continue;
        }
        dbm = dbm2mw.key(mw);
        break;
    }

    return dbm;
}

int dbmTomwatts(int dbm) {
    if (dbm2mw.contains(dbm)) {
        return dbm2mw[dbm];
    }
    auto iter = dbm2mw.lowerBound(dbm);
    if (iter == dbm2mw.end()) {
        return dbm2mw.last();
    }
    return iter.value();
}

QString Varicode::extendedChars() {
    static QString c;
    if (c.size() == 0) {
        for (quint32 i = 0; i < JSC::prefixSize; i++) {
            if (JSC::prefix[i].size != 1) {
                continue;
            }
            c.append(QLatin1String(JSC::prefix[i].str, 1));
        }
    }
    return c;
}

QString Varicode::escape(const QString &text) {
    static const int size = 6;
    QString escaped;
    escaped.reserve(size * text.size());
    for (QString::const_iterator it = text.begin(); it != text.end(); ++it) {
        QChar ch = *it;
        ushort code = ch.unicode();
        if (code < 0x80) {
            escaped += ch;
        } else {
#if JS8_USE_ESCAPE_SUB_CHAR
            // escaped += "\x1A"; // substitute char
#else
            escaped += "\\U"; // "U+"; // substitute char
#endif
            escaped += QString::number(code, 16).rightJustified(4, '0');
        }
    }

    return escaped;
}

QString Varicode::unescape(const QString &text) {
    QString unescaped(text);
#if JS8_USE_ESCAPE_SUB_CHAR
    static const int size = 5;
    QRegularExpression r("([\\x1A][0-9a-fA-F]{4})");
#else
    static const int size = 6;
    QRegularExpression r("(([uU][+]|\\\\[uU])[0-9a-fA-F]{4})");
#endif
    qsizetype pos = 0;
    QRegularExpressionMatch match;
    while ((pos = unescaped.indexOf(r, pos, &match)) != -1) {
        unescaped.replace(pos++, size,
                          QChar(match.captured(1).right(4).toUShort(0, 16)));
    }

    return unescaped;
}

QString Varicode::rstrip(const QString &str) {
    qsizetype n = str.size() - 1;
    for (; n >= 0; --n) {
        if (str.at(n).isSpace()) {
            continue;
        }
        return str.left(n + 1);
    }
    return "";
}

QString Varicode::lstrip(const QString &str) {
    qsizetype len = str.size();
    for (int n = 0; n < len; n++) {
        if (str.at(n).isSpace()) {
            continue;
        }
        return str.mid(n);
    }
    return "";
}

/*
 * VARICODE
 */
QMap<QString, QString> Varicode::defaultHuffTable() { return hufftable; }

QString Varicode::cqString(int number) {
    if (!cqs.contains(number)) {
        return QString{};
    }
    return cqs[number];
}

QString Varicode::hbString(int number) {
    if (!hbs.contains(number)) {
        return QString{};
    }
    return hbs[number];
}

bool Varicode::startsWithCQ(QString text) {
    foreach (auto cq, cqs.values()) {
        if (text.startsWith(cq)) {
            return true;
        }
    }
    return false;
}

bool Varicode::startsWithHB(QString text) {
    foreach (auto hb, hbs.values()) {
        if (text.startsWith(hb)) {
            return true;
        }
    }
    return false;
}

QString Varicode::formatSNR(int snr) {
    if (snr < -60 || snr > 60) {
        return QString();
    }

    return QString("%1%2")
        .arg(snr >= 0 ? "+" : "")
        .arg(snr, snr < 0 ? 3 : 2, 10, QChar('0'));
}

QString Varicode::checksum16(QString const &input) {
    auto fromBytes = input.toLocal8Bit();
    auto crc = CRC::Calculate(fromBytes.data(), fromBytes.length(),
                              CRC::CRC_16_KERMIT());
    auto checksum = Varicode::pack16bits(crc);
    if (checksum.length() < 3) {
        checksum += QString(" ").repeated(3 - checksum.length());
    }
    return checksum;
}

bool Varicode::checksum16Valid(QString const &checksum, QString const &input) {
    auto fromBytes = input.toLocal8Bit();
    auto crc = CRC::Calculate(fromBytes.data(), fromBytes.length(),
                              CRC::CRC_16_KERMIT());
    return Varicode::pack16bits(crc) == checksum;
}

QString Varicode::checksum32(QString const &input) {
    auto fromBytes = input.toLocal8Bit();
    auto crc = CRC::Calculate(fromBytes.data(), fromBytes.length(),
                              CRC::CRC_32_BZIP2());
    auto checksum = Varicode::pack32bits(crc);
    if (checksum.length() < 6) {
        checksum += QString(" ").repeated(6 - checksum.length());
    }
    return checksum;
}

bool Varicode::checksum32Valid(QString const &checksum, QString const &input) {
    auto fromBytes = input.toLocal8Bit();
    auto crc = CRC::Calculate(fromBytes.data(), fromBytes.length(),
                              CRC::CRC_32_BZIP2());
    return Varicode::pack32bits(crc) == checksum;
}

QStringList Varicode::parseCallsigns(QString const &input) {
    QStringList callsigns;
    QRegularExpression re(compound_callsign_pattern);
    QRegularExpressionMatchIterator iter = re.globalMatch(input);
    while (iter.hasNext()) {
        QRegularExpressionMatch match = iter.next();
        if (!match.hasMatch()) {
            continue;
        }
        QString callsign = match.captured("callsign").trimmed();
        if (!Varicode::isValidCallsign(callsign, nullptr)) {
            continue;
        }
        QRegularExpression m(grid_pattern);
        if (m.match(callsign).hasMatch()) {
            continue;
        }
        callsigns.append(callsign);
    }
    return callsigns;
}

QStringList Varicode::parseGrids(const QString &input) {
    QStringList grids;
    QRegularExpression re(grid_pattern);
    QRegularExpressionMatchIterator iter = re.globalMatch(input);
    while (iter.hasNext()) {
        QRegularExpressionMatch match = iter.next();
        if (!match.hasMatch()) {
            continue;
        }
        auto grid = match.captured("grid");
        if (grid == "RR73") {
            continue;
        }
        grids.append(grid);
    }
    return grids;
}

QList<QPair<int, QVector<bool>>>
Varicode::huffEncode(const QMap<QString, QString> &huff, QString const &text) {
    QList<QPair<int, QVector<bool>>> out;

    int i = 0;

    auto keys = huff.keys();
    std::sort(keys.begin(), keys.end(), [](QString const &a, QString const &b) {
        auto alen = a.length();
        auto blen = b.length();
        if (blen < alen) {
            return true;
        }
        if (alen < blen) {
            return false;
        }

        return b < a;
    });

    while (i < text.length()) {
        bool found = false;
        foreach (auto ch, keys) {
            if (QStringView(text.begin() + i, text.end()).startsWith(ch)) {
                out.append({ch.length(), Varicode::strToBits(huff[ch])});
                i += ch.length();
                found = true;
                break;
            }
        }

        if (!found) {
            i++;
        }
    }

    return out;
}

QString Varicode::huffDecode(QMap<QString, QString> const &huff,
                             QVector<bool> const &bitvec) {
    QString text;

    QString bits = Varicode::bitsToStr(bitvec);

    while (bits.length() > 0) {
        bool found = false;
        foreach (auto key, huff.keys()) {
            if (bits.startsWith(huff[key])) {
                if (key == EOT) {
                    text.append(" ");
                    found = false;
                    break;
                }
                text.append(key);
                bits = bits.mid(huff[key].length());
                found = true;
            }
        }
        if (!found) {
            break;
        }
    }

    return text;
}

QSet<QString> Varicode::huffValidChars(const QMap<QString, QString> &huff) {
    auto const keys = huff.keys();
    return QSet<QString>(keys.begin(), keys.end());
}

// convert char* array of 0 bytes and 1 bytes to bool vector
QVector<bool> Varicode::bytesToBits(char *bitvec, int n) {
    QVector<bool> bits;
    for (int i = 0; i < n; i++) {
        bits.append(bitvec[i] == 0x01);
    }
    return bits;
}

// convert string of 0s and 1s to bool vector
QVector<bool> Varicode::strToBits(QString const &bitvec) {
    QVector<bool> bits;
    foreach (auto ch, bitvec) {
        bits.append(ch == '1');
    }
    return bits;
}

QString Varicode::bitsToStr(QVector<bool> const &bitvec) {
    QString bits;
    foreach (auto bit, bitvec) {
        bits.append(bit ? "1" : "0");
    }
    return bits;
}

QVector<bool> Varicode::intToBits(quint64 value, int expected) {
    QVector<bool> bits;

    while (value) {
        bits.prepend((bool)(value & 1));
        value = value >> 1;
    }

    if (expected) {
        while (bits.count() < expected) {
            bits.prepend((bool)0);
        }
    }

    return bits;
}

quint64 Varicode::bitsToInt(QVector<bool> const value) {
    quint64 v = 0;
    foreach (bool bit, value) {
        v = (v << 1) + (int)(bit);
    }
    return v;
}

quint64 Varicode::bitsToInt(QVector<bool>::ConstIterator start, int n) {
    quint64 v = 0;
    for (int i = 0; i < n; i++) {
        int bit = (int)(*start);
        v = (v << 1) + (int)(bit);
        start++;
    }
    return v;
}

QVector<bool> Varicode::bitsListToBits(QList<QVector<bool>> &list) {
    QVector<bool> out;
    foreach (auto vec, list) {
        out += vec;
    }
    return out;
}

quint8 Varicode::unpack5bits(QString const &value) {
    return alphabet.indexOf(value.at(0));
}

// pack a 5-bit value from 0 to 31 into a single character
QString Varicode::pack5bits(quint8 packed) { return alphabet.at(packed % 32); }

quint8 Varicode::unpack6bits(QString const &value) {
    return alphabet.indexOf(value.at(0));
}

// pack a 6-bit value from 0 to 40 into a single character
QString Varicode::pack6bits(quint8 packed) { return alphabet.at(packed % 41); }

quint16 Varicode::unpack16bits(QString const &value) {
    int a = alphabet.indexOf(value.at(0));
    int b = alphabet.indexOf(value.at(1));
    int c = alphabet.indexOf(value.at(2));

    int unpacked = (nalphabet * nalphabet) * a + nalphabet * b + c;
    if (unpacked > (1 << 16) - 1) {
        // BASE-41 can produce a value larger than 16 bits... ala "???" == 70643
        return 0;
    }

    return unpacked & ((1 << 16) - 1);
}

// pack a 16-bit value into a three character sequence
QString Varicode::pack16bits(quint16 packed) {
    QString out;
    quint16 tmp = packed / (nalphabet * nalphabet);

    out.append(alphabet.at(tmp));

    tmp = (packed - (tmp * (nalphabet * nalphabet))) / nalphabet;
    out.append(alphabet.at(tmp));

    tmp = packed % nalphabet;
    out.append(alphabet.at(tmp));

    return out;
}

quint32 Varicode::unpack32bits(QString const &value) {
    return (quint32)(unpack16bits(value.left(3))) << 16 |
           unpack16bits(value.right(3));
}

QString Varicode::pack32bits(quint32 packed) {
    quint16 a = (packed & 0xFFFF0000) >> 16;
    quint16 b = packed & 0xFFFF;
    return pack16bits(a) + pack16bits(b);
}

quint64 Varicode::unpack64bits(QString const &value) {
    return (quint64)(unpack32bits(value.left(6))) << 32 |
           unpack32bits(value.right(6));
}

QString Varicode::pack64bits(quint64 packed) {
    quint32 a = (packed & 0xFFFFFFFF00000000) >> 32;
    quint32 b = packed & 0xFFFFFFFF;
    return pack32bits(a) + pack32bits(b);
}

// returns the first 64 bits and sets the last 8 bits in pRem
quint64 Varicode::unpack72bits(QString const &text, quint8 *pRem) {
    quint64 value = 0;
    quint8 rem = 0;
    quint8 mask2 = ((1 << 2) - 1);

    for (int i = 0; i < 10; i++) {
        value |= (quint64)(alphabet72.indexOf(text.at(i))) << (58 - 6 * i);
    }

    quint8 remHigh = alphabet72.indexOf(text.at(10));
    value |= remHigh >> 2;

    quint8 remLow = alphabet72.indexOf(text.at(11));
    rem = ((remHigh & mask2) << 6) | remLow;

    if (pRem)
        *pRem = rem;
    return value;
}

QString Varicode::pack72bits(quint64 value, quint8 rem) {
    QChar packed[12]; // 12 x 6bit characters

    quint8 mask4 = ((1 << 4) - 1);
    quint8 mask6 = ((1 << 6) - 1);

    quint8 remHigh = ((value & mask4) << 2) | (rem >> 6);
    quint8 remLow = rem & mask6;
    value = value >> 4;

    packed[11] = alphabet72.at(remLow);
    packed[10] = alphabet72.at(remHigh);

    for (int i = 0; i < 10; i++) {
        packed[9 - i] = alphabet72.at(value & mask6);
        value = value >> 6;
    }

    return QString(packed, 12);
}

//     //
// --- //
//     //

// pack a 4-digit alpha-numeric + space into a 22 bit value
// 21 bits for the data + 1 bit for a flag indicator
// giving us a total of 5.5 bits per character
quint32 Varicode::packAlphaNumeric22(QString const &value, bool isFlag) {
    QString word =
        QString(value).replace(QRegularExpression("[^A-Z0-9/ ]"), "");
    if (word.length() < 4) {
        word = word + QString(" ").repeated(4 - word.length());
    }

    quint32 a = 38 * 38 * 38 * alphanumeric.indexOf(word.at(0));
    quint32 b = 38 * 38 * alphanumeric.indexOf(word.at(1));
    quint32 c = 38 * alphanumeric.indexOf(word.at(2));
    quint32 d = alphanumeric.indexOf(word.at(3));

    quint32 packed = a + b + c + d;
    packed = (packed << 1) + (int)isFlag;

    return packed;
}

QString Varicode::unpackAlphaNumeric22(quint32 packed, bool *isFlag) {
    QChar word[4];

    if (isFlag)
        *isFlag = packed & 1;
    packed = packed >> 1;

    quint32 tmp = packed % 38;
    word[3] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[2] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[1] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[0] = alphanumeric.at(tmp);
    packed = packed / 38;

    return QString(word, 4);
}

// pack a 10-digit alpha-numeric + space + forward-slash into a 50 bit value
// optionally start with an @
//
// [39][38][38][02][38][38][38][02][38][38][38]
// [K] [N] [4] [ ] [C] [R] [D] [/] [Q] [R] [P]
// [V] [E] [3] [/] [L] [B] [9] [ ] [Y] [H] [X]
// [@] [R] [A] [ ] [C] [E] [S] [ ] [ ] [ ] [ ]
//
// giving us a total of 4.5-5.55 bits per character
quint64 Varicode::packAlphaNumeric50(QString const &value) {
    QString word =
        QString(value).replace(QRegularExpression("[^A-Z0-9 /@]"), "");
    if (word.length() > 3 && word.at(3) != '/') {
        word.insert(3, ' ');
    }
    if (word.length() > 7 && word.at(7) != '/') {
        word.insert(7, ' ');
    }

    if (word.length() < 11) {
        word = word + QString(" ").repeated(11 - word.length());
    }

    quint64 a = (quint64)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 * 38 * 38 *
                alphanumeric.indexOf(word.at(0));
    quint64 b = (quint64)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 * 38 *
                alphanumeric.indexOf(word.at(1));
    quint64 c = (quint64)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 *
                alphanumeric.indexOf(word.at(2));
    quint64 d =
        (quint64)38 * 38 * 38 * 2 * 38 * 38 * 38 * (int)(word.at(3) == '/');
    quint64 e =
        (quint64)38 * 38 * 38 * 2 * 38 * 38 * alphanumeric.indexOf(word.at(4));
    quint64 f =
        (quint64)38 * 38 * 38 * 2 * 38 * alphanumeric.indexOf(word.at(5));
    quint64 g = (quint64)38 * 38 * 38 * 2 * alphanumeric.indexOf(word.at(6));
    quint64 h = (quint64)38 * 38 * 38 * (int)(word.at(7) == '/');
    quint64 i = (quint64)38 * 38 * alphanumeric.indexOf(word.at(8));
    quint64 j = (quint64)38 * alphanumeric.indexOf(word.at(9));
    quint64 k = (quint64)alphanumeric.indexOf(word.at(10));

    quint64 packed = a + b + c + d + e + f + g + h + i + j + k;

    return packed;
}

QString Varicode::unpackAlphaNumeric50(quint64 packed) {
    QChar word[11];

    quint64 tmp = packed % 38;
    word[10] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[9] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[8] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 2;
    word[7] = tmp ? '/' : ' ';
    packed = packed / 2;

    tmp = packed % 38;
    word[6] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[5] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[4] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 2;
    word[3] = tmp ? '/' : ' ';
    packed = packed / 2;

    tmp = packed % 38;
    word[2] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 38;
    word[1] = alphanumeric.at(tmp);
    packed = packed / 38;

    tmp = packed % 39;
    word[0] = alphanumeric.at(tmp);
    packed = packed / 39;

    auto value = QString(word, 11);

    return value.replace(" ", "");
}

// pack a callsign into a 28-bit value and a boolean portable flag
quint32 Varicode::packCallsign(QString const &value, bool *pPortable) {
    quint32 packed = 0;

    QString callsign = value.toUpper().trimmed();

    if (basecalls.contains(callsign)) {
        return basecalls[callsign];
    }

    // strip /P
    if (callsign.endsWith("/P")) {
        callsign = callsign.left(callsign.length() - 2);

        if (pPortable)
            *pPortable = true;
    }

    // workaround for swaziland
    if (callsign.startsWith("3DA0")) {
        callsign = "3D0" + callsign.mid(4);
    }

    // workaround for guinea
    if (callsign.startsWith("3X") && 'A' <= callsign.at(2) &&
        callsign.at(2) <= 'Z') {
        callsign = "Q" + callsign.mid(2);
    }

    int slen = callsign.length();
    if (slen < 2) {
        return packed;
    }

    if (slen > 6) {
        return packed;
    }

    QStringList permutations = {callsign};
    if (slen == 2) {
        permutations.append(" " + callsign + "   ");
    }
    if (slen == 3) {
        permutations.append(" " + callsign + "  ");
        permutations.append(callsign + "   ");
    }
    if (slen == 4) {
        permutations.append(" " + callsign + " ");
        permutations.append(callsign + "  ");
    }
    if (slen == 5) {
        permutations.append(" " + callsign);
        permutations.append(callsign + " ");
    }

    QString matched;
    QRegularExpression m(pack_callsign_pattern);
    foreach (auto permutation, permutations) {
        auto match = m.match(permutation);
        if (match.hasMatch()) {
            matched = match.captured(0);
        }
    }
    if (matched.isEmpty()) {
        return packed;
    }
    if (matched.length() < 6) {
        return packed;
    }

    packed = alphanumeric.indexOf(matched.at(0));
    packed = 36 * packed + alphanumeric.indexOf(matched.at(1));
    packed = 10 * packed + alphanumeric.indexOf(matched.at(2));
    packed = 27 * packed + alphanumeric.indexOf(matched.at(3)) - 10;
    packed = 27 * packed + alphanumeric.indexOf(matched.at(4)) - 10;
    packed = 27 * packed + alphanumeric.indexOf(matched.at(5)) - 10;

    return packed;
}

QString Varicode::unpackCallsign(quint32 value, bool portable) {
    foreach (auto key, basecalls.keys()) {
        if (basecalls[key] == value) {
            return key;
        }
    }

    QChar word[6];
    quint32 tmp = value % 27 + 10;
    word[5] = alphanumeric.at(tmp);
    value = value / 27;

    tmp = value % 27 + 10;
    word[4] = alphanumeric.at(tmp);
    value = value / 27;

    tmp = value % 27 + 10;
    word[3] = alphanumeric.at(tmp);
    value = value / 27;

    tmp = value % 10;
    word[2] = alphanumeric.at(tmp);
    value = value / 10;

    tmp = value % 36;
    word[1] = alphanumeric.at(tmp);
    value = value / 36;

    tmp = value;
    word[0] = alphanumeric.at(tmp);

    QString callsign(word, 6);
    if (callsign.startsWith("3D0")) {
        callsign = "3DA0" + callsign.mid(3);
    }

    if (callsign.startsWith("Q") and 'A' <= callsign.at(1) &&
        callsign.at(1) <= 'Z') {
        callsign = "3X" + callsign.mid(1);
    }

    if (portable) {
        callsign = callsign.trimmed() + "/P";
    }

    return callsign.trimmed();
}

QString Varicode::deg2grid(float dlong, float dlat) {
    QChar grid[6];

    if (dlong < -180) {
        dlong += 360;
    }
    if (dlong > 180) {
        dlong -= 360;
    }

    int nlong = int(60.0 * (180.0 - dlong) / 5);

    int n1 = nlong / 240;
    int n2 = (nlong - 240 * n1) / 24;
    int n3 = (nlong - 240 * n1 - 24 * n2);

    grid[0] = QChar('A' + n1);
    grid[2] = QChar('0' + n2);
    grid[4] = QChar('a' + n3);

    int nlat = int(60.0 * (dlat + 90) / 2.5);

    n1 = nlat / 240;
    n2 = (nlat - 240 * n1) / 24;
    n3 = (nlat - 240 * n1 - 24 * n2);

    grid[1] = QChar('A' + n1);
    grid[3] = QChar('0' + n2);
    grid[5] = QChar('a' + n3);

    return QString(grid, 6);
}

QPair<float, float> Varicode::grid2deg(QString const &grid) {
    QPair<float, float> longLat;

    QString g = grid;
    if (g.length() < 6) {
        g = grid.left(4) + "mm";
    }

    g = g.left(4).toUpper() + g.right(2).toLower();

    int nlong = 180 - 20 * (g.at(0).toLatin1() - 'A');
    int n20d = 2 * (g.at(2).toLatin1() - '0');
    float xminlong = 5 * (g.at(4).toLatin1() - 'a' + 0.5);
    float dlong = nlong - n20d - xminlong / 60.0;

    int nlat = -90 + 10 * (g.at(1).toLatin1() - 'A') + g.at(3).toLatin1() - '0';
    float xminlat = 2.5 * (g.at(5).toLatin1() - 'a' + 0.5);
    float dlat = nlat + xminlat / 60.0;

    longLat.first = dlong;
    longLat.second = dlat;

    return longLat;
}

// pack a 4-digit maidenhead grid locator into a 15-bit value
quint16 Varicode::packGrid(QString const &value) {
    QString grid = QString(value).trimmed();
    if (grid.length() < 4) {
        return (1 << 15) - 1;
    }

    auto pair = Varicode::grid2deg(grid.left(4));
    int ilong = pair.first;
    int ilat = pair.second + 90;

    return ((ilong + 180) / 2) * 180 + ilat;
}

QString Varicode::unpackGrid(quint16 value) {
    if (value > nbasegrid) {
        return "";
    }

    float dlat = value % 180 - 90;
    float dlong = value / 180 * 2 - 180 + 2;

    return Varicode::deg2grid(dlong, dlat).left(4);
}

// pack a number or snr into an integer between 0 & 62
quint8 Varicode::packNum(QString const &num, bool *ok) {
    int inum = 0;
    if (num.isEmpty()) {
        if (ok)
            *ok = false;
        return inum;
    }

    inum = qMax(-30, qMin(num.toInt(ok, 10), 31));
    return inum + 30 + 1;
}

// pack a reduced fidelity command and a number into the extra bits provided
// between nbasegrid and nmaxgrid
quint8 Varicode::packCmd(quint8 cmd, quint8 num, bool *pPackedNum) {
    // quint8 allowed = nmaxgrid - nbasegrid - 1;

    // if cmd == snr
    quint8 value = 0;
    auto cmdStr = directed_cmds.key(cmd);
    if (Varicode::isSNRCommand(cmdStr)) {
        // 8 bits - 1 bit flag + 1 bit type + 6 bit number
        // [1][X][6]
        // X = 0 == SNR
        // X = 1 == DEADBEEF
        value = ((1 << 1) | (int)(cmdStr == " HEARTBEAT SNR")) << 6;
        value = value + (num & ((1 << 6) - 1));
        if (pPackedNum)
            *pPackedNum = true;
    } else {
        value = cmd & ((1 << 7) - 1);
        if (pPackedNum)
            *pPackedNum = false;
    }

    return value;
}

quint8 Varicode::unpackCmd(quint8 value, quint8 *pNum) {
    // if the first bit is 1, this is an SNR with a number encoded in the lower
    // 6 bits
    if (value & (1 << 7)) {
        if (pNum)
            *pNum = value & ((1 << 6) - 1);

        auto cmd = directed_cmds[" SNR"];

        // sending digits with ACKS this way was deprecated in 2.2 (for reasons)
        // so we zero them out when unpacking so we don't display them even if
        // they were encoded that way.
        if (value & (1 << 6)) {
            cmd = directed_cmds[" HEARTBEAT SNR"];
        }
        return cmd;
    } else {
        if (pNum)
            *pNum = 0;
        return value & ((1 << 7) - 1);
    }
}

bool Varicode::isSNRCommand(const QString &cmd) {
    return directed_cmds.contains(cmd) && snr_cmds.contains(directed_cmds[cmd]);
}

bool Varicode::isCommandAllowed(const QString &cmd) {
    return directed_cmds.contains(cmd) &&
           allowed_cmds.contains(directed_cmds[cmd]);
}

bool Varicode::isCommandBuffered(const QString &cmd) {
    return directed_cmds.contains(cmd) &&
           (cmd.contains(" ") || buffered_cmds.contains(directed_cmds[cmd]));
}

int Varicode::isCommandChecksumed(const QString &cmd) {
    if (!directed_cmds.contains(cmd) ||
        !checksum_cmds.contains(directed_cmds[cmd])) {
        return 0;
    }

    return checksum_cmds[directed_cmds[cmd]];
}

bool Varicode::isCommandAutoreply(const QString &cmd) {
    return directed_cmds.contains(cmd) &&
           (autoreply_cmds.contains(directed_cmds[cmd]));
}

bool isValidCompoundCallsign(QStringView callsign) {
    // compound calls cannot be > 9 characters after removing the /
    if (callsign.length() - callsign.count('/') > 9) {
        return false;
    }

    // compound is valid when it is:
    // 1) a group call (starting with @)
    // 2) an actual compound call (containing /) that is not a base call
    // 3) is greater than two characters and has an alphanumeric character
    // sequence
    //
    // this is so arbitrary words < 10 characters in length don't end up coded
    // as callsigns

    if (const auto index = callsign.indexOf('/'); index != -1) {
        return !basecalls.contains(callsign.first(index).toString());
    }

    if (callsign.startsWith('@')) {
        return true;
    }

    if (callsign.length() > 2 && QRegularExpression("[0-9][A-Z]|[A-Z][0-9]")
                                     .matchView(callsign)
                                     .hasMatch()) {
        return true;
    }

    return false;
}

bool Varicode::isValidCallsign(const QString &callsign, bool *pIsCompound) {
    if (basecalls.contains(callsign)) {
        if (pIsCompound)
            *pIsCompound = false;
        return true;
    }

    auto match = QRegularExpression(base_callsign_pattern).match(callsign);
    if (match.hasMatch() && (match.capturedLength() == callsign.length())) {
        if (pIsCompound)
            *pIsCompound = false;
        return callsign.length() > 2 &&
               QRegularExpression("[0-9][A-Z]|[A-Z][0-9]")
                   .match(callsign)
                   .hasMatch();
    }

    match = QRegularExpression("^" + compound_callsign_pattern).match(callsign);

    if (match.hasMatch() && (match.capturedLength() == callsign.length())) {
        bool isValid = isValidCompoundCallsign(match.capturedView(0));

        if (pIsCompound)
            *pIsCompound = isValid;
        return isValid;
    }

    if (pIsCompound)
        *pIsCompound = false;
    return false;
}

bool Varicode::isCompoundCallsign(const QString &callsign) {
    if (basecalls.contains(callsign) && !callsign.startsWith("@")) {
        return false;
    }

    auto match = QRegularExpression(base_callsign_pattern).match(callsign);
    if (match.hasMatch() && (match.capturedLength() == callsign.length())) {
        return false;
    }

    match = QRegularExpression("^" + compound_callsign_pattern).match(callsign);
    if (!match.hasMatch() || (match.capturedLength() != callsign.length())) {
        return false;
    }

    bool isValid = isValidCompoundCallsign(match.capturedView(0));

    qCDebug(varicode_js8) << "is valid compound?" << match.capturedView(0)
                          << isValid;

    return isValid;
}

bool Varicode::isGroupAllowed(const QString &group) {
    const QSet<QString> disallowed = {
        "@APRSIS",
        "@JS8NET",
    };
    return !disallowed.contains(group);
}

int Varicode::dmmToTenths(int degrees, double minutes, bool negative) {
    int const t = degrees * 600 + qRound(minutes * 10.0);
    return negative ? -t : t;
}

QString Varicode::packOpsFrame(quint32 id, int latTenths, int lonTenths,
                               int course, int speed) {
    quint32 const latMag = qAbs(latTenths);
    quint32 const lonMag = qAbs(lonTenths);

    if (id > 99999 || latMag > 54000 || lonMag > 108000) {
        return QString{};
    }

    quint32 const latField = ((quint32)(latTenths < 0) << 17) | latMag;
    quint32 const lonField = ((quint32)(lonTenths < 0) << 18) | lonMag;
    quint32 const pcourse = (course >= 0 && course <= 359) ? course : 511;
    quint32 const pspeed = qBound(0, speed, 63);

    auto const bits = Varicode::intToBits(Varicode::FrameOps, 3) +
                      Varicode::intToBits(id, 17) +
                      Varicode::intToBits(latField, 18) +
                      Varicode::intToBits(lonField, 19) +
                      Varicode::intToBits(pcourse, 9) +
                      Varicode::intToBits(pspeed, 6);

    quint64 const value = Varicode::bitsToInt(bits.constBegin(), 64);
    quint8 const rem = (quint8)Varicode::bitsToInt(bits.constBegin() + 64, 8);
    return Varicode::pack72bits(value, rem);
}

bool Varicode::unpackOpsFrame(QString const &text, quint32 *id,
                              int *latTenths, int *lonTenths, int *course,
                              int *speed) {
    if (text.length() < 12 || text.contains(" ")) {
        return false;
    }

    quint8 rem = 0;
    auto const bits =
        Varicode::intToBits(Varicode::unpack72bits(text, &rem), 64) +
        Varicode::intToBits(rem, 8);

    if (Varicode::bitsToInt(bits.mid(0, 3)) != Varicode::FrameOps) {
        return false;
    }

    quint32 const pid = Varicode::bitsToInt(bits.mid(3, 17));
    quint32 const latField = Varicode::bitsToInt(bits.mid(20, 18));
    quint32 const lonField = Varicode::bitsToInt(bits.mid(38, 19));
    quint32 const pcourse = Varicode::bitsToInt(bits.mid(57, 9));
    quint32 const pspeed = Varicode::bitsToInt(bits.mid(66, 6));

    quint32 const latMag = latField & 0x1FFFF;
    quint32 const lonMag = lonField & 0x3FFFF;

    if (pid > 99999 || latMag > 54000 || lonMag > 108000) {
        return false; // out-of-range: reject
    }

    if (id)         *id = pid;
    if (latTenths)  *latTenths = (latField >> 17) ? -(int)latMag : (int)latMag;
    if (lonTenths)  *lonTenths = (lonField >> 18) ? -(int)lonMag : (int)lonMag;
    if (course)     *course = (pcourse <= 359) ? (int)pcourse : -1; // -1 = unknown
    if (speed)      *speed = (int)pspeed;

    return true;
}

QString packHuffMessage(const QString &input, const QVector<bool> prefix,
                        int *n) {
    static const int frameSize = 72;

    QString frame;

    // [1][1][70] = 72
    // The first bit is a flag that indicates this is a data frame, technically
    // encoded as [100] but, since none of the other frame types start with a 0,
    // we can drop the two zeros and use them for encoding the first two bits of
    // the actuall data sent. boom! The second bit is a flag that indicates this
    // is not compressed frame (huffman coding)
    QVector<bool> frameBits;
    if (!prefix.isEmpty()) {
        frameBits << prefix;
    }

    int i = 0;

    // only pack huff messages that only contain valid chars
    QString::const_iterator it;
    QSet<QString> validChars =
        Varicode::huffValidChars(Varicode::defaultHuffTable());
    for (it = input.constBegin(); it != input.constEnd(); it++) {
        auto ch = (*it).toUpper();
        if (!validChars.contains(ch)) {
            if (n)
                *n = 0;
            return frame;
        }
    }

    // pack using the default huff table
    foreach (auto pair,
             Varicode::huffEncode(Varicode::defaultHuffTable(), input)) {
        auto charN = pair.first;
        auto charBits = pair.second;
        if (frameBits.length() + charBits.length() < frameSize) {
            frameBits += charBits;
            i += charN;
            continue;
        }
        break;
    }

    qCDebug(varicode_js8) << "Huff bits" << frameBits.length() << "chars" << i;

    int pad = frameSize - frameBits.length();
    if (pad) {
        // the way we will pad is this...
        // set the bit after the frame to 0 and every bit after that a 1
        // to unpad, seek from the end of the bits until you hit a zero... the
        // rest is the actual frame.
        for (int i = 0; i < pad; i++) {
            frameBits.append(i == 0 ? (bool)0 : (bool)1);
        }
    }

    quint64 value = Varicode::bitsToInt(frameBits.constBegin(), 64);
    quint8 rem = (quint8)Varicode::bitsToInt(frameBits.constBegin() + 64, 8);
    frame = Varicode::pack72bits(value, rem);

    if (n)
        *n = i;

    return frame;
}

QString packCompressedMessage(const QString &input, QVector<bool> prefix,
                              int *n) {
    static const int frameSize = 72;

    QString frame;

    // [1][1][70] = 72
    // The first bit is a flag that indicates this is a data frame, technically
    // encoded as [100] but, since none of the other frame types start with a 1,
    // we can drop the two zeros and use them for encoding the first two bits of
    // the actuall data sent. boom! The second bit is a flag that indicates this
    // is a compressed frame (dense coding) For fast modes, we don't use the
    // prefix since it is indicated by the JS8CallData flag.
    QVector<bool> frameBits;
    if (!prefix.isEmpty()) {
        frameBits << prefix;
    }

    int i = 0;
    foreach (auto pair, JSC::compress(input)) {
        auto bits = pair.first;
        auto chars = pair.second;

        if (frameBits.length() + bits.length() < frameSize) {
            frameBits.append(bits);
            i += chars;
            continue;
        }

        break;
    }

    qCDebug(varicode_js8) << "Compressed bits" << frameBits.length() << "chars"
                          << i;

    int pad = frameSize - frameBits.length();
    if (pad) {
        // the way we will pad is this...
        // set the bit after the frame to 0 and every bit after that a 1
        // to unpad, seek from the end of the bits until you hit a zero... the
        // rest is the actual frame.
        for (int i = 0; i < pad; i++) {
            frameBits.append(i == 0 ? (bool)0 : (bool)1);
        }
    }

    quint64 value = Varicode::bitsToInt(frameBits.constBegin(), 64);
    quint8 rem = (quint8)Varicode::bitsToInt(frameBits.constBegin() + 64, 8);
    frame = Varicode::pack72bits(value, rem);

    if (n)
        *n = i;

    return frame;
}

// TODO: DEPRECATED in 2.2 (we will eventually stop transmitting these frames)
// pack data message using 70 bits available flagged as data by the first 2 bits
QString Varicode::packDataMessage(const QString &input, int *n) {
    QString huffFrame;
    int huffChars = 0;
    huffFrame = packHuffMessage(input, {true, false}, &huffChars);

    QString compressedFrame;
    int compressedChars = 0;
    compressedFrame =
        packCompressedMessage(input, {true, true}, &compressedChars);

    if (huffChars > compressedChars) {
        if (n)
            *n = huffChars;
        return huffFrame;
    } else {
        if (n)
            *n = compressedChars;
        return compressedFrame;
    }
}

// TODO: DEPRECATED in 2.2 (still available for decoding legacy frames, but will
// eventually no longer be decodable) unpack data message using 70 bits
// available flagged as data by the first 2 bits
QString Varicode::unpackDataMessage(const QString &text) {
    QString unpacked;

    if (text.length() < 12 || text.contains(" ")) {
        return unpacked;
    }

    quint8 rem = 0;
    quint64 value = Varicode::unpack72bits(text, &rem);
    auto bits = Varicode::intToBits(value, 64) + Varicode::intToBits(rem, 8);

    bool isData = bits.at(0);
    if (!isData) {
        return unpacked;
    }

    bits = bits.mid(1);

    bool compressed = bits.at(0);
    int n = bits.lastIndexOf(0);

    // trim off the pad bits
    bits = bits.mid(1, n - 1);

    if (compressed) {
        // partial word (s,c)-dense coding with code tables
        unpacked = JSC::decompress(bits);
    } else {
        // huff decode the bits (without escapes)
        unpacked = Varicode::huffDecode(Varicode::defaultHuffTable(), bits);
    }

    return unpacked;
}

#define JS8_FAST_DATA_CAN_USE_HUFF 0

// pack data message using the full 72 bits available (with the data flag in the
// i3bit header)
QString Varicode::packFastDataMessage(const QString &input, int *n) {
#if JS8_FAST_DATA_CAN_USE_HUFF
    QString huffFrame;
    int huffChars = 0;
    huffFrame = packHuffMessage(input, {false}, &huffChars);

    QString compressedFrame;
    int compressedChars = 0;
    compressedFrame = packCompressedMessage(input, {true}, &compressedChars);

    if (huffChars > compressedChars) {
        if (n)
            *n = huffChars;
        return huffFrame;
    } else {
        if (n)
            *n = compressedChars;
        return compressedFrame;
    }
#else
    QString compressedFrame;
    int compressedChars = 0;
    compressedFrame = packCompressedMessage(input, {}, &compressedChars);

    if (n)
        *n = compressedChars;
    return compressedFrame;
#endif
}

// unpack data message using the full 72 bits available (with the data flag in
// the i3bit header)
QString Varicode::unpackFastDataMessage(const QString &text) {
    QString unpacked;

    if (text.length() < 12 || text.contains(" ")) {
        return unpacked;
    }

    quint8 rem = 0;
    quint64 value = Varicode::unpack72bits(text, &rem);
    auto bits = Varicode::intToBits(value, 64) + Varicode::intToBits(rem, 8);

#if JS8_FAST_DATA_CAN_USE_HUFF
    bool compressed = bits.at(0);
    int n = bits.lastIndexOf(0);

    // trim off the pad bits
    bits = bits.mid(1, n - 1);

    if (compressed) {
        // partial word (s,c)-dense coding with code tables
        unpacked = JSC::decompress(bits);
    } else {
        // huff decode the bits (without escapes)
        unpacked = Varicode::huffDecode(Varicode::defaultHuffTable(), bits);
    }
#else
    int n = bits.lastIndexOf(0);

    // trim off the pad bits
    bits = bits.mid(0, n);

    // partial word (s,c)-dense coding with code tables
    unpacked = JSC::decompress(bits);
#endif

    return unpacked;
}

QList<QPair<QString, int>>
Varicode::buildMessageFrames(QString const &mycall, QString const &mygrid,
                             QString const &selectedCall, QString const &text,
                             bool forceIdentify, bool forceData, int submode,
                             MessageInfo *pInfo) {
    Q_UNUSED(mygrid);
    Q_UNUSED(selectedCall);
    Q_UNUSED(pInfo);

    QList<QPair<QString, int>> allFrames;
    QStringList lines = {text};

    foreach (QString line, lines) {
        QList<QPair<QString, int>> lineFrames;

        if (forceData) {
            forceIdentify = false;
        }

        // remove our callsign from the start of the line...
        if (line.startsWith(mycall + ":") || line.startsWith(mycall + " ")) {
            line = lstrip(line.mid(mycall.length() + 1));
        }

        // ensure our callsign is included
        if (forceIdentify && !line.isEmpty() && !line.contains(mycall)) {
            line = QString("%1: %2").arg(mycall).arg(line);
        }

        bool const fast = submode != Varicode::JS8CallNormal;

        while (line.size() > 0) {
            int m = 0;
            QString frame = fast ? packFastDataMessage(line, &m)
                                 : packDataMessage(line, &m);
            if (m <= 0 || frame.isEmpty()) {
                break; // nothing packable; avoids looping forever
            }
            lineFrames.append(
                {frame, fast ? Varicode::JS8CallData : Varicode::JS8Call});
            line = line.mid(m);
        }

        if (!lineFrames.isEmpty()) {
            lineFrames.first().second |= Varicode::JS8CallFirst;
            lineFrames.last().second |= Varicode::JS8CallLast;
        }

        allFrames.append(lineFrames);
    }

    return allFrames;
}

BuildMessageFramesThread::BuildMessageFramesThread(
    const QString &mycall, const QString &mygrid, const QString &selectedCall,
    const QString &text, bool forceIdentify, bool forceData, int submode,
    QObject *parent)
    : QThread(parent), m_mycall{mycall}, m_mygrid{mygrid},
      m_selectedCall{selectedCall}, m_text{text},
      m_forceIdentify{forceIdentify}, m_forceData{forceData},
      m_submode{submode} {}

void BuildMessageFramesThread::run() {
    auto results =
    Varicode::buildMessageFrames(m_mycall, m_mygrid, m_selectedCall, m_text,
                                 m_forceIdentify, m_forceData, m_submode);
    
    QStringList textList;
    qCDebug(varicode_js8) << "frames:";
    foreach (auto frame, results) {
        auto dt = DecodedText(frame.first, frame.second, m_submode);
        qCDebug(varicode_js8) << "->" << frame << dt.message()
        << Varicode::frameTypeString(dt.frameType())
        << "submode:" << m_submode;
        textList.append(dt.message());
    }
    
    emit resultReady(results.length());
}

Varicode::SubmodeType Varicode::intToSubmode(int sm) {
    switch (sm) {
    case Varicode::JS8CallNormal:
        return Varicode::JS8CallNormal;
    case Varicode::JS8CallFast:
        return Varicode::JS8CallFast;
    case Varicode::JS8CallTurbo:
        return Varicode::JS8CallTurbo;
    case Varicode::JS8CallSlow:
        return Varicode::JS8CallSlow;
    case Varicode::JS8CallUltra:
        return Varicode::JS8CallUltra;
    };
    throw std::invalid_argument{
        boost::str(boost::format("Unexpected JS8 submode %1%") % sm)};
}

Q_LOGGING_CATEGORY(varicode_js8, "varicode.js8", QtWarningMsg)
