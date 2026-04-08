// main.cpp
#include <QApplication>
#include "remote_mouse_widget.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // 设置应用程序图标
    app.setWindowIcon(QIcon(":/icons/logo.png"));

    RemoteMouseWidget widget;
    widget.show();

    return app.exec();
}
