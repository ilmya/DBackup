#pragma once
#include <QString>
namespace DBackupTheme {
inline QString styleSheet() {
    return QStringLiteral(R"(
        * { color: #172033; }
        QMainWindow, QWidget#appRoot, QDialog { background: #f5f7fb; }
        QWidget#appRoot { border: 1px solid #94a3b8; }
        QMenuBar { background: white; border-bottom: 1px solid #cbd5e1; padding: 2px; }
        QMenuBar::item:selected { background: #dbeafe; }
        QLabel#pageTitle { font-size: 22px; font-weight: 700; color: #0f172a; }
        QGroupBox { background: white; border: 1px solid #e2e8f0; border-radius: 10px; margin-top: 12px; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; color: #334155; font-weight: 600; }
        QLineEdit, QComboBox, QSpinBox, QDateEdit, QListWidget, QTreeView, QPlainTextEdit { background: white; border: 1px solid #a8b7ca; border-radius: 6px; padding: 5px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDateEdit:focus, QListWidget:focus, QPlainTextEdit:focus { border-color: #2563eb; }
        QPushButton { background: #f8fafc; border: 1px solid #94a3b8; border-radius: 6px; padding: 7px 14px; }
        QPushButton:hover { background: #eff6ff; border-color: #2563eb; color: #1d4ed8; }
        QPushButton:pressed { background: #dbeafe; }
        QPushButton:disabled { color: #94a3b8; background: #f1f5f9; }
        QPushButton[primary="true"] { background: #2563eb; color: white; border-color: #2563eb; font-weight: 600; padding: 9px 18px; }
        QPushButton[primary="true"]:hover { background: #1d4ed8; }
        QPushButton[primary="true"]:disabled { background: #94a3b8; border-color: #94a3b8; }
        QLabel[muted="true"] { color: #64748b; }
        QProgressBar { border: none; background: #e2e8f0; border-radius: 4px; height: 8px; text-align: center; }
        QProgressBar::chunk { background: #2563eb; border-radius: 4px; }
        QStatusBar { background: white; border-top: 1px solid #e2e8f0; color: #64748b; }
        QTabWidget::pane { border: 1px solid #e2e8f0; background: white; border-radius: 8px; }
        QTabBar::tab { padding: 9px 16px; color: #64748b; }
        QTabBar::tab:selected { color: #2563eb; font-weight: 600; }
        QTabBar[settingsActive="true"]::tab:selected { color: #64748b; font-weight: normal; }
    )");
}
}
