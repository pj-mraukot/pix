#ifndef IMAGE_VIEW_H
#define IMAGE_VIEW_H

#include <QWidget>
#include <QPixmap>
#include <QImage>

using namespace std;

// shows an image at a given zoom; its size is the zoomed image size (lives in a QScrollArea)
class ImageView : public QWidget
{
    Q_OBJECT

public:
    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void clear();
    void setZoom(double zoom);

    bool hasImage() const { return !pixmap.isNull(); }
    QSize imageSize() const { return pixmap.size(); }
    double zoom() const { return zoomFactor; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap pixmap;
    double zoomFactor;
};

#endif
