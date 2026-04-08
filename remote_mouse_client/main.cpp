// main.cpp
#include <QApplication>
#include "remote_mouse_widget.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    RemoteMouseWidget widget;
    widget.show();

    return app.exec();
}
