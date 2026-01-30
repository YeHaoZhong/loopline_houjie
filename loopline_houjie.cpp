#include "loopline_houjie.h"
#include "ui_loopline_houjie.h"
#include <QMessageBox>

loopline_houjie::loopline_houjie(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::loopline_houjie)
{
    ui->setupUi(this);
    ui->_stop_btn->setEnabled(false);
    connect(ui->_run_btn,&QPushButton::clicked,this,&loopline_houjie::onRunBtnClicked);
    connect(ui->_stop_btn, &QPushButton::clicked,this, &loopline_houjie::onStopBtnClicked);
    connect(ui->inOperator_btn,&QPushButton::clicked,this,&loopline_houjie::onInOperatorClicked);
    connect(ui->outOperator_btn,&QPushButton::clicked,this,&loopline_houjie::onOutOperatorClicked);
}

loopline_houjie::~loopline_houjie()
{
    delete ui;
}

void loopline_houjie::onRunBtnClicked(){

    ui->_run_btn->setEnabled(false);
    int type = m_dataProcess.getOperateType();
    if(type == 0){
        QMessageBox::warning(NULL,"警告","请选择操作模式！",QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        ui->_run_btn->setEnabled(true);
        ui->_stop_btn->setEnabled(false);
        return;
    }
    ui->_stop_btn->setEnabled(true);
    m_dataProcess.dataProInit();
}
void loopline_houjie::onStopBtnClicked(){
    ui->_run_btn->setEnabled(true);
    ui->_stop_btn->setEnabled(false);
    m_dataProcess.tcpDisconnect();
}
void loopline_houjie::onInOperatorClicked(){
    ui->inOperator_btn->setEnabled(false);
    ui->outOperator_btn->setEnabled(true);
    m_dataProcess.setOperateType(1);                    //进港
}
void loopline_houjie::onOutOperatorClicked(){
    ui->inOperator_btn->setEnabled(true);
    ui->outOperator_btn->setEnabled(false);
    m_dataProcess.setOperateType(2);                    //出港
}
