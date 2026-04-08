// remote_mouse_widget.cpp
#include "remote_mouse_widget.h"
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QRegularExpression>
#include <QRegularExpressionValidator>

RemoteMouseWidget::RemoteMouseWidget(QWidget *parent)
    : QMainWindow(parent)
    , m_tcpSocket(new QTcpSocket(this))
    , m_buttonsState(0)
    , m_ipInput(nullptr)
    , m_toggleBtn(nullptr)
    , m_statusIndicator(nullptr)
    , m_statusLabel(nullptr)
    , m_deviceInfoLabel(nullptr)
    , m_touchpadArea(nullptr)
    , m_centralWidget(nullptr)
    , m_handshakeDone(false)
    , m_deviceWidth(0)
    , m_deviceHeight(0)
    , m_isConnected(false)
{
    setupUI();
    setupStatusBar();
    applyStyles();

    // TCP信号连接
    connect(m_tcpSocket, &QTcpSocket::connected,
            this, &RemoteMouseWidget::onTcpConnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead,
            this, &RemoteMouseWidget::onTcpReadyRead);
    connect(m_tcpSocket, &QTcpSocket::disconnected,
            this, &RemoteMouseWidget::onTcpDisconnected);
    connect(m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            [this](QAbstractSocket::SocketError socketError){
        qDebug() << "TCP Error:" << socketError;
        updateStatusLabel(tr("连接失败"), QColor(220, 80, 80));
        setConnectionState(false);
    });

    m_tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
}

RemoteMouseWidget::~RemoteMouseWidget()
{
    if (m_tcpSocket->state() == QAbstractSocket::ConnectedState) {
        m_tcpSocket->disconnectFromHost();
    }
}

void RemoteMouseWidget::setupUI()
{
    setWindowTitle(tr("i.MX8MP 远程鼠标"));
    setFixedSize(440, 380);

    // 创建中央部件
    m_centralWidget = new QWidget(this);
    m_centralWidget->setObjectName("centralWidget");
    setCentralWidget(m_centralWidget);

    // 主布局 - 垂直布局，触摸板居中
    QVBoxLayout *mainLayout = new QVBoxLayout(m_centralWidget);
    //mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(0);

    // 触摸板区域
    m_touchpadArea = new TouchpadArea(m_centralWidget);
    m_touchpadArea->setFixedSize(TOUCHPAD_WIDTH, TOUCHPAD_HEIGHT);

    // 触摸板阴影效果
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(24);
    shadow->setColor(QColor(0, 0, 0, 50));
    shadow->setOffset(0, 6);
    m_touchpadArea->setGraphicsEffect(shadow);

    // 连接触摸板的信号
    connect(m_touchpadArea, &TouchpadArea::mousePressed,
            [this](const QPoint &pos, Qt::MouseButton button) {
        if (button == Qt::LeftButton)   m_buttonsState |= 1;
        if (button == Qt::RightButton)  m_buttonsState |= 2;
        if (button == Qt::MidButton)    m_buttonsState |= 4;

        int32_t devX, devY;
        mapToDevice(pos, devX, devY);
        sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));
    });

    connect(m_touchpadArea, &TouchpadArea::mouseReleased,
            [this](const QPoint &pos, Qt::MouseButton button) {
        if (button == Qt::LeftButton)   m_buttonsState &= ~1u;
        if (button == Qt::RightButton)  m_buttonsState &= ~2u;
        if (button == Qt::MidButton)    m_buttonsState &= ~4u;

        int32_t devX, devY;
        mapToDevice(pos, devX, devY);
        sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));
    });

    connect(m_touchpadArea, &TouchpadArea::mouseMoved,
            [this](const QPoint &pos) {
        int32_t devX, devY;
        mapToDevice(pos, devX, devY);
        sendFrame(devX, devY, static_cast<int32_t>(m_buttonsState));
    });

    // 添加触摸板到布局（居中）
    mainLayout->addStretch();
    mainLayout->addWidget(m_touchpadArea, 0, Qt::AlignCenter);
    mainLayout->addStretch();
}

void RemoteMouseWidget::setupStatusBar()
{
    QStatusBar *statusBar = this->statusBar();
    statusBar->setFixedHeight(48);
    statusBar->setSizeGripEnabled(false);

    // ===== 创建主容器 =====
    QWidget *mainContainer = new QWidget(statusBar);
    QHBoxLayout *mainLayout = new QHBoxLayout(mainContainer);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ===== 左侧控制区域 =====
    QWidget *controlWidget = new QWidget(mainContainer);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlWidget);
    controlLayout->setContentsMargins(16, 0, 12, 0);
    controlLayout->setSpacing(10);

    // IP标签
    QLabel *ipLabel = new QLabel(tr("设备IP:"), controlWidget);
    ipLabel->setStyleSheet("color: #8890a0; font-size: 12px; font-weight: 500;");

    // IP输入框
    m_ipInput = new QLineEdit(controlWidget);
    m_ipInput->setPlaceholderText(tr("例如: 192.168.1.100"));
    m_ipInput->setText("127.0.0.1");
    m_ipInput->setFixedWidth(120);

    // IP验证器
    QRegularExpression ipRegex("^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$");
    QRegularExpressionValidator *ipValidator = new QRegularExpressionValidator(ipRegex, this);
    m_ipInput->setValidator(ipValidator);

    // 连接/断开复用按钮
    m_toggleBtn = new QPushButton(tr("连接"), controlWidget);
    m_toggleBtn->setFixedWidth(60);
    m_toggleBtn->setCursor(Qt::PointingHandCursor);
    connect(m_toggleBtn, &QPushButton::clicked, this, &RemoteMouseWidget::onToggleButtonClicked);

    controlLayout->addWidget(ipLabel);
    controlLayout->addWidget(m_ipInput);
    controlLayout->addWidget(m_toggleBtn);

    // ===== 右侧状态区域 =====
    QWidget *statusWidget = new QWidget(mainContainer);
    QHBoxLayout *statusLayout = new QHBoxLayout(statusWidget);
    statusLayout->setContentsMargins(12, 0, 16, 0);
    statusLayout->setSpacing(10);

    // 分隔线
    QFrame *separator = new QFrame(statusWidget);
    separator->setFrameShape(QFrame::VLine);
    separator->setStyleSheet("color: #3a404d;");
    separator->setFixedHeight(20);

    // 状态指示器（圆点）
    m_statusIndicator = new QLabel(statusWidget);
    m_statusIndicator->setFixedSize(8, 8);
    m_statusIndicator->setStyleSheet("background-color: #8890a0; border-radius: 4px;");

    // 状态文字
    m_statusLabel = new QLabel(tr("就绪"), statusWidget);
    m_statusLabel->setStyleSheet("color: #8890a0; font-size: 12px;");

    // 设备信息（分辨率）
    m_deviceInfoLabel = new QLabel("", statusWidget);
    m_deviceInfoLabel->setStyleSheet("color: #667080; font-size: 11px;");
    m_deviceInfoLabel->setVisible(false);

    statusLayout->addWidget(separator);
    statusLayout->addWidget(m_statusIndicator);
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addWidget(m_deviceInfoLabel);

    // ===== 将两个区域添加到主布局 =====
    mainLayout->addWidget(controlWidget, 0, Qt::AlignLeft | Qt::AlignVCenter);
    mainLayout->addStretch();
    mainLayout->addWidget(statusWidget, 0, Qt::AlignRight | Qt::AlignVCenter);

    // 添加到 StatusBar
    statusBar->addWidget(mainContainer, 1);
}

void RemoteMouseWidget::applyStyles()
{
    // 主窗口背景
    setStyleSheet(R"(
        RemoteMouseWidget {
            background-color: #1a1d24;
        }
        RemoteMouseWidget #centralWidget {
            background-color: #1a1d24;
        }
        QMainWindow::separator {
            background: #3a404d;
            width: 1px;
        }
    )");

    // StatusBar 样式
    statusBar()->setStyleSheet(R"(
        QStatusBar {
            background-color: #22262d;
            border-top: 1px solid #3a404d;
        }
        QStatusBar::item {
            border: none;
        }
    )");

    // IP输入框样式
    m_ipInput->setStyleSheet(R"(
        QLineEdit {
            background-color: #1a1d24;
            border: 1px solid #3a404d;
            border-radius: 6px;
            color: #e8eaf0;
            padding: 6px 10px;
            font-size: 12px;
            selection-background-color: #4a90d9;
        }
        QLineEdit:focus {
            border-color: #4a90d9;
        }
        QLineEdit::placeholder {
            color: #667080;
        }
    )");

    updateToggleButtonStyle(false);

    // 触摸板区域样式
    m_touchpadArea->setStyleSheet(R"(
        TouchpadArea {
            background-color: #252a32;
            border: 1px solid #3a404d;
            border-radius: 12px;
        }
    )");
}

void RemoteMouseWidget::updateToggleButtonStyle(bool connected)
{
    if (connected) {
        // 断开按钮样式（红色）
        m_toggleBtn->setStyleSheet(R"(
            QPushButton {
                background-color: #8a3d3d;
                border: none;
                border-radius: 6px;
                color: #ffffff;
                padding: 6px 14px;
                font-size: 12px;
                font-weight: 500;
            }
            QPushButton:hover {
                background-color: #9a4545;
            }
            QPushButton:pressed {
                background-color: #7a3535;
            }
        )");
        m_toggleBtn->setText(tr("断开"));
    } else {
        // 连接按钮样式（绿色）
        m_toggleBtn->setStyleSheet(R"(
            QPushButton {
                background-color: #2d8a4e;
                border: none;
                border-radius: 6px;
                color: #ffffff;
                padding: 6px 14px;
                font-size: 12px;
                font-weight: 500;
            }
            QPushButton:hover {
                background-color: #359a58;
            }
            QPushButton:pressed {
                background-color: #267a42;
            }
        )");
        m_toggleBtn->setText(tr("连接"));
    }
}

void RemoteMouseWidget::onToggleButtonClicked()
{
    if (m_isConnected) {
        // 当前已连接，执行断开
        disconnectFromServer();
    } else {
        // 当前未连接，执行连接
        QString ip = m_ipInput->text().trimmed();
        if (ip.isEmpty()) {
            updateStatusLabel(tr("请输入IP地址"), QColor(220, 150, 60));
            return;
        }
        connectToServer(ip);
    }
}

void RemoteMouseWidget::connectToServer(const QString &ip)
{
    m_handshakeDone = false;
    m_handshakeBuffer.clear();
    m_deviceWidth = 0;
    m_deviceHeight = 0;
    m_deviceInfoLabel->setVisible(false);
    updateStatusLabel(tr("连接中..."), QColor(100, 160, 220));
    m_tcpSocket->connectToHost(ip, SERVER_PORT);
}

void RemoteMouseWidget::disconnectFromServer()
{
    if (m_tcpSocket->state() == QAbstractSocket::ConnectedState) {
        m_tcpSocket->disconnectFromHost();
    }
}

void RemoteMouseWidget::onTcpConnected()
{
    qDebug() << "TCP connected, waiting for handshake...";
    updateStatusLabel(tr("握手..."), QColor(220, 180, 60));
}

void RemoteMouseWidget::onTcpReadyRead()
{
    if (!m_handshakeDone) {
        m_handshakeBuffer.append(m_tcpSocket->readAll());
        processHandshake();
    } else {
        m_tcpSocket->readAll();
    }
}

void RemoteMouseWidget::processHandshake()
{
    const int HANDSHAKE_SIZE = 2 * sizeof(int32_t);
    if (m_handshakeBuffer.size() < HANDSHAKE_SIZE) {
        return;
    }

    const int32_t *data = reinterpret_cast<const int32_t*>(m_handshakeBuffer.constData());
    m_deviceWidth  = data[0];
    m_deviceHeight = data[1];

    if (m_deviceWidth <= 0 || m_deviceHeight <= 0) {
        qDebug() << "Invalid resolution from server:" << m_deviceWidth << "x" << m_deviceHeight;
        updateStatusLabel(tr("分辨率无效"), QColor(220, 80, 80));
        m_tcpSocket->disconnectFromHost();
        return;
    }

    m_handshakeDone = true;
    setConnectionState(true);
    qDebug() << "Handshake OK! Device resolution:" << m_deviceWidth << "x" << m_deviceHeight;

    // 显示设备信息
    m_deviceInfoLabel->setText(QString("|  %1×%2").arg(m_deviceWidth).arg(m_deviceHeight));
    m_deviceInfoLabel->setVisible(true);

    updateStatusLabel(tr("已连接"), QColor(80, 200, 120));
    m_touchpadArea->setConnected(true);
    m_touchpadArea->update();

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
    m_deviceInfoLabel->setVisible(false);
    setConnectionState(false);
    updateStatusLabel(tr("已断开"), QColor(160, 160, 170));
    m_touchpadArea->setConnected(false);
    m_touchpadArea->update();
}

void RemoteMouseWidget::setConnectionState(bool connected)
{
    m_isConnected = connected;
    m_ipInput->setEnabled(!connected);
    updateToggleButtonStyle(connected);
}

void RemoteMouseWidget::updateStatusLabel(const QString &text, const QColor &color)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 500;").arg(color.name()));
    m_statusIndicator->setStyleSheet(QString("background-color: %1; border-radius: 4px;").arg(color.name()));
}

void RemoteMouseWidget::mapToDevice(const QPoint &widgetPos, int32_t &devX, int32_t &devY)
{
    devX = static_cast<int32_t>(
        static_cast<qint64>(widgetPos.x()) * (m_deviceWidth - 1) / (TOUCHPAD_WIDTH - 1));
    devY = static_cast<int32_t>(
        static_cast<qint64>(widgetPos.y()) * (m_deviceHeight - 1) / (TOUCHPAD_HEIGHT - 1));

    if (devX < 0) devX = 0;
    if (devX >= m_deviceWidth) devX = m_deviceWidth - 1;
    if (devY < 0) devY = 0;
    if (devY >= m_deviceHeight) devY = m_deviceHeight - 1;
}

void RemoteMouseWidget::sendFrame(int32_t abs_x, int32_t abs_y, int32_t buttons)
{
    if (!m_handshakeDone) return;
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) return;

    int32_t frame[3] = { abs_x, abs_y, buttons };
    m_tcpSocket->write(reinterpret_cast<const char*>(frame), sizeof(frame));
    m_tcpSocket->flush();
}

// ===== TouchpadArea 实现 =====

TouchpadArea::TouchpadArea(QWidget *parent)
    : QWidget(parent)
    , m_connected(false)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
}

void TouchpadArea::setConnected(bool connected)
{
    m_connected = connected;
    update();
}

void TouchpadArea::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    QRect r = rect();

    // 背景渐变
    QLinearGradient bgGrad(0, 0, 0, r.height());
    bgGrad.setColorAt(0.0, QColor(37, 42, 50));
    bgGrad.setColorAt(1.0, QColor(30, 35, 42));
    painter.fillRect(r, bgGrad);

    // 边框
    painter.setPen(QPen(QColor(58, 64, 77), 1));
    painter.drawRoundedRect(r.adjusted(1, 1, -1, -1), 11, 11);

    // 网格线
    painter.setPen(QPen(QColor(50, 56, 68, 100), 1, Qt::DotLine));
    for (int x = 50; x < width(); x += 50) {
        painter.drawLine(x, 10, x, height() - 10);
    }
    for (int y = 50; y < height(); y += 50) {
        painter.drawLine(10, y, width() - 10, y);
    }

    // 中心十字线
    painter.setPen(QPen(QColor(80, 90, 110, 80), 1));
    painter.drawLine(width() / 2, 20, width() / 2, height() - 20);
    painter.drawLine(20, height() / 2, width() - 20, height() / 2);

    // 中心点
    painter.setBrush(QColor(100, 110, 130, 60));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(width()/2, height()/2), 6, 6);

    // 如果已连接，显示状态
    if (m_connected) {
        painter.setPen(QColor(80, 200, 120, 200));
        QFont font("Segoe UI", 10, QFont::Medium);
        painter.setFont(font);
        painter.drawText(r.adjusted(0, 15, 0, 0), Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8("● 已连接"));
    }
}

void TouchpadArea::mousePressEvent(QMouseEvent *event)
{
    emit mousePressed(event->pos(), event->button());
}

void TouchpadArea::mouseReleaseEvent(QMouseEvent *event)
{
    emit mouseReleased(event->pos(), event->button());
}

void TouchpadArea::mouseMoveEvent(QMouseEvent *event)
{
    emit mouseMoved(event->pos());
}
