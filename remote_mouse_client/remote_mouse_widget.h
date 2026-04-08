// remote_mouse_widget.h
#ifndef REMOTE_MOUSE_WIDGET_H
#define REMOTE_MOUSE_WIDGET_H

#include <QWidget>
#include <QTcpSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QDebug>
#include <QTimer>
#include <QLabel>

class RemoteMouseWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteMouseWidget(QWidget *parent = nullptr);
    ~RemoteMouseWidget();

private slots:
    void connectToServer();
    void onTcpReadyRead();
    void onTcpConnected();
    void onTcpDisconnected();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void sendFrame(int32_t abs_x, int32_t abs_y, int32_t buttons);
    void mapToDevice(const QPoint &widgetPos, int32_t &devX, int32_t &devY);
    void processHandshake();
    void updateStatusLabel(const QString &text, const QColor &bgColor);

    QTcpSocket *m_tcpSocket;
    quint32 m_buttonsState;

    QLabel *m_statusLabel;

    // 握手状态
    bool m_handshakeDone;          // 是否已完成握手（收到设备分辨率）
    QByteArray m_handshakeBuffer;  // 握手数据缓冲区

    // 设备端屏幕分辨率（连接后由服务端发送，动态设置）
    int32_t m_deviceWidth;
    int32_t m_deviceHeight;

    // 客户端触摸板固定分辨率
    static const int TOUCHPAD_WIDTH  = 400;
    static const int TOUCHPAD_HEIGHT = 300;

    // 服务器地址
    static const QString SERVER_IP;
    static const quint16 SERVER_PORT;
};

#endif // REMOTE_MOUSE_WIDGET_H
