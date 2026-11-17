#ifndef ACKDIALOG_H
#define ACKDIALOG_H

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

// Builds: ACK <to> <OVER|OUT>   (FROM is always this station's id)
class AckDialog final : public QDialog {
    Q_OBJECT

  public:
    AckDialog(QString const &to, QString const &from, QWidget *parent = nullptr);
    QString commandText() const;

  private:
    void updateState();

    QLineEdit *to_;
    QComboBox *over_;
    QLabel *preview_;
    QDialogButtonBox *buttons_;
};

#endif // ACKDIALOG_H
