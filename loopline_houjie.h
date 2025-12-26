#ifndef LOOPLINE_HOUJIE_H
#define LOOPLINE_HOUJIE_H

#include <QWidget>

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
};
#endif // LOOPLINE_HOUJIE_H
