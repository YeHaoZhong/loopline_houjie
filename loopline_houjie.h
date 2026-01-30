#ifndef LOOPLINE_HOUJIE_H
#define LOOPLINE_HOUJIE_H

#include <QWidget>
#include "dataprocess.h"
#include "sqlconnectionpool.h"
#include <QCloseEvent>
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
    void onInOperatorClicked();
    void onOutOperatorClicked();
protected:
    void closeEvent(QCloseEvent *event) override
    {
        try{
            m_dataProcess.dataProCleanUp();
            Logger::getInstance().close();
            SqlConnectionPool::instance().shutdown();
        }catch(...){}
        event->accept();
    }
};
#endif // LOOPLINE_HOUJIE_H
