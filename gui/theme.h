// 图形界面共用样式
// 集中提供 Qt Widgets 样式，统一页面、按钮、表格和状态文本的视觉表现。
#pragma once
#include <QString>
namespace DBackupTheme {
// 返回主窗口与对话框共用的应用内控件样式表
inline QString styleSheet() {
    return QStringLiteral(R"(
        * { color: #172033; }
        QMainWindow, QWidget#appRoot, QDialog { background: #e8eef6; }
        QWidget#appRoot { background: #e8eef6; border: 1px solid #8294aa; }
        QFileDialog, QDialog#planEditor, QDialog#planDetails, QDialog#filterDialog { border: 1px solid #7188a2; }
        QScrollArea, QScrollArea > QWidget > QWidget { background: #e8eef6; border: none; }
        QMenuBar { background: white; border-bottom: 1px solid #cbd5e1; padding: 2px; }
        QMenuBar::item:selected { background: #cfe0f5; }
        QLabel#pageTitle { font-size: 19px; font-weight: 700; color: #0f172a; }
        QGroupBox { background: #ffffff; border: 1px solid #b7c5d6; border-radius: 7px; margin-top: 9px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #334155; font-weight: 600; }
        QLineEdit, QComboBox, QSpinBox, QDateEdit, QTreeView, QPlainTextEdit { background: #ffffff; border: 1px solid #899bb1; border-radius: 5px; padding: 3px; }
        QLineEdit:disabled { color: #66758a; background: #dce4ee; border-color: #b7c4d3; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDateEdit:focus, QPlainTextEdit:focus { border: 2px solid #1d4ed8; }
        QTableView { background: #ffffff; alternate-background-color: #eef3f9; border: 1px solid #9aaabd; gridline-color: #b3c0cf; selection-background-color: #c6dcf5; selection-color: #14253d; }
        QTableView::item { padding: 4px; }
        QTableView::item:selected { background: #c6dcf5; color: #14253d; }
        QTreeView::item:selected { background: #c6dcf5; color: #14253d; }
        QHeaderView::section { background: #d9e3ef; color: #172033; border: 1px solid #aebccd; padding: 4px 6px; font-weight: 600; }
        QPushButton { background: #f5f8fc; border: 1px solid #899bb1; border-radius: 5px; padding: 4px 10px; }
        QPushButton:hover { background: #dce9f8; border-color: #1d4ed8; color: #153c83; }
        QPushButton:pressed { background: #c6dcf5; }
        QPushButton:disabled { color: #66758a; background: #dce4ee; border-color: #b7c4d3; }
        QPushButton[primary="true"] { background: #1d4ed8; color: #ffffff; border-color: #1d4ed8; font-weight: 600; padding: 5px 12px; }
        QPushButton[primary="true"]:hover { background: #173eaf; border-color: #173eaf; }
        QPushButton[primary="true"]:disabled { color: #ffffff; background: #7388ad; border-color: #7388ad; }
        QLabel[muted="true"] { color: #475569; }
        QProgressBar { border: none; background: #cfd9e6; border-radius: 4px; height: 8px; text-align: center; }
        QProgressBar::chunk { background: #1d4ed8; border-radius: 4px; }
        QStatusBar { background: #ffffff; border-top: 1px solid #b7c5d6; color: #475569; }
        QTabWidget::pane { border: 1px solid #b7c5d6; background: #ffffff; border-radius: 8px; }
        QTabBar::tab { background: #d9e3ef; border: 1px solid #aebccd; border-right: none; border-bottom: none; border-radius: 0; margin: 0; padding: 6px 13px; color: #334155; }
        QTabBar::tab:last, QTabBar::tab:only-one { border-right: 1px solid #aebccd; }
        QTabBar::tab:selected { background: #ffffff; color: #173eaf; font-weight: 700; }
        QTabBar[settingsActive="true"]::tab:selected { background: #d9e3ef; color: #334155; font-weight: 600; }
    )");
}
} // namespace DBackupTheme
