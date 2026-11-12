#ifndef OPSDIALOG_H
#define OPSDIALOG_H

#include <QDialog>

class QComboBox;
class QLabel;
class QSpinBox;

// Builds: OPS <to> <U|A> <latDeg> <latMin> <N|S> <lonDeg> <lonMin> <E|W> <crs> <spd>
class OpsDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit OpsDialog(QWidget *parent = nullptr);
    QString commandText() const;

  private:
    void updatePreview();

    QSpinBox *to_, *latDeg_, *latMin_, *lonDeg_, *lonMin_, *course_, *speed_;
    QComboBox *status_, *latDir_, *lonDir_;
    QLabel *preview_;
};

#endif // OPSDIALOG_H
