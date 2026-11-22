#pragma once

#include <QImage>
#include <QWidget>

class QLabel;
class ImageCanvas;

// A picture from a conversation in its own window (as Signal's): fitted to
// the window, or bigger with the wheel, +/−, a double-click; dragged when
// bigger than the window. Decrypted in memory only: nothing is written to
// disk unless Save As… is chosen. Closed by Esc, and by the window when it
// locks or the message goes.
class ImageViewer : public QWidget
{
    Q_OBJECT

public:
    ImageViewer(const QImage &image, const QString &name, qint64 attachment, QWidget *parent = nullptr);

    qint64 attachment() const { return m_attachment; }
    double zoom() const;  // 1 = the picture's own pixels
    bool isFitted() const; // following the window's size

    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualSize();

signals:
    void saveRequested(qint64 attachment);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void updateZoomLabel();

    qint64 m_attachment;
    ImageCanvas *m_canvas;
    QLabel *m_zoomLabel;
};
