# remote_mouse.pro
QT += core widgets network
TARGET = remote_mouse_gui
TEMPLATE = app
SOURCES += \
    main.cpp \
    remote_mouse_widget.cpp

HEADERS += \
    remote_mouse_widget.h

# 平台特定图标配置
win32 {
    RC_FILE = icons/app_icon.rc
}

macx {
    ICON = icons/logo.icns
}

# 资源文件
RESOURCES += resources.qrc
