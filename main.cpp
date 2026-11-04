#include <QApplication>
#include <QCoreApplication>

#include "gui/main_window.h"
#include "gui/theme.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("DBackup Team");
    QCoreApplication::setApplicationName("DBackup");
    QCoreApplication::setApplicationVersion("1.0.0");
    app.setStyle("Fusion");
    app.setStyleSheet(DBackupTheme::styleSheet());
    MainWindow window;
    window.show();
    return app.exec();
}
