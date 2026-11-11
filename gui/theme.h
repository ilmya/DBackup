#pragma once
#include <QString>
namespace DBackupTheme {
// 返回主窗口与对话框共用的应用内控件样式表。
inline QString styleSheet() {
    return QStringLiteral(R"(
        * { color: #172033; }
        QMainWindow, QWidget#appRoot, QDialog { background: #f5f7fb; }
        QWidget#appRoot { border: 1px solid #94a3b8; }
        QMenuBar { background: white; border-bottom: 1px solid #cbd5e1; padding: 2px; }
        QMenuBar::item:selected { background: #dbeafe; }
        QLabel#pageTitle { font-size: 19px; font-weight: 700; color: #0f172a; }
        QGroupBox { background: white; border: 1px solid #e2e8f0; border-radius: 7px; margin-top: 9px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #334155; font-weight: 600; }
        QLineEdit, QComboBox, QSpinBox, QDateEdit, QTreeView, QPlainTextEdit { background: white; border: 1px solid #a8b7ca; border-radius: 5px; padding: 3px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDateEdit:focus, QPlainTextEdit:focus { border-color: #2563eb; }
        QPushButton { background: #f8fafc; border: 1px solid #94a3b8; border-radius: 5px; padding: 4px 10px; }
        QPushButton:hover { background: #eff6ff; border-color: #2563eb; color: #1d4ed8; }
        QPushButton:pressed { background: #dbeafe; }
        QPushButton:disabled { color: #94a3b8; background: #f1f5f9; }
        QPushButton[primary="true"] { background: #2563eb; color: white; border-color: #2563eb; font-weight: 600; padding: 5px 12px; }
        QPushButton[primary="true"]:hover { background: #1d4ed8; }
        QPushButton[primary="true"]:disabled { background: #94a3b8; border-color: #94a3b8; }
        QLabel[muted="true"] { color: #64748b; }
        QProgressBar { border: none; background: #e2e8f0; border-radius: 4px; height: 8px; text-align: center; }
        QProgressBar::chunk { background: #2563eb; border-radius: 4px; }
        QStatusBar { background: white; border-top: 1px solid #e2e8f0; color: #64748b; }
        QTabWidget::pane { border: 1px solid #e2e8f0; background: white; border-radius: 8px; }
        QTabBar::tab { padding: 6px 13px; color: #64748b; }
        QTabBar::tab:selected { color: #2563eb; font-weight: 600; }
        QTabBar[settingsActive="true"]::tab:selected { color: #64748b; font-weight: normal; }
    )");
}
} // namespace DBackupTheme
