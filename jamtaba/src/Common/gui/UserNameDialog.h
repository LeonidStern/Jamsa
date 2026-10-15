#ifndef USERNAMEDIALOG_H
#define USERNAMEDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui {
class UserNameDialog;
}
QT_END_NAMESPACE

class UserNameDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UserNameDialog(QWidget *parent, const QString &lastUserName);
    ~UserNameDialog();
    QString getUserName() const;

private slots:
    void updateOkButtonAvailability();

private:
    Ui::UserNameDialog *ui;
};

#endif // USERNAMEDIALOG_H
