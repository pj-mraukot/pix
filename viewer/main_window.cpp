#include <string.h>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScrollArea>
#include <QStatusBar>
#include <QUrl>
#include "main_window.h"
#include "image_view.h"
#include "pix_api.h"
#include "pix_license.h"

using namespace std;

static const double ZOOM_STEP = 1.25;
static const double ZOOM_MIN = 0.02;
static const double ZOOM_MAX = 32.0;

// file types in save dialogs; the PIX type chosen there selects the algorithm
static const char *PIX1_FILTER = "PIX algorithm 1 (*.pix)";
static const char *PIX2_FILTER = "PIX algorithm 2, smaller files (*.pix)";

// shows the wait cursor while it exists
struct WaitCursor
{
    WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursor() { QApplication::restoreOverrideCursor(); }
};

//----------------------------------------------------------------------------------------------------------------------
static bool isPixFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QByteArray magic = file.read(4);
    return magic.size() == 4 && magic[0] == 'P' && magic[1] == 'I' && magic[2] == 'X' && magic[3] == 0;
}

//----------------------------------------------------------------------------------------------------------------------
static QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

//----------------------------------------------------------------------------------------------------------------------
MainWindow::MainWindow() : currentFileSize(0)
{
    view = new ImageView;
    scrollArea = new QScrollArea;
    scrollArea->setBackgroundRole(QPalette::Dark);
    scrollArea->setAlignment(Qt::AlignCenter);
    scrollArea->setWidget(view);
    setCentralWidget(scrollArea);

    infoLabel = new QLabel;
    zoomLabel = new QLabel;
    statusBar()->addPermanentWidget(infoLabel);
    statusBar()->addPermanentWidget(zoomLabel);

    createActions();
    updateActions();
    setAcceptDrops(true);
    setWindowTitle("PIX Viewer");
    resize(1000, 750);
    lastDir = QDir::currentPath();
    statusBar()->showMessage("Open an image (Ctrl+O) or drop a file here");
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::createActions()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    QAction *openAct = fileMenu->addAction("&Open...");
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::open);
    saveAsAct = fileMenu->addAction("Save &As...");
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    saveAsAct->setStatusTip("Save the image as PIX, PNG, BMP or JPEG (chosen by file extension)");
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::saveAs);
    fileMenu->addSeparator();
    QAction *quitAct = fileMenu->addAction("&Quit");
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    QMenu *convertMenu = menuBar()->addMenu("&Convert");
    QAction *toPixAct = convertMenu->addAction("Image to &PIX...");
    toPixAct->setShortcut(QKeySequence("Ctrl+E"));
    toPixAct->setStatusTip("Choose a PNG, JPEG, BMP, ... image and save it as PIX");
    connect(toPixAct, &QAction::triggered, this, &MainWindow::imageToPix);
    QAction *fromPixAct = convertMenu->addAction("PIX to &Image...");
    fromPixAct->setShortcut(QKeySequence("Ctrl+D"));
    fromPixAct->setStatusTip("Choose a PIX file and save it as PNG, BMP or JPEG");
    connect(fromPixAct, &QAction::triggered, this, &MainWindow::pixToImage);
    convertMenu->addSeparator();
    QMenu *algorithmMenu = convertMenu->addMenu("PIX &Algorithm");
    QActionGroup *algorithmGroup = new QActionGroup(this);
    algorithm1Act = algorithmMenu->addAction("Algorithm &1: nibble codes + zlib");
    algorithm1Act->setStatusTip("Save PIX files with algorithm 1");
    algorithm2Act = algorithmMenu->addAction("Algorithm &2: prediction + arithmetic coding");
    algorithm2Act->setStatusTip("Save PIX files with algorithm 2 (smaller files)");
    algorithm1Act->setCheckable(true);
    algorithm2Act->setCheckable(true);
    algorithmGroup->addAction(algorithm1Act);
    algorithmGroup->addAction(algorithm2Act);
    algorithm2Act->setChecked(true);

    QMenu *viewMenu = menuBar()->addMenu("&View");
    zoomInAct = viewMenu->addAction("Zoom &In");
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAct, &QAction::triggered, this, &MainWindow::zoomIn);
    zoomOutAct = viewMenu->addAction("Zoom &Out");
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, &MainWindow::zoomOut);
    actualSizeAct = viewMenu->addAction("&Actual Size");
    actualSizeAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(actualSizeAct, &QAction::triggered, this, &MainWindow::actualSize);
    viewMenu->addSeparator();
    fitAct = viewMenu->addAction("&Fit to Window");
    fitAct->setShortcut(QKeySequence("Ctrl+F"));
    fitAct->setStatusTip("Shrink large images to fit the window");
    fitAct->setCheckable(true);
    fitAct->setChecked(true);
    connect(fitAct, &QAction::toggled, this, &MainWindow::fitToWindow);

    QMenu *helpMenu = menuBar()->addMenu("&Help");
    QAction *aboutAct = helpMenu->addAction("&About");
    connect(aboutAct, &QAction::triggered, this, &MainWindow::about);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::updateActions()
{
    bool has = !image.isNull();
    saveAsAct->setEnabled(has);
    zoomInAct->setEnabled(has && view->zoom() < ZOOM_MAX);
    zoomOutAct->setEnabled(has && view->zoom() > ZOOM_MIN);
    actualSizeAct->setEnabled(has);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::updateStatus()
{
    if (image.isNull())
    {
        infoLabel->clear();
        zoomLabel->clear();
        return;
    }
    QString text = QString("%1   %2 x %3").arg(QFileInfo(currentPath).fileName()).arg(image.width()).arg(image.height());
    if (currentFileSize > 0)
    {
        double raw = (double)image.width() * image.height() * 3;
        text += QString("   %1 (%2% of raw RGB)").arg(formatSize(currentFileSize))
                    .arg(100.0 * currentFileSize / raw, 0, 'f', 1);
    }
    infoLabel->setText(text);
    zoomLabel->setText(QString("%1%").arg(qRound(view->zoom() * 100)));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::setZoom(double zoom)
{
    if (zoom < ZOOM_MIN)
        zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX)
        zoom = ZOOM_MAX;
    view->setZoom(zoom);
    updateActions();
    updateStatus();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::applyFit()
{
    if (!view->hasImage())
        return;
    QSize area = scrollArea->maximumViewportSize();
    QSize size = view->imageSize();
    double zoom = qMin((double)area.width() / size.width(), (double)area.height() / size.height());
    setZoom(qMin(zoom, 1.0));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::showImage(const QImage &newImage, const QString &path, qint64 fileSize)
{
    image = newImage;
    currentPath = path;
    currentFileSize = fileSize;
    view->setImage(image);
    if (fitAct->isChecked())
        applyFit();
    else
        setZoom(view->zoom());
    setWindowTitle(QFileInfo(path).fileName() + " - PIX Viewer");
    updateActions();
    updateStatus();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::showError(const QString &title, const QString &message)
{
    QMessageBox::warning(this, title, message);
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::readPix(const QString &path, QImage &result, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = file.errorString();
        return false;
    }
    QByteArray bytes = file.readAll();
    vector<unsigned char> data(bytes.begin(), bytes.end());

    WaitCursor wait;
    QElapsedTimer timer;
    timer.start();
    vector<unsigned char> rgb;
    int width = 0;
    int height = 0;
    if (!pixDecodeRgb(data, rgb, width, height))
    {
        error = QString::fromLocal8Bit(pixLastError());
        return false;
    }
    result = QImage(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; y++)
        memcpy(result.scanLine(y), &rgb[(size_t)y * width * 3], (size_t)width * 3);
    statusBar()->showMessage(QString("Decoded PIX in %1 s").arg(timer.elapsed() / 1000.0, 0, 'f', 3));
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::readImage(const QString &path, QImage &result, QString &error)
{
    if (isPixFile(path))
        return readPix(path, result, error);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    WaitCursor wait;
    result = reader.read();
    if (result.isNull())
    {
        error = reader.errorString();
        return false;
    }
    statusBar()->clearMessage();
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
static QImage toRgb888(const QImage &source)
{
    // PIX stores 24 bit RGB: transparent pixels are composed onto white (as the command line tool does);
    // any size works, the encoder pads to whole 16x16 blocks and the file keeps the original size
    if (!source.hasAlphaChannel())
        return source.convertToFormat(QImage::Format_RGB888);
    QImage white(source.size(), QImage::Format_RGB32);
    white.fill(Qt::white);
    QPainter painter(&white);
    painter.drawImage(0, 0, source);
    painter.end();
    return white.convertToFormat(QImage::Format_RGB888);
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::writePix(const QString &path, const QImage &source, QString &error)
{
    QImage rgb = toRgb888(source);

    WaitCursor wait;
    QElapsedTimer timer;
    timer.start();
    vector<unsigned char> data;
    PixEncodeInfo info;
    int algorithm = algorithm2Act->isChecked() ? 2 : 1;
    if (!pixEncodeRgb(rgb.constBits(), rgb.width(), rgb.height(), (int)rgb.bytesPerLine(), data, info, algorithm))
    {
        error = QString::fromLocal8Bit(pixLastError());
        return false;
    }
    double seconds = timer.elapsed() / 1000.0;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write((const char *)&data[0], (qint64)data.size()) != (qint64)data.size())
    {
        error = file.errorString();
        return false;
    }
    file.close();

    double raw = (double)rgb.width() * rgb.height() * 3;
    statusBar()->showMessage(QString("Saved %1: %2 (%3% of raw RGB), algorithm %4, encoded in %5 s")
                                 .arg(QFileInfo(path).fileName()).arg(formatSize((qint64)data.size()))
                                 .arg(100.0 * data.size() / raw, 0, 'f', 1).arg(algorithm)
                                 .arg(seconds, 0, 'f', 3));
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::writeImage(const QString &path, const QImage &source, QString &error)
{
    QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "pix")
        return writePix(path, source, error);

    QImageWriter writer(path);
    if (suffix == "jpg" || suffix == "jpeg")
        writer.setQuality(95);
    WaitCursor wait;
    if (!writer.write(source))
    {
        error = writer.errorString();
        return false;
    }
    statusBar()->showMessage(QString("Saved %1: %2").arg(QFileInfo(path).fileName())
                                 .arg(formatSize(QFileInfo(path).size())));
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
QString MainWindow::openFilter() const
{
    QString patterns = "*.pix";
    QList<QByteArray> formats = QImageReader::supportedImageFormats();
    for (int i = 0; i < formats.size(); i++)
        patterns += " *." + QString::fromLatin1(formats[i]);
    return QString("Images (%1);;PIX images (*.pix);;All files (*)").arg(patterns);
}

//----------------------------------------------------------------------------------------------------------------------
QString MainWindow::saveFilter() const
{
    return QString("%1;;%2;;PNG images (*.png);;BMP images (*.bmp);;JPEG images (*.jpg *.jpeg)")
        .arg(PIX1_FILTER, PIX2_FILTER);
}

//----------------------------------------------------------------------------------------------------------------------
QString MainWindow::pixFilter(int algorithm) const
{
    return algorithm == 2 ? PIX2_FILTER : PIX1_FILTER;
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::choosePixAlgorithm(const QString &filter)
{
    // a PIX file type chosen in a save dialog becomes the algorithm used for saving
    if (filter == PIX1_FILTER)
        algorithm1Act->setChecked(true);
    else if (filter == PIX2_FILTER)
        algorithm2Act->setChecked(true);
}

//----------------------------------------------------------------------------------------------------------------------
QString MainWindow::askSourceFile(const QString &title, const QString &filter)
{
    QString path = QFileDialog::getOpenFileName(this, title, lastDir, filter);
    if (!path.isEmpty())
        lastDir = QFileInfo(path).absolutePath();
    return path;
}

//----------------------------------------------------------------------------------------------------------------------
QString MainWindow::askTargetFile(const QString &title, const QString &suggestedName, const QString &filter,
                                  QString *chosenFilter)
{
    QFileDialog dialog(this, title, lastDir, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QFileInfo(suggestedName).suffix());
    // a PIX file starts with the PIX type of the current algorithm selected
    if (QFileInfo(suggestedName).suffix().toLower() == "pix")
        dialog.selectNameFilter(pixFilter(algorithm2Act->isChecked() ? 2 : 1));
    dialog.selectFile(suggestedName);
    connect(&dialog, &QFileDialog::filterSelected, this, &MainWindow::saveTypeSelected);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return QString();
    if (chosenFilter)
        *chosenFilter = dialog.selectedNameFilter();
    QString path = dialog.selectedFiles().first();
    lastDir = QFileInfo(path).absolutePath();
    return path;
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::saveTypeSelected(const QString &filter)
{
    // the file name in the save dialog gets the extension of the selected file type,
    // e.g. "JPEG images (*.jpg *.jpeg)": photo.png -> photo.jpg, photo.jpeg stays
    QFileDialog *dialog = qobject_cast<QFileDialog *>(sender());
    if (!dialog)
        return;
    QStringList suffixes;
    int pos = filter.indexOf("*.");
    while (pos >= 0)
    {
        int end = pos + 2;
        while (end < filter.size() && filter[end] != ' ' && filter[end] != ')')
            end++;
        QString suffix = filter.mid(pos + 2, end - pos - 2);
        if (!suffix.isEmpty() && suffix != "*")
            suffixes.append(suffix);
        pos = filter.indexOf("*.", end);
    }
    if (suffixes.isEmpty())
        return;

    // read the name before changing the default suffix: selectedFiles() adds the default suffix
    // to a name typed without one
    QStringList files = dialog->selectedFiles();
    dialog->setDefaultSuffix(suffixes.first());
    if (files.isEmpty())
        return;
    QFileInfo current(files.first());
    QString name = current.fileName();
    if (name.isEmpty() || current.isDir() || suffixes.contains(current.suffix(), Qt::CaseInsensitive))
        return;
    QString base = current.suffix().isEmpty() ? name : current.completeBaseName();
    dialog->selectFile(base + "." + suffixes.first());
}

//----------------------------------------------------------------------------------------------------------------------
bool MainWindow::loadFile(const QString &path)
{
    QImage loaded;
    QString error;
    if (!readImage(path, loaded, error))
    {
        showError("Open", QString("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(path), error));
        return false;
    }
    lastDir = QFileInfo(path).absolutePath();
    showImage(loaded, path, QFileInfo(path).size());
    return true;
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::open()
{
    QString path = askSourceFile("Open Image", openFilter());
    if (!path.isEmpty())
        loadFile(path);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::saveAs()
{
    if (image.isNull())
        return;
    QFileInfo info(currentPath);
    QString suggested = info.completeBaseName() + (isPixFile(currentPath) ? ".png" : ".pix");
    QString filter;
    QString path = askTargetFile("Save Image As", suggested, saveFilter(), &filter);
    if (path.isEmpty())
        return;
    choosePixAlgorithm(filter);
    QString error;
    if (!writeImage(path, image, error) && !error.isEmpty())
        showError("Save As", QString("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(path), error));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::imageToPix()
{
    QString source = askSourceFile("Image to PIX - choose image", openFilter());
    if (source.isEmpty() || !loadFile(source))
        return;
    if (isPixFile(source))
    {
        showError("Image to PIX", "This file is already a PIX image.");
        return;
    }
    QString filter;
    QString target = askTargetFile("Image to PIX - save as", QFileInfo(source).completeBaseName() + ".pix",
                                   QString("%1;;%2").arg(PIX1_FILTER, PIX2_FILTER), &filter);
    if (target.isEmpty())
        return;
    choosePixAlgorithm(filter);
    if (QFileInfo(target).suffix().toLower() != "pix")
        target += ".pix";
    QString error;
    if (!writePix(target, image, error) && !error.isEmpty())
        showError("Image to PIX", QString("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(target), error));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::pixToImage()
{
    QString source = askSourceFile("PIX to Image - choose PIX file", "PIX images (*.pix);;All files (*)");
    if (source.isEmpty())
        return;
    if (!isPixFile(source))
    {
        showError("PIX to Image", QString("%1 is not a PIX file.").arg(QDir::toNativeSeparators(source)));
        return;
    }
    if (!loadFile(source))
        return;
    QString target = askTargetFile("PIX to Image - save as", QFileInfo(source).completeBaseName() + ".png",
                                   "PNG images (*.png);;BMP images (*.bmp);;JPEG images (*.jpg *.jpeg)");
    if (target.isEmpty())
        return;
    QString error;
    if (!writeImage(target, image, error) && !error.isEmpty())
        showError("PIX to Image", QString("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(target), error));
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::zoomIn()
{
    fitAct->setChecked(false);
    setZoom(view->zoom() * ZOOM_STEP);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::zoomOut()
{
    fitAct->setChecked(false);
    setZoom(view->zoom() / ZOOM_STEP);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::actualSize()
{
    fitAct->setChecked(false);
    setZoom(1.0);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::fitToWindow()
{
    if (fitAct->isChecked())
        applyFit();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::about()
{
    // the BSD 2-clause license asks binaries to carry the copyright notice and the license
    QStringList paragraphs = QString::fromUtf8(PIX_LICENSE).split("\n\n");
    QString license;
    for (int i = 0; i < paragraphs.size(); i++)
        license += "<p><small>" + paragraphs[i].toHtmlEscaped() + "</small></p>";
    QMessageBox::about(this, "About PIX Viewer",
        "<h3>PIX Viewer</h3>"
        "<p>Views PIX images and any image format supported by Qt, "
        "and converts images to and from PIX.</p>"
        "<p>PIX is a lossless 24 bit RGB format with two algorithms: 16x16 blocks of nibble codes "
        "(colour caches, runs and MED predicted residuals) compressed with zlib, or per pixel prediction "
        "with adaptive binary arithmetic coding (smaller files, slower).</p>"
        "<p>" + QString::fromUtf8(PIX_COPYRIGHT).toHtmlEscaped() + "</p>" + license);
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (fitAct->isChecked())
        applyFit();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls() && !mime->urls().isEmpty() && mime->urls().first().isLocalFile())
        event->acceptProposedAction();
}

//----------------------------------------------------------------------------------------------------------------------
void MainWindow::dropEvent(QDropEvent *event)
{
    QString path = event->mimeData()->urls().first().toLocalFile();
    event->acceptProposedAction();
    loadFile(path);
}
