// remote_mouse_widget.h
#ifndef REMOTE_MOUSE_WIDGET_H
#define REMOTE_MOUSE_WIDGET_H

#include <QWidget>
#include <QMainWindow>
#include <QStatusBar>
#include <QTcpSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QDebug>
#include <QTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

// 触摸板区域自定义控件
class TouchpadArea : public QWidget
{
    Q_OBJECT

public:
    explicit TouchpadArea(QWidget *parent = nullptr);
    void setConnected(bool connected);

signals:
    void mousePressed(const QPoint &pos, Qt::MouseButton button);
    void mouseReleased(const QPoint &pos, Qt::MouseButton button);
    void mouseMoved(const QPoint &pos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    bool m_connected;
};

class RemoteMouseWidget : public QMainWindow
{
    Q_OBJECT

public:
    explicit RemoteMouseWidget(QWidget *parent = nullptr);
    ~RemoteMouseWidget();

private slots:
    void connectToServer(const QString &ip);
    void disconnectFromServer();
    void onTcpReadyRead();
    void onTcpConnected();
    void onTcpDisconnected();
    void onToggleButtonClicked();

private:
    void setupUI();
    void applyStyles();
    void setupStatusBar();
    void updateToggleButtonStyle(bool connected);
    void sendFrame(int32_t abs_x, int32_t abs_y, int32_t buttons);
    void mapToDevice(const QPoint &widgetPos, int32_t &devX, int32_t &devY);
    void processHandshake();
    void updateStatusLabel(const QString &text, const QColor &color);
    void setConnectionState(bool connected);

    QTcpSocket *m_tcpSocket;
    quint32 m_buttonsState;

    // 中央触摸板区域
    TouchpadArea *m_touchpadArea;
    QWidget *m_centralWidget;

    // StatusBar 组件 - 左侧控制区
    QLineEdit *m_ipInput;
    QPushButton *m_toggleBtn;  // 连接/断开复用按钮

    // StatusBar 组件 - 右侧状态区
    QLabel *m_statusIndicator;
    QLabel *m_statusLabel;
    QLabel *m_deviceInfoLabel;

    // 状态
    bool m_handshakeDone;
    QByteArray m_handshakeBuffer;
    int32_t m_deviceWidth;
    int32_t m_deviceHeight;
    bool m_isConnected;

    // 常量
    static const int TOUCHPAD_WIDTH = 400;
    static const int TOUCHPAD_HEIGHT = 300;
    static const quint16 SERVER_PORT = 9999;
};

#endif // REMOTE_MOUSE_WIDGET_H
