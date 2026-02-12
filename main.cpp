#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_OHOS
#include <QtOhosExtras>
#endif

class ScreenshotOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ScreenshotOverlay(QScreen *screen, QWidget *parent = nullptr)
        : QWidget(parent), m_screen(screen) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setCursor(Qt::CrossCursor);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_NativeWindow);

#ifdef Q_OS_OHOS
        QtOhosExtras::setShowWindowAsFloatWindowHint(this, true);
#endif

        if (m_screen) {
            m_screenImage = m_screen->grabWindow(0);
            setGeometry(m_screen->geometry());
        }
    }

    void prepareForScreen() {
        if (!m_screen || !windowHandle()) {
            return;
        }
        windowHandle()->setScreen(m_screen);
    }

signals:
    void screenshotReady(const QPixmap &pixmap);
    void canceled();

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        activateWindow();
        setFocus();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.drawPixmap(rect(), m_screenImage);

        QColor mask(0, 0, 0, 120);
        painter.fillRect(rect(), mask);

        if (m_selectionRect.isValid()) {
            painter.drawPixmap(m_selectionRect, m_screenImage, m_selectionRect);
            QPen pen(Qt::red);
            pen.setWidth(2);
            painter.setPen(pen);
            painter.drawRect(m_selectionRect.adjusted(0, 0, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragStart = event->pos();
            m_selectionRect = QRect(m_dragStart, QSize());
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (!m_dragging) {
            return;
        }
        m_selectionRect = QRect(m_dragStart, event->pos()).normalized();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton || !m_dragging) {
            return;
        }

        m_dragging = false;
        m_selectionRect = QRect(m_dragStart, event->pos()).normalized();

        if (m_selectionRect.width() > 2 && m_selectionRect.height() > 2) {
            emit screenshotReady(m_screenImage.copy(m_selectionRect));
        } else {
            emit canceled();
        }
        close();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            emit canceled();
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QScreen *m_screen = nullptr;
    QPixmap m_screenImage;
    bool m_dragging = false;
    QPoint m_dragStart;
    QRect m_selectionRect;
};

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(12);

        m_captureButton = new QPushButton(QStringLiteral("截屏"), this);
        m_captureButton->setFixedHeight(40);

        m_previewLabel = new QLabel(QStringLiteral("截图完成后会展示在这里"), this);
        m_previewLabel->setAlignment(Qt::AlignCenter);
        m_previewLabel->setMinimumHeight(360);
        m_previewLabel->setStyleSheet("QLabel { border: 1px solid #999; background: #f5f5f5; }");

        layout->addWidget(m_captureButton);
        layout->addWidget(m_previewLabel, 1);

        setWindowTitle(QStringLiteral("Qt5 截屏 Demo"));
        resize(760, 560);
        keepInsideOneScreen();

        connect(m_captureButton, &QPushButton::clicked, this, &MainWindow::startCapture);
    }

protected:
    void moveEvent(QMoveEvent *event) override {
        QWidget::moveEvent(event);
        keepInsideOneScreen();
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        keepInsideOneScreen();
        updatePreview();
    }

private:
    void startCapture() {
        if (m_captureActive) {
            return;
        }

        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            return;
        }

        m_captureActive = true;
        hide();

        QTimer::singleShot(120, this, [this, screens]() {
            for (QScreen *screen : screens) {
                auto *overlay = new ScreenshotOverlay(screen);
                overlay->winId();
                overlay->prepareForScreen();

                connect(overlay, &ScreenshotOverlay::screenshotReady, this, [this](const QPixmap &shot) {
                    m_lastShot = shot;
                    updatePreview();
                    closeAllOverlays();
                });
                connect(overlay, &ScreenshotOverlay::canceled, this, [this]() {
                    closeAllOverlays();
                });

                m_overlays.append(overlay);
                overlay->show();
            }
        });
    }

    void closeAllOverlays() {
        if (!m_captureActive) {
            return;
        }

        const auto overlays = m_overlays;
        m_overlays.clear();

        for (ScreenshotOverlay *overlay : overlays) {
            if (overlay) {
                overlay->blockSignals(true);
                overlay->close();
            }
        }

        m_captureActive = false;
        show();
        raise();
        activateWindow();
        keepInsideOneScreen();
    }

    QScreen *screenForWindow() const {
        const QPoint center = mapToGlobal(rect().center());
        if (QScreen *screen = QGuiApplication::screenAt(center)) {
            return screen;
        }
        if (windowHandle() && windowHandle()->screen()) {
            return windowHandle()->screen();
        }
        return QGuiApplication::primaryScreen();
    }

    void keepInsideOneScreen() {
        QScreen *screen = screenForWindow();
        if (!screen) {
            return;
        }

        QRect available = screen->availableGeometry();
        QRect g = geometry();

        if (g.width() > available.width()) {
            g.setWidth(available.width());
        }
        if (g.height() > available.height()) {
            g.setHeight(available.height());
        }

        if (g.left() < available.left()) {
            g.moveLeft(available.left());
        }
        if (g.top() < available.top()) {
            g.moveTop(available.top());
        }
        if (g.right() > available.right()) {
            g.moveRight(available.right());
        }
        if (g.bottom() > available.bottom()) {
            g.moveBottom(available.bottom());
        }

        if (g != geometry()) {
            setGeometry(g);
        }
    }

    void updatePreview() {
        if (m_lastShot.isNull()) {
            m_previewLabel->setText(QStringLiteral("截图失败或已取消"));
            m_previewLabel->setPixmap(QPixmap());
            return;
        }
        m_previewLabel->setText(QString());
        const QPixmap scaled = m_lastShot.scaled(
            m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_previewLabel->setPixmap(scaled);
    }

private:
    QPushButton *m_captureButton = nullptr;
    QLabel *m_previewLabel = nullptr;
    QPixmap m_lastShot;
    QList<ScreenshotOverlay *> m_overlays;
    bool m_captureActive = false;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);

#ifdef Q_OS_OHOS
    QtOhosExtras::QOhosAppContext *appcontext = QtOhosExtras::QOhosAppContext::instance();
    appcontext->requestPermissionFromUserIfNeeded(
        QtOhosExtras::QOhosAppContext::AppPermission::CustomScreenCapture);
#endif

    MainWindow w;
    w.show();

    return app.exec();
}
