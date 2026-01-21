#include "loopline_houjie.h"
#include "ui_loopline_houjie.h"

loopline_houjie::loopline_houjie(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::loopline_houjie)
{
    ui->setupUi(this);

    connect(ui->_run_btn,&QPushButton::clicked,this,&loopline_houjie::onRunBtnClicked);
    connect(ui->_stop_btn, &QPushButton::clicked,this, &loopline_houjie::onStopBtnClicked);
}

loopline_houjie::~loopline_houjie()
{
    delete ui;
}

void loopline_houjie::onRunBtnClicked(){
    m_dataProcess.dataProInit();
}
void loopline_houjie::onStopBtnClicked(){
    m_dataProcess.tcpDisconnect();
}
