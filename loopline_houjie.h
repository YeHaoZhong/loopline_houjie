#ifndef LOOPLINE_HOUJIE_H
#define LOOPLINE_HOUJIE_H

#include <QWidget>
#include "dataprocess.h"
QT_BEGIN_NAMESPACE
namespace Ui {
class loopline_houjie;
}
QT_END_NAMESPACE

class loopline_houjie : public QWidget
{
    Q_OBJECT

public:
    loopline_houjie(QWidget *parent = nullptr);
    ~loopline_houjie();

private:
    Ui::loopline_houjie *ui;
    DataProcess m_dataProcess;

private slots:
    void onRunBtnClicked();
    void onStopBtnClicked();

};
#endif // LOOPLINE_HOUJIE_H
