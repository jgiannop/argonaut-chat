#include "ImageViewer.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr double MinZoom = 0.05;
constexpr double MaxZoom = 16.0;
constexpr double Step = 1.25;

} // namespace

// Draws the picture at a zoom, centred plus an offset the mouse drags.
class ImageCanvas : public QWidget
{
    Q_OBJECT

public:
    ImageCanvas(const QImage &image, QWidget *parent)
        : QWidget(parent)
        , m_pixmap(QPixmap::fromImage(image))
    {
        setMouseTracking(true);
        setMinimumSize(200, 150);
    }

    QSize imageSize() const { return m_pixmap.size(); }
    bool fitted() const { return m_fitted; }

    // Large pictures shrink to the window; small ones keep their own size.
    double fitZoom() const
    {
        if (m_pixmap.isNull() || width() <= 0 || height() <= 0)
            return 1.0;
        return std::min({1.0, double(width()) / m_pixmap.width(), double(height()) / m_pixmap.height()});
    }

    double zoom() const { return m_fitted ? fitZoom() : m_zoom; }

    void fit()
    {
        m_fitted = true;
        m_offset = QPointF();
        changed();
    }

    // Zoomed with `at` (a point in this widget) staying under the mouse.
    void setZoom(double zoom, QPointF at = QPointF(-1, -1))
    {
        if (at.x() < 0)
            at = rect().center();
        const double old = this->zoom();
        zoom = std::clamp(zoom, MinZoom, MaxZoom);
        const QPointF center = QPointF(rect().center()) + m_offset;
        const QPointF picturePoint = (at - center) / old;
        m_fitted = false;
        m_zoom = zoom;
        m_offset = at - picturePoint * zoom - QPointF(rect().center());
        changed();
    }

signals:
    void zoomChanged();
    void toggleRequested();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(20, 20, 20));
        if (m_pixmap.isNull())
            return;
        const double z = zoom();
        // Pixels stay sharp from 3x up (to see them), smooth below.
        p.setRenderHint(QPainter::SmoothPixmapTransform, z < 3.0);
        p.drawPixmap(target(), m_pixmap, QRectF(m_pixmap.rect()));
    }

    void resizeEvent(QResizeEvent *) override
    {
        clampOffset();
        if (m_fitted)
            emit zoomChanged(); // the fitting zoom follows the window
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int delta = event->angleDelta().y();
        if (delta == 0)
            return;
        setZoom(zoom() * std::pow(Step, delta / 120.0), event->position());
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && canDrag()) {
            m_dragFrom = event->position();
            setCursor(Qt::ClosedHandCursor);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragFrom) {
            m_offset += event->position() - *m_dragFrom;
            m_dragFrom = event->position();
            clampOffset();
            update();
        } else {
            setCursor(canDrag() ? Qt::OpenHandCursor : Qt::ArrowCursor);
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_dragFrom.reset();
        setCursor(canDrag() ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            emit toggleRequested();
    }

private:
    QRectF target() const
    {
        const double z = zoom();
        const QSizeF size(m_pixmap.width() * z, m_pixmap.height() * z);
        const QPointF center = QPointF(rect().center()) + m_offset;
        return QRectF(center - QPointF(size.width() / 2, size.height() / 2), size);
    }

    bool canDrag() const
    {
        const QRectF t = target();
        return t.width() > width() || t.height() > height();
    }

    // Never dragged off: a picture larger than the window keeps it covered,
    // a smaller one stays in the middle.
    void clampOffset()
    {
        const double z = zoom();
        const double spareX = std::max(0.0, (m_pixmap.width() * z - width()) / 2);
        const double spareY = std::max(0.0, (m_pixmap.height() * z - height()) / 2);
        m_offset.setX(std::clamp(m_offset.x(), -spareX, spareX));
        m_offset.setY(std::clamp(m_offset.y(), -spareY, spareY));
    }

    void changed()
    {
        clampOffset();
        update();
        emit zoomChanged();
    }

    QPixmap m_pixmap;
    bool m_fitted = true;
    double m_zoom = 1.0;
    QPointF m_offset;
    std::optional<QPointF> m_dragFrom;
};

ImageViewer::ImageViewer(const QImage &image, const QString &name, qint64 attachment, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_attachment(attachment)
{
    setObjectName(QStringLiteral("imageViewer"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(name);
    setStyleSheet(QStringLiteral(
        "QWidget#imageViewer, QWidget#imageBar { background: #141414; }"
        "QLabel { color: #e8e8e8; }"
        "QPushButton { color: #e8e8e8; background: #2a2a2a; border: none; border-radius: 6px; padding: 5px 12px; }"
        "QPushButton:hover { background: #3a3a3a; }"));

    m_canvas = new ImageCanvas(image, this);
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("imageBar"));
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(10, 6, 10, 8);
    auto *nameLabel = new QLabel(QStringLiteral("%1  ·  %2 × %3").arg(name).arg(image.width()).arg(image.height()), bar);
    nameLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_zoomLabel = new QLabel(bar);
    m_zoomLabel->setObjectName(QStringLiteral("zoomLabel"));
    m_zoomLabel->setMinimumWidth(48);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    auto button = [this, bar](const QString &text, const QString &tip, const QString &object, auto slot) {
        auto *b = new QPushButton(text, bar);
        b->setObjectName(object);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    barLayout->addWidget(nameLabel, 1);
    barLayout->addWidget(button(QStringLiteral("−"), tr("Zoom out (−)"), QStringLiteral("zoomOutButton"), &ImageViewer::zoomOut));
    barLayout->addWidget(m_zoomLabel);
    barLayout->addWidget(button(QStringLiteral("+"), tr("Zoom in (+)"), QStringLiteral("zoomInButton"), &ImageViewer::zoomIn));
    barLayout->addWidget(button(tr("Fit"), tr("Fit to the window (0)"), QStringLiteral("fitButton"), &ImageViewer::fitToWindow));
    barLayout->addWidget(button(tr("100%"), tr("Actual size (1)"), QStringLiteral("actualSizeButton"), &ImageViewer::actualSize));
    barLayout->addSpacing(8);
    barLayout->addWidget(button(tr("Save As…"), tr("Save a copy (Ctrl+S)"), QStringLiteral("saveImageButton"),
                                [this] { emit saveRequested(m_attachment); }));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_canvas, 1);
    layout->addWidget(bar);

    connect(m_canvas, &ImageCanvas::zoomChanged, this, &ImageViewer::updateZoomLabel);
    connect(m_canvas, &ImageCanvas::toggleRequested, this, [this] {
        // Fitted: its own pixels (twice them for a small one); else fitted again.
        if (isFitted())
            m_canvas->setZoom(zoom() < 1.0 ? 1.0 : 2.0, m_canvas->mapFromGlobal(QCursor::pos()));
        else
            fitToWindow();
    });

    // As large as the picture, within most of the screen.
    QSize size = image.size() + QSize(0, bar->sizeHint().height());
    if (const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen())
        size = size.boundedTo(screen->availableGeometry().size() * 0.85);
    resize(size.expandedTo(QSize(480, 360)));
    updateZoomLabel();
}

double ImageViewer::zoom() const
{
    return m_canvas->zoom();
}

bool ImageViewer::isFitted() const
{
    return m_canvas->fitted();
}

void ImageViewer::zoomIn()
{
    m_canvas->setZoom(zoom() * Step);
}

void ImageViewer::zoomOut()
{
    m_canvas->setZoom(zoom() / Step);
}

void ImageViewer::fitToWindow()
{
    m_canvas->fit();
}

void ImageViewer::actualSize()
{
    m_canvas->setZoom(1.0);
}

void ImageViewer::updateZoomLabel()
{
    m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(zoom() * 100)));
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Save)) {
        emit saveRequested(m_attachment);
        return;
    }
    if (event->matches(QKeySequence::Close)) {
        close();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        return;
    case Qt::Key_Minus:
        zoomOut();
        return;
    case Qt::Key_0:
        fitToWindow();
        return;
    case Qt::Key_1:
        actualSize();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

#include "ImageViewer.moc"
