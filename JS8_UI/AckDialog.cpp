#include "AckDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

AckDialog::AckDialog(QString const &to, QString const &from, QWidget *parent)
    : QDialog{parent} {
    setWindowTitle(tr("CGCall ACK"));

    to_ = new QLineEdit(to, this);
    to_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(R"((\d{5}|@S\d{1,3}))",
                           QRegularExpression::CaseInsensitiveOption),
        to_));

    auto *fromLabel = new QLabel(from, this); // read-only: always MyCall

    over_ = new QComboBox(this);
    over_->addItem(tr("OUT - transmission complete"), QStringLiteral("OUT"));
    over_->addItem(tr("OVER - more to follow"), QStringLiteral("OVER"));

    auto *form = new QFormLayout;
    form->addRow(tr("To"), to_);
    form->addRow(tr("From"), fromLabel);
    form->addRow(tr("Status"), over_);

    preview_ = new QLabel(this);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(tr("Send"));
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(preview_);
    layout->addWidget(buttons_);

    connect(to_, &QLineEdit::textChanged, this, &AckDialog::updateState);
    connect(over_, &QComboBox::currentIndexChanged, this,
            &AckDialog::updateState);
    updateState();
}

QString AckDialog::commandText() const {
    return QStringLiteral("ACK %1 %2")
        .arg(to_->text().trimmed().toUpper(), over_->currentData().toString());
}

void AckDialog::updateState() {
    preview_->setText(commandText());
    buttons_->button(QDialogButtonBox::Ok)
        ->setEnabled(to_->hasAcceptableInput());
}
