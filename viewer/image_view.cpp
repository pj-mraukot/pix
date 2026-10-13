#include <QPainter>
#include <QPaintEvent>
#include "image_view.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
ImageView::ImageView(QWidget *parent) : QWidget(parent), zoomFactor(1.0)
{
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::setImage(const QImage &image)
{
    pixmap = QPixmap::fromImage(image);
    resize(sizeHint());
    update();
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::clear()
{
    pixmap = QPixmap();
    resize(sizeHint());
    update();
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::setZoom(double zoom)
{
    zoomFactor = zoom;
    resize(sizeHint());
    update();
}

//----------------------------------------------------------------------------------------------------------------------
QSize ImageView::sizeHint() const
{
    if (pixmap.isNull())
        return QSize(0, 0);
    return QSize(qMax(1, qRound(pixmap.width() * zoomFactor)), qMax(1, qRound(pixmap.height() * zoomFactor)));
}

//----------------------------------------------------------------------------------------------------------------------
void ImageView::paintEvent(QPaintEvent *event)
{
    if (pixmap.isNull())
        return;
    QPainter painter(this);
    painter.setClipRect(event->rect());
    // smooth when shrinking, sharp pixels when magnifying
    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoomFactor < 1.0);
    painter.drawPixmap(rect(), pixmap);
}
