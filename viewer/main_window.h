#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QString>

class QAction;
class QLabel;
class QScrollArea;
class ImageView;

using namespace std;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();

    // opens PIX or any image format supported by Qt and shows it
    bool loadFile(const QString &path);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void open();
    void saveAs();
    void imageToPix();
    void pixToImage();
    void zoomIn();
    void zoomOut();
    void actualSize();
    void fitToWindow();
    void about();
    void saveTypeSelected(const QString &filter);

private:
    void createActions();
    void updateActions();
    void updateStatus();
    void setZoom(double zoom);
    void applyFit();
    void showImage(const QImage &newImage, const QString &path, qint64 fileSize);
    void showError(const QString &title, const QString &message);

    bool readImage(const QString &path, QImage &result, QString &error);
    bool readPix(const QString &path, QImage &result, QString &error);
    bool writeImage(const QString &path, const QImage &source, QString &error);
    bool writePix(const QString &path, const QImage &source, QString &error);

    QString askSourceFile(const QString &title, const QString &filter);
    QString askTargetFile(const QString &title, const QString &suggestedName, const QString &filter,
                          QString *chosenFilter = nullptr);
    QString pixFilter(int algorithm) const;
    void choosePixAlgorithm(const QString &filter);
    QString openFilter() const;
    QString saveFilter() const;

    ImageView *view;
    QScrollArea *scrollArea;
    QLabel *infoLabel;
    QLabel *zoomLabel;

    QImage image;
    QString currentPath;
    qint64 currentFileSize;
    QString lastDir;

    QAction *saveAsAct;
    QAction *zoomInAct;
    QAction *zoomOutAct;
    QAction *actualSizeAct;
    QAction *fitAct;
    QAction *algorithm1Act;
    QAction *algorithm2Act;
};

#endif
