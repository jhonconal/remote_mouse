// remote_mouse_widget.cpp
#include "remote_mouse_widget.h"
#include <QFont>

const QString RemoteMouseWidget::SERVER_IP = "172.31.34.47"; // 替换为你的 i.MX8MP IP
const quint16 RemoteMouseWidget::SERVER_PORT = 9999;         // 与 remote_mouse_server.c 一致

RemoteMouseWidget::RemoteMouseWidget(QWidget *parent)
    : QWidget(parent)
    , m_tcpSocket(new QTcpSocket(this))
    , m_buttonsState(0)
    , m_statusLabel(new QLabel("Connecting...", this))
    , m_handshakeDone(false)
    , m_deviceWidth(0)
    , m_deviceHeight(0)
{
    // 固定窗口 400x300
    setWindowTitle("i.MX8MP Remote Mouse");
    setFixedSize(TOUCHPAD_WIDTH, TOUCHPAD_HEIGHT);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    // 状态标签
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
    updateStatusLabel("Connecting...", QColor(80, 80, 80));

    // TCP 信号连接
    connect(m_tcpSocket, &QTcpSocket::connected,
            this, &RemoteMouseWidget::onTcpConnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead,
            this, &RemoteMouseWidget::onTcpReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected,
            this, &RemoteMouseWidget::onTcpDisconnected);
    connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            [this](QAbstractSocket::SocketError socketError){
        qDebug() << "TCP Error:" << socketError;
        updateStatusLabel("Connection error", QColor(200, 0, 0));
    });

    m_tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connectToServer();
}

RemoteMouseWidget::~RemoteMouseWidget()
{
    if (m_tcpSocket->state() == QAbstractSocket::ConnectedState) {
        m_tcpSocket->disconnectFromHost();
    }
}

void RemoteMouseWidget::connectToServer()
{
    m_handshakeDone = false;
    m_handshakeBuffer.clear();
    m_deviceWidth = 0;
    m_deviceHeight = 0;
    updateStatusLabel("Connecting...", QColor(80, 80, 80));
    update();
    m_tcpSocket->connectToHost(SERVER_IP, SERVER_PORT);
}

void RemoteMouseWidget::onTcpConnected()
{
    qDebug() << "TCP connected, waiting for handshake...";
    updateStatusLabel("Handshaking...", QColor(180, 140, 0));
    // 握手数据会在 onTcpReadyRead 中处理
}

void RemoteMouseWidget::onTcpReadyRead()
{
    if (!m_handshakeDone) {
        // ---- 握手阶段：接收服务端发送的屏幕分辨率 (8字节) ----
        m_handshakeBuffer.append(m_tcpSocket->readAll());
        processHandshake();
    } else {
        // 正常运行阶段：目前协议是单向 PC->Device，丢弃服务端数据
        m_tcpSocket->readAll();
    }
}

void RemoteMouseWidget::processHandshake()
{
    // 需要 8 字节: [int32_t width, int32_t height]
    const int HANDSHAKE_SIZE = 2 * sizeof(int32_t);
    if (m_handshakeBuffer.size() < HANDSHAKE_SIZE) {
        return; // 等待更多数据
    }

    const int32_t *data = reinterpret_cast<const int32_t*>(m_handshakeBuffer.constData());
    m_deviceWidth  = data[0];
    m_deviceHeight = data[1];

    if (m_deviceWidth <= 0 || m_deviceHeight <= 0) {
        qDebug() << "Invalid resolution from server:" << m_deviceWidth << "x" << m_deviceHeight;
        updateStatusLabel("Invalid resolution!", QColor(200, 0, 0));
        m_tcpSocket->disconnectFromHost();
        return;
    }

    m_handshakeDone = true;
    qDebug() << "Handshake OK! Device resolution:" << m_deviceWidth << "x" << m_deviceHeight;

    QString statusText = QString("Connected | Device: %1x%2")
                             .arg(m_deviceWidth).arg(m_deviceHeight);
    updateStatusLabel(statusText, QColor(0, 128, 0));
    update(); // 触发重绘，显示分辨率信息

    // 如果握手数据后面还有多余字节，丢弃
    if (m_handshakeBuffer.size() > HANDSHAKE_SIZE) {
        qDebug() << "Extra bytes after handshake:" << m_handshakeBuffer.size() - HANDSHAKE_SIZE;
    }
    m_handshakeBuffer.clear();
}

void RemoteMouseWidget::onTcpDisconnected()
{
    qDebug() << "Disconnected from server.";
    m_handshakeDone = false;
    m_deviceWidth = 0;
    m_deviceHeight = 0;
    updateStatusLabel("Disconnected (retrying...)", QColor(200, 0, 0));
    update();
    QTimer::singleShot(5000, this, &RemoteMouseWidget::connectToServer);
}

void RemoteMouseWidget::updateStatusLabel(const QString &text, const QColor &bgColor)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QString("QLabel {"
                "  color: white;"
                "  background-color: rgba(%1, %2, %3, 200);"
                "  padding: 4px 8px;"
                "  border-radius: 4px;"
                "  font-size: 11px;"
                "}")
            .arg(bgColor.red()).arg(bgColor.green()).arg(bgColor.blue())
    );
    m_statusLabel->adjustSize();
    m_statusLabel->move(TOUCHPAD_WIDTH - m_statusLabel->width() - 6, 6);
}

// ---- 坐标映射: 触摸板 (400x300) -> 设备屏幕 (动态分辨率) ----
void RemoteMouseWidget::mapToDevice(const QPoint &widgetPos, int32_t &devX, int32_t &devY)
{
    devX = static_cast<int32_t>(
        static_cast<qint64>(widgetPos.x()) * (m_deviceWidth - 1) / (TOUCHPAD_WIDTH - 1));
    devY = static_cast<int32_t>(
        static_cast<qint64>(widgetPos.y()) * (m_deviceHeight - 1) / (TOUCHPAD_HEIGHT - 1));

    // 边界保护
    if (devX < 0) devX = 0;
    if (devX >= m_deviceWidth) devX = m_deviceWidth - 1;
    if (devY < 0) devY = 0;
    if (devY >= m_deviceHeight) devY = m_deviceHeight - 1;
}

// ---- 发送一帧 ----
void RemoteMouseWidget::sendFrame(int32_t abs_x, int32_t abs_y, int32_t buttons)
{
    if (!m_handshakeDone) return; // 握手未完成，不发送
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) return;

    int32_t frame[3] = { abs_x, abs_y, buttons };
    m_tcpSocket->write(reinterpret_cast<const char*>(frame), sizeof(frame));
    m_tcpSocket->flush();
}

// ---- 绘制触摸板 ----
void RemoteMouseWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 背景渐变
    QLinearGradient bgGrad(0, 0, 0, height());
    bgGrad.setColorAt(0.0, QColor(45, 45, 55));
    bgGrad.setColorAt(1.0, QColor(25, 25, 35));
    painter.fillRect(rect(), bgGrad);

    // 边框
    painter.setPen(QPen(QColor(80, 80, 100), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));

    // 网格线
    painter.setPen(QPen(QColor(60, 60, 75, 80), 1, Qt::DotLine));
    for (int x = 50; x < TOUCHPAD_WIDTH; x += 50) {
        painter.drawLine(x, 0, x, TOUCHPAD_HEIGHT);
    }
    for (int y = 50; y < TOUCHPAD_HEIGHT; y += 50) {
        painter.drawLine(0, y, TOUCHPAD_WIDTH, y);
    }

    // 中心十字线
    painter.setPen(QPen(QColor(100, 100, 130, 60), 1));
    painter.drawLine(TOUCHPAD_WIDTH / 2, 0, TOUCHPAD_WIDTH / 2, TOUCHPAD_HEIGHT);
    painter.drawLine(0, TOUCHPAD_HEIGHT / 2, TOUCHPAD_WIDTH, TOUCHPAD_HEIGHT / 2);

    // 底部提示
    painter.setPen(QColor(150, 150, 170));
    QFont font("Segoe UI", 9);
    painter.setFont(font);

    QString infoText;
    if (m_handshakeDone) {
        infoText = QString("Touchpad %1x%2  →  Device %3x%4")
                       .arg(TOUCHPAD_WIDTH).arg(TOUCHPAD_HEIGHT)
                       .arg(m_deviceWidth).arg(m_deviceHeight);
    } else {
        infoText = QString("Touchpad %1x%2  |  Waiting for device...")
                       .arg(TOUCHPAD_WIDTH).arg(TOUCHPAD_HEIGHT);
    }
    painter.drawText(QRect(0, TOUCHPAD_HEIGHT - 30, TOUCHPAD_WIDTH, 24),
                     Qt::AlignCenter, infoText);
}

// ---- 鼠标事件 ----
void RemoteMouseWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)   m_buttonsState |= 1;
    if (event->button() == Qt::RightButton)  m_buttonsState |= 2;
    if (event->button() == Qt::MidButton)    m_buttonsState |= 4;

    int32_t devX, devY;
    mapToDevice(event->pos(), devX, devY);
    sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));

    QWidget::mousePressEvent(event);
}

void RemoteMouseWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)   m_buttonsState &= ~1u;
    if (event->button() == Qt::RightButton)  m_buttonsState &= ~2u;
    if (event->button() == Qt::MidButton)    m_buttonsState &= ~4u;

    int32_t devX, devY;
    mapToDevice(event->pos(), devX, devY);
    sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));

    QWidget::mouseReleaseEvent(event);
}

void RemoteMouseWidget::mouseMoveEvent(QMouseEvent *event)
{
    int32_t devX, devY;
    mapToDevice(event->pos(), devX, devY);
    sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));

    QWidget::mouseMoveEvent(event);
}
