#include "OpsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

OpsDialog::OpsDialog(QWidget *parent) : QDialog{parent} {
    setWindowTitle(tr("CGCall OPS Entry"));

    auto spin = [this](int lo, int hi, QString const &suffix = {}) {
        auto *s = new QSpinBox(this);
        s->setRange(lo, hi);
        s->setSuffix(suffix);
        return s;
    };
    auto combo = [this](QString const &a, char aData, QString const &b, char bData) {
        auto *c = new QComboBox(this);
        c->addItem(a, QString(QChar(aData)));
        c->addItem(b, QString(QChar(bData)));
        return c;
    };

    to_ = spin(0, 127);
    status_ = combo(tr("Underway"), 'U', tr("Anchored"), 'A');
    latDeg_ = spin(0, 90, QString(QChar(0x00B0)));
    latMin_ = spin(0, 59, QStringLiteral("'"));
    latDir_ = combo(QStringLiteral("N"), 'N', QStringLiteral("S"), 'S');
    lonDeg_ = spin(0, 180, QString(QChar(0x00B0)));
    lonMin_ = spin(0, 59, QStringLiteral("'"));
    lonDir_ = combo(QStringLiteral("E"), 'E', QStringLiteral("W"), 'W');
    course_ = spin(-1, 359, QString(QChar(0x00B0)));
    course_->setSpecialValueText(tr("Unknown")); // -1
    speed_ = spin(0, 63, tr(" kt"));

    auto row = [](QWidget *a, QWidget *b, QWidget *c) {
        auto *h = new QHBoxLayout;
        h->addWidget(a); h->addWidget(b); h->addWidget(c);
        return h;
    };

    auto *form = new QFormLayout;
    form->addRow(tr("To (sector, 0 = everyone)"), to_);
    form->addRow(tr("Status"), status_);
    form->addRow(tr("Latitude"), row(latDeg_, latMin_, latDir_));
    form->addRow(tr("Longitude"), row(lonDeg_, lonMin_, lonDir_));
    form->addRow(tr("Course (true)"), course_);
    form->addRow(tr("Speed"), speed_);

    preview_ = new QLabel(this);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Send"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(preview_);
    layout->addWidget(buttons);

    // minutes must be 0 at 90 N/S or 180 E/W
    connect(latDeg_, &QSpinBox::valueChanged, this,
            [this](int d) { latMin_->setMaximum(d >= 90 ? 0 : 59); });
    connect(lonDeg_, &QSpinBox::valueChanged, this,
            [this](int d) { lonMin_->setMaximum(d >= 180 ? 0 : 59); });

    for (auto *s : {to_, latDeg_, latMin_, lonDeg_, lonMin_, course_, speed_})
        connect(s, &QSpinBox::valueChanged, this, &OpsDialog::updatePreview);
    for (auto *c : {status_, latDir_, lonDir_})
        connect(c, &QComboBox::currentIndexChanged, this, &OpsDialog::updatePreview);

    updatePreview();
}

QString OpsDialog::commandText() const {
    int const crs = course_->value();
    return QStringLiteral("OPS %1 %2 %3 %4 %5 %6 %7 %8 %9 %10")
        .arg(to_->value())
        .arg(status_->currentData().toString())
        .arg(latDeg_->value())
        .arg(latMin_->value())
        .arg(latDir_->currentData().toString())
        .arg(lonDeg_->value())
        .arg(lonMin_->value())
        .arg(lonDir_->currentData().toString())
        .arg(crs < 0 ? QStringLiteral("---") : QString::number(crs))
        .arg(speed_->value());
}

void OpsDialog::updatePreview() { preview_->setText(commandText()); }
