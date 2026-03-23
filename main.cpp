#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSharedMemory>
#include <QSystemSemaphore>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstring>

namespace {
constexpr int kBufferSize = 256;
const char *kSharedMemoryKey = "Qt5.CrossProcessDemo.SharedMemory";
const char *kSystemSemaphoreKey = "Qt5.CrossProcessDemo.Semaphore";
const char *kServerNameA = "Qt5CrossProcessDemoServerA";
const char *kServerNameB = "Qt5CrossProcessDemoServerB";

struct SharedState {
    qint32 revision;
    qint32 semaphoreCounter;
    qint64 lastWriteMs;
    char owner[32];
    char sharedMessage[kBufferSize];
    char semaphoreNote[kBufferSize];
};

QString nowText() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
}

QString trimmedToBuffer(const QString &text, int size) {
    QByteArray bytes = text.toUtf8();
    if (bytes.size() >= size) {
        bytes.truncate(size - 1);
    }
    return QString::fromUtf8(bytes);
}

void writeCString(char *target, int size, const QString &text) {
    std::memset(target, 0, static_cast<size_t>(size));
    const QByteArray bytes = trimmedToBuffer(text, size).toUtf8();
    std::memcpy(target, bytes.constData(), static_cast<size_t>(bytes.size()));
}

QString readCString(const char *source) {
    return QString::fromUtf8(source);
}
}

class DemoWindow : public QWidget {
    Q_OBJECT
public:
    explicit DemoWindow(const QString &instanceRole, QWidget *parent = nullptr)
        : QWidget(parent),
          m_role(instanceRole.toUpper() == "B" ? "B" : "A"),
          m_peerRole(m_role == "A" ? "B" : "A"),
          m_sharedMemory(QString::fromUtf8(kSharedMemoryKey)),
          m_semaphore(QString::fromUtf8(kSystemSemaphoreKey), 1),
          m_server(new QLocalServer(this)),
          m_peerProcess(new QProcess(this)),
          m_pollTimer(new QTimer(this)) {
        setWindowTitle(QStringLiteral("Qt5 跨进程通信验证 Demo - 实例 %1").arg(m_role));
        resize(980, 780);

        buildUi();
        initSharedMemory();
        initLocalServer();
        initProcessHandling();
        initPolling();
        refreshSharedState(true);
        updatePeerStatus();
    }

    ~DemoWindow() override {
        if (m_server->isListening()) {
            m_server->close();
        }
    }

private slots:
    void startPeerInstance() {
        if (m_peerProcess->state() != QProcess::NotRunning) {
            appendLog(QStringLiteral("QProcess"), QStringLiteral("对端启动命令已发起，当前进程仍在运行中。"));
            return;
        }

        const QString program = QCoreApplication::applicationFilePath();
        QStringList arguments;
        arguments << "--instance" << m_peerRole;

        m_peerProcess->setProgram(program);
        m_peerProcess->setArguments(arguments);
        m_peerProcess->setWorkingDirectory(QDir::currentPath());
        m_peerProcess->start();

        if (!m_peerProcess->waitForStarted(3000)) {
            appendLog(QStringLiteral("QProcess"),
                      QStringLiteral("启动对端实例失败：%1").arg(m_peerProcess->errorString()));
            updatePeerStatus();
            return;
        }

        appendLog(QStringLiteral("QProcess"),
                  QStringLiteral("已通过 QProcess 启动实例 %1，PID=%2，程序=%3 %4")
                      .arg(m_peerRole)
                      .arg(m_peerProcess->processId())
                      .arg(program, arguments.join(' ')));
        updatePeerStatus();
    }

    void stopPeerInstance() {
        if (m_peerProcess->state() == QProcess::NotRunning) {
            appendLog(QStringLiteral("QProcess"), QStringLiteral("当前没有由本实例托管的对端进程。"));
            updatePeerStatus();
            return;
        }

        m_peerProcess->terminate();
        if (!m_peerProcess->waitForFinished(2000)) {
            m_peerProcess->kill();
            m_peerProcess->waitForFinished(2000);
        }

        appendLog(QStringLiteral("QProcess"), QStringLiteral("已请求结束本实例启动的对端进程。"));
        updatePeerStatus();
    }

    void sendLocalMessage() {
        const QString message = m_localSocketEdit->text().trimmed();
        if (message.isEmpty()) {
            appendLog(QStringLiteral("QLocalSocket"), QStringLiteral("请输入要发送给对端的本地 socket 消息。"));
            return;
        }

        auto *socket = new QLocalSocket(this);
        connect(socket, &QLocalSocket::connected, this, [this, socket, message]() {
            const QString payload = QStringLiteral("[%1] 实例 %2 发送：%3")
                                        .arg(nowText(), m_role, message);
            socket->write(payload.toUtf8());
            socket->flush();
            socket->disconnectFromServer();
            appendLog(QStringLiteral("QLocalSocket"),
                      QStringLiteral("已发送到 %1：%2").arg(m_peerRole, payload));
        });
        connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError) {
            appendLog(QStringLiteral("QLocalSocket"),
                      QStringLiteral("连接实例 %1 的本地服务失败：%2")
                          .arg(m_peerRole, socket->errorString()));
            socket->deleteLater();
            updatePeerStatus();
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);

        socket->connectToServer(peerServerName());
    }

    void onNewLocalConnection() {
        while (m_server->hasPendingConnections()) {
            QLocalSocket *socket = m_server->nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
                const QString payload = QString::fromUtf8(socket->readAll());
                m_localSocketReceivedLabel->setText(payload);
                appendLog(QStringLiteral("QLocalSocket"),
                          QStringLiteral("收到来自实例 %1 的消息：%2").arg(m_peerRole, payload));
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        }
    }

    void writeSharedMessage() {
        const QString message = m_sharedMemoryEdit->text().trimmed();
        if (message.isEmpty()) {
            appendLog(QStringLiteral("QSharedMemory"), QStringLiteral("请输入要写入共享内存的内容。"));
            return;
        }

        mutateSharedState([&](SharedState &state) {
            ++state.revision;
            state.lastWriteMs = QDateTime::currentMSecsSinceEpoch();
            writeCString(state.owner, sizeof(state.owner), QStringLiteral("实例 %1").arg(m_role));
            writeCString(state.sharedMessage, sizeof(state.sharedMessage), message);
        });

        appendLog(QStringLiteral("QSharedMemory"),
                  QStringLiteral("已写入共享内存，当前显示 revision=%1：%2").arg(m_lastSeenRevision).arg(message));
        refreshSharedState(true);
    }

    void incrementSemaphoreCounter() {
        const QString note = m_semaphoreEdit->text().trimmed().isEmpty()
                                 ? QStringLiteral("实例 %1 执行一次受保护的跨进程累加").arg(m_role)
                                 : m_semaphoreEdit->text().trimmed();

        int before = 0;
        int after = 0;
        mutateSharedState([&](SharedState &state) {
            before = state.semaphoreCounter;
            ++state.semaphoreCounter;
            after = state.semaphoreCounter;
            ++state.revision;
            state.lastWriteMs = QDateTime::currentMSecsSinceEpoch();
            writeCString(state.owner, sizeof(state.owner), QStringLiteral("实例 %1").arg(m_role));
            writeCString(state.semaphoreNote, sizeof(state.semaphoreNote), note);
        });

        appendLog(QStringLiteral("QSystemSemaphore"),
                  QStringLiteral("进入临界区并完成 +1：%1 -> %2，说明：%3").arg(before).arg(after).arg(note));
        refreshSharedState(true);
    }

    void refreshSharedState(bool forceLog = false) {
        SharedState state = readSharedState();
        const QString owner = readCString(state.owner);
        const QString sharedMessage = readCString(state.sharedMessage);
        const QString semaphoreNote = readCString(state.semaphoreNote);
        const QString timeText = state.lastWriteMs > 0
                                     ? QDateTime::fromMSecsSinceEpoch(state.lastWriteMs)
                                           .toString("yyyy-MM-dd hh:mm:ss.zzz")
                                     : QStringLiteral("- ");

        m_sharedRevisionLabel->setText(QString::number(state.revision));
        m_sharedOwnerLabel->setText(owner.isEmpty() ? QStringLiteral("- ") : owner);
        m_sharedMessageLabel->setText(sharedMessage.isEmpty() ? QStringLiteral("- ") : sharedMessage);
        m_sharedTimeLabel->setText(timeText);
        m_semaphoreCounterLabel->setText(QString::number(state.semaphoreCounter));
        m_semaphoreNoteLabel->setText(semaphoreNote.isEmpty() ? QStringLiteral("- ") : semaphoreNote);

        if (forceLog || state.revision != m_lastSeenRevision) {
            appendLog(QStringLiteral("State"),
                      QStringLiteral("检测到共享状态 revision=%1，owner=%2，message=%3，counter=%4")
                          .arg(state.revision)
                          .arg(owner.isEmpty() ? QStringLiteral("- ") : owner)
                          .arg(sharedMessage.isEmpty() ? QStringLiteral("- ") : sharedMessage)
                          .arg(state.semaphoreCounter));
        }

        m_lastSeenRevision = state.revision;
        updatePeerStatus();
    }

    void updatePeerStatus() {
        const bool processRunning = m_peerProcess->state() != QProcess::NotRunning;
        const QString processText = processRunning
                                        ? QStringLiteral("本实例已通过 QProcess 启动对端，PID=%1")
                                              .arg(m_peerProcess->processId())
                                        : QStringLiteral("当前未托管对端进程，可点击“启动对端实例”。");
        m_processStatusLabel->setText(processText);

        const QString socketStatus = QFile::exists(serverSocketPath(peerServerName()))
                                         ? QStringLiteral("实例 %1 的 QLocalServer 已可见").arg(m_peerRole)
                                         : QStringLiteral("尚未检测到实例 %1 的 QLocalServer").arg(m_peerRole);
        m_localSocketStatusLabel->setText(socketStatus);
    }

    void handlePeerProcessStateChanged(QProcess::ProcessState) {
        updatePeerStatus();
    }

    void handlePeerProcessError(QProcess::ProcessError error) {
        Q_UNUSED(error)
        appendLog(QStringLiteral("QProcess"),
                  QStringLiteral("对端进程发生错误：%1").arg(m_peerProcess->errorString()));
        updatePeerStatus();
    }

private:
    QString localServerName() const {
        return m_role == "A" ? QString::fromUtf8(kServerNameA) : QString::fromUtf8(kServerNameB);
    }

    QString peerServerName() const {
        return m_peerRole == "A" ? QString::fromUtf8(kServerNameA) : QString::fromUtf8(kServerNameB);
    }

    QString serverSocketPath(const QString &serverName) const {
#ifdef Q_OS_WIN
        return serverName;
#else
        return QDir::temp().absoluteFilePath(serverName);
#endif
    }

    void buildUi() {
        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(12, 12, 12, 12);
        rootLayout->setSpacing(12);

        auto *summaryGroup = new QGroupBox(QStringLiteral("实例信息 / 验证方式"), this);
        auto *summaryLayout = new QFormLayout(summaryGroup);
        m_instanceLabel = new QLabel(QStringLiteral("当前实例：%1；对端实例：%2").arg(m_role, m_peerRole), this);
        summaryLayout->addRow(QStringLiteral("角色"), m_instanceLabel);
        summaryLayout->addRow(new QLabel(
            QStringLiteral("同一套代码可启动两个实例：每个实例都提供 QLocalServer，轮询共享内存；"
                           "QSystemSemaphore 用于保护共享状态修改；QProcess 可直接拉起对端实例。"),
            this));
        rootLayout->addWidget(summaryGroup);

        auto *processGroup = new QGroupBox(QStringLiteral("1. QProcess：启动/结束对端实例"), this);
        auto *processLayout = new QVBoxLayout(processGroup);
        auto *processButtonLayout = new QHBoxLayout();
        m_startPeerButton = new QPushButton(QStringLiteral("启动对端实例 %1").arg(m_peerRole), this);
        m_stopPeerButton = new QPushButton(QStringLiteral("结束我启动的对端实例"), this);
        processButtonLayout->addWidget(m_startPeerButton);
        processButtonLayout->addWidget(m_stopPeerButton);
        m_processStatusLabel = new QLabel(this);
        m_processStatusLabel->setWordWrap(true);
        processLayout->addLayout(processButtonLayout);
        processLayout->addWidget(m_processStatusLabel);
        rootLayout->addWidget(processGroup);

        auto *socketGroup = new QGroupBox(QStringLiteral("2. QLocalSocket：点对点消息发送"), this);
        auto *socketLayout = new QGridLayout(socketGroup);
        m_localSocketEdit = new QLineEdit(this);
        m_localSocketEdit->setPlaceholderText(QStringLiteral("输入要发送给对端实例的 socket 消息"));
        m_sendSocketButton = new QPushButton(QStringLiteral("发送给实例 %1").arg(m_peerRole), this);
        m_localSocketStatusLabel = new QLabel(this);
        m_localSocketReceivedLabel = new QLabel(QStringLiteral("尚未收到消息"), this);
        m_localSocketReceivedLabel->setWordWrap(true);
        socketLayout->addWidget(new QLabel(QStringLiteral("发送内容"), this), 0, 0);
        socketLayout->addWidget(m_localSocketEdit, 0, 1);
        socketLayout->addWidget(m_sendSocketButton, 0, 2);
        socketLayout->addWidget(new QLabel(QStringLiteral("服务状态"), this), 1, 0);
        socketLayout->addWidget(m_localSocketStatusLabel, 1, 1, 1, 2);
        socketLayout->addWidget(new QLabel(QStringLiteral("最近接收"), this), 2, 0);
        socketLayout->addWidget(m_localSocketReceivedLabel, 2, 1, 1, 2);
        rootLayout->addWidget(socketGroup);

        auto *sharedGroup = new QGroupBox(QStringLiteral("3. QSharedMemory：共享字符串状态"), this);
        auto *sharedLayout = new QGridLayout(sharedGroup);
        m_sharedMemoryEdit = new QLineEdit(this);
        m_sharedMemoryEdit->setPlaceholderText(QStringLiteral("输入要写入共享内存的文本，两个实例都会看到"));
        m_writeSharedButton = new QPushButton(QStringLiteral("写入共享内存"), this);
        m_sharedRevisionLabel = new QLabel(this);
        m_sharedOwnerLabel = new QLabel(this);
        m_sharedMessageLabel = new QLabel(this);
        m_sharedTimeLabel = new QLabel(this);
        m_sharedMessageLabel->setWordWrap(true);
        sharedLayout->addWidget(new QLabel(QStringLiteral("写入内容"), this), 0, 0);
        sharedLayout->addWidget(m_sharedMemoryEdit, 0, 1);
        sharedLayout->addWidget(m_writeSharedButton, 0, 2);
        sharedLayout->addWidget(new QLabel(QStringLiteral("Revision"), this), 1, 0);
        sharedLayout->addWidget(m_sharedRevisionLabel, 1, 1, 1, 2);
        sharedLayout->addWidget(new QLabel(QStringLiteral("最后写入者"), this), 2, 0);
        sharedLayout->addWidget(m_sharedOwnerLabel, 2, 1, 1, 2);
        sharedLayout->addWidget(new QLabel(QStringLiteral("共享消息"), this), 3, 0);
        sharedLayout->addWidget(m_sharedMessageLabel, 3, 1, 1, 2);
        sharedLayout->addWidget(new QLabel(QStringLiteral("最后更新时间"), this), 4, 0);
        sharedLayout->addWidget(m_sharedTimeLabel, 4, 1, 1, 2);
        rootLayout->addWidget(sharedGroup);

        auto *semaphoreGroup = new QGroupBox(QStringLiteral("4. QSystemSemaphore：受保护的跨进程计数器"), this);
        auto *semaphoreLayout = new QGridLayout(semaphoreGroup);
        m_semaphoreEdit = new QLineEdit(this);
        m_semaphoreEdit->setPlaceholderText(QStringLiteral("输入本次进入临界区的说明（可选）"));
        m_incrementSemaphoreButton = new QPushButton(QStringLiteral("进入临界区并 +1"), this);
        m_semaphoreCounterLabel = new QLabel(this);
        m_semaphoreNoteLabel = new QLabel(this);
        m_semaphoreNoteLabel->setWordWrap(true);
        semaphoreLayout->addWidget(new QLabel(QStringLiteral("操作说明"), this), 0, 0);
        semaphoreLayout->addWidget(m_semaphoreEdit, 0, 1);
        semaphoreLayout->addWidget(m_incrementSemaphoreButton, 0, 2);
        semaphoreLayout->addWidget(new QLabel(QStringLiteral("共享计数器"), this), 1, 0);
        semaphoreLayout->addWidget(m_semaphoreCounterLabel, 1, 1, 1, 2);
        semaphoreLayout->addWidget(new QLabel(QStringLiteral("最近一次说明"), this), 2, 0);
        semaphoreLayout->addWidget(m_semaphoreNoteLabel, 2, 1, 1, 2);
        rootLayout->addWidget(semaphoreGroup);

        auto *logGroup = new QGroupBox(QStringLiteral("运行日志"), this);
        auto *logLayout = new QVBoxLayout(logGroup);
        m_logEdit = new QPlainTextEdit(this);
        m_logEdit->setReadOnly(true);
        logLayout->addWidget(m_logEdit);
        rootLayout->addWidget(logGroup, 1);

        connect(m_startPeerButton, &QPushButton::clicked, this, &DemoWindow::startPeerInstance);
        connect(m_stopPeerButton, &QPushButton::clicked, this, &DemoWindow::stopPeerInstance);
        connect(m_sendSocketButton, &QPushButton::clicked, this, &DemoWindow::sendLocalMessage);
        connect(m_writeSharedButton, &QPushButton::clicked, this, &DemoWindow::writeSharedMessage);
        connect(m_incrementSemaphoreButton, &QPushButton::clicked, this, &DemoWindow::incrementSemaphoreCounter);
    }

    void initSharedMemory() {
        if (m_sharedMemory.attach()) {
            appendLog(QStringLiteral("Init"), QStringLiteral("已连接到现有共享内存。"));
            return;
        }

        if (m_sharedMemory.create(static_cast<int>(sizeof(SharedState)))) {
            mutateSharedState([&](SharedState &state) {
                state.revision = 1;
                state.semaphoreCounter = 0;
                state.lastWriteMs = QDateTime::currentMSecsSinceEpoch();
                writeCString(state.owner, sizeof(state.owner), QStringLiteral("实例 %1 初始化").arg(m_role));
                writeCString(state.sharedMessage, sizeof(state.sharedMessage), QStringLiteral("共享内存已创建，可在任意实例写入文本。"));
                writeCString(state.semaphoreNote, sizeof(state.semaphoreNote), QStringLiteral("等待第一次 QSystemSemaphore 受保护操作。"));
            });
            appendLog(QStringLiteral("Init"), QStringLiteral("已创建新的共享内存。"));
            return;
        }

        appendLog(QStringLiteral("Init"),
                  QStringLiteral("共享内存创建/连接失败：%1").arg(m_sharedMemory.errorString()));
    }

    void initLocalServer() {
        QLocalServer::removeServer(localServerName());
        if (!m_server->listen(localServerName())) {
            appendLog(QStringLiteral("QLocalSocket"),
                      QStringLiteral("本地服务监听失败：%1").arg(m_server->errorString()));
            return;
        }

        connect(m_server, &QLocalServer::newConnection, this, &DemoWindow::onNewLocalConnection);
        appendLog(QStringLiteral("QLocalSocket"),
                  QStringLiteral("实例 %1 已监听本地服务：%2").arg(m_role, localServerName()));
    }

    void initProcessHandling() {
        connect(m_peerProcess, &QProcess::stateChanged, this, &DemoWindow::handlePeerProcessStateChanged);
        connect(m_peerProcess,
                QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
                this,
                &DemoWindow::handlePeerProcessError);
    }

    void initPolling() {
        m_pollTimer->setInterval(500);
        connect(m_pollTimer, &QTimer::timeout, this, [this]() { refreshSharedState(false); });
        m_pollTimer->start();
    }

    SharedState readSharedState() {
        SharedState state{};
        if (!ensureSharedMemoryAttached()) {
            return state;
        }

        m_semaphore.acquire();
        if (m_sharedMemory.lock()) {
            std::memcpy(&state, m_sharedMemory.constData(), sizeof(SharedState));
            m_sharedMemory.unlock();
        }
        m_semaphore.release();
        return state;
    }

    template <typename Func>
    void mutateSharedState(Func func) {
        if (!ensureSharedMemoryAttached()) {
            appendLog(QStringLiteral("Shared"), QStringLiteral("共享内存不可用，无法修改状态。"));
            return;
        }

        m_semaphore.acquire();
        if (m_sharedMemory.lock()) {
            SharedState state{};
            std::memcpy(&state, m_sharedMemory.constData(), sizeof(SharedState));
            func(state);
            std::memcpy(m_sharedMemory.data(), &state, sizeof(SharedState));
            m_sharedMemory.unlock();
        } else {
            appendLog(QStringLiteral("Shared"),
                      QStringLiteral("共享内存加锁失败：%1").arg(m_sharedMemory.errorString()));
        }
        m_semaphore.release();
    }

    bool ensureSharedMemoryAttached() {
        if (m_sharedMemory.isAttached()) {
            return true;
        }
        return m_sharedMemory.attach();
    }

    void appendLog(const QString &module, const QString &message) {
        m_logEdit->appendPlainText(QStringLiteral("[%1] [%2] %3").arg(nowText(), module, message));
    }

private:
    QString m_role;
    QString m_peerRole;
    QSharedMemory m_sharedMemory;
    QSystemSemaphore m_semaphore;
    QLocalServer *m_server;
    QProcess *m_peerProcess;
    QTimer *m_pollTimer;
    int m_lastSeenRevision = -1;

    QLabel *m_instanceLabel = nullptr;
    QPushButton *m_startPeerButton = nullptr;
    QPushButton *m_stopPeerButton = nullptr;
    QLabel *m_processStatusLabel = nullptr;

    QLineEdit *m_localSocketEdit = nullptr;
    QPushButton *m_sendSocketButton = nullptr;
    QLabel *m_localSocketStatusLabel = nullptr;
    QLabel *m_localSocketReceivedLabel = nullptr;

    QLineEdit *m_sharedMemoryEdit = nullptr;
    QPushButton *m_writeSharedButton = nullptr;
    QLabel *m_sharedRevisionLabel = nullptr;
    QLabel *m_sharedOwnerLabel = nullptr;
    QLabel *m_sharedMessageLabel = nullptr;
    QLabel *m_sharedTimeLabel = nullptr;

    QLineEdit *m_semaphoreEdit = nullptr;
    QPushButton *m_incrementSemaphoreButton = nullptr;
    QLabel *m_semaphoreCounterLabel = nullptr;
    QLabel *m_semaphoreNoteLabel = nullptr;

    QPlainTextEdit *m_logEdit = nullptr;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QString role = QStringLiteral("A");
    const QStringList arguments = app.arguments();
    const int index = arguments.indexOf("--instance");
    if (index >= 0 && index + 1 < arguments.size()) {
        role = arguments.at(index + 1).trimmed().toUpper();
    }

    DemoWindow window(role);
    window.show();
    return app.exec();
}
