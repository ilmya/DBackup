// 图形备份程序的启动入口
#include <QApplication>
#include <QCoreApplication>

#include "gui/main_window.h"
#include "gui/theme.h"

// 初始化 Qt 应用、主题和主窗口，并进入事件循环
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("DBackup Team");
    QCoreApplication::setApplicationName("DBackup");
    QCoreApplication::setApplicationVersion("1.0.0");
    app.setStyle("Fusion");
    app.setStyleSheet(DBackupTheme::styleSheet());
    MainWindow window;
    window.show();
    return app.exec();
}
