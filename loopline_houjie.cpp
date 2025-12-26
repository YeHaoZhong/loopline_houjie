#include "loopline_houjie.h"
#include "ui_loopline_houjie.h"

loopline_houjie::loopline_houjie(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::loopline_houjie)
{
    ui->setupUi(this);
}

loopline_houjie::~loopline_houjie()
{
    delete ui;
}
