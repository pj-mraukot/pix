#include <QApplication>
#include "main_window.h"

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("PIX Viewer");

    MainWindow window;
    window.show();
    if (argc > 1)
        window.loadFile(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
