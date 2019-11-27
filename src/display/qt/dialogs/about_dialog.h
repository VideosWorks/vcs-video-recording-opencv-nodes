#ifndef D_ABOUT_DIALOG_H
#define D_ABOUT_DIALOG_H

#include <QDialog>

namespace Ui {
class AboutDialog;
}

class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = 0);
    ~AboutDialog();

    void notify_of_new_program_version(void);

private:
    Ui::AboutDialog *ui;
};

#endif

