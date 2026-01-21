QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++20

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    dataprocess.cpp \
    jtrequest.cpp \
    logger.cpp \
    main.cpp \
    loopline_houjie.cpp \
    otherfunction.cpp \
    qttcpserver.cpp \
    sqlconnection.cpp \
    sqlconnectionpool.cpp \
    tcpsocketclient.cpp

HEADERS += \
    dataprocess.h \
    jtrequest.h \
    logger.h \
    loopline_houjie.h \
    qttcpserver.h \
    spsc_ring.h \
    sqlconnection.h \
    sqlconnectionpool.h \
    tcpsocketclient.h \
    udpreceiver.h

FORMS += \
    loopline_houjie.ui

TRANSLATIONS += \
    loopline_houjie_zh_CN.ts
CONFIG += lrelease
CONFIG += embed_translations

#INCLUDEPATH += D:/vcpkg/installed/x64-windows/include               #公司电脑
#MYSQL_DIR = "D:/Program Files/MySQL/MySQL Server 8.0"
INCLUDEPATH += D:/yhz/Company/project/C++/src/vcpkg-master/installed/x64-windows/include        #家里电脑
MYSQL_DIR = D:/Software/MySQL                                       #家里电脑
INCLUDEPATH += $$MYSQL_DIR/include
LIBS += -L$$MYSQL_DIR/lib -llibmysql
#LIBS += -LD:/vcpkg/installed/x64-windows/lib -lspdlog              #公司电脑
#LIBS += "D:/vcpkg/packages/libsodium_x64-windows/lib/libsodium.lib"
LIBS += -D:/yhz/Company/project/C++/src/vcpkg-master/installed/x64-windows/lib
# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
