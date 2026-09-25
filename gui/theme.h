#pragma once
#include <QString>
namespace DBackupTheme {
inline QString styleSheet() {
    return QStringLiteral(R"(
        * { color: #172033; }
        QMainWindow, QWidget#appRoot { background: #f5f7fb; }
        QFrame#sidebar { background: #111827; border: none; }
        QLabel#brand { color: white; font-size: 22px; font-weight: 700; padding: 6px 10px; }
        QLabel#brandSub { color: #94a3b8; font-size: 11px; padding: 0 10px 14px 10px; }
        QPushButton[nav="true"] { color: #cbd5e1; background: transparent; border: none; border-radius: 8px; padding: 11px 14px; text-align: left; font-weight: 500; }
        QPushButton[nav="true"]:hover { background: #1f2937; color: white; }
        QPushButton[nav="true"]:checked { background: #2563eb; color: white; }
        QLabel#pageTitle { font-size: 26px; font-weight: 700; color: #0f172a; }
        QLabel#pageSubtitle { color: #64748b; }
        QFrame[card="true"], QGroupBox { background: white; border: 1px solid #e2e8f0; border-radius: 10px; margin-top: 12px; }
        QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; color: #334155; font-weight: 600; }
        QLineEdit, QComboBox, QSpinBox, QDateEdit, QListWidget, QPlainTextEdit { background: white; border: 1px solid #cbd5e1; border-radius: 6px; padding: 7px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDateEdit:focus, QListWidget:focus, QPlainTextEdit:focus { border-color: #2563eb; }
        QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 7px; padding: 8px 14px; }
        QPushButton:hover { border-color: #2563eb; color: #1d4ed8; }
        QPushButton[primary="true"] { background: #2563eb; color: white; border-color: #2563eb; font-weight: 600; padding: 9px 18px; }
        QPushButton[primary="true"]:hover { background: #1d4ed8; }
        QPushButton[primary="true"]:disabled { background: #94a3b8; border-color: #94a3b8; }
        QLabel[muted="true"] { color: #64748b; }
        QLabel[metric="true"] { font-size: 24px; font-weight: 700; color: #0f172a; }
        QProgressBar { border: none; background: #e2e8f0; border-radius: 4px; height: 8px; text-align: center; }
        QProgressBar::chunk { background: #2563eb; border-radius: 4px; }
        QStatusBar { background: white; border-top: 1px solid #e2e8f0; color: #64748b; }
        QTabWidget::pane { border: 1px solid #e2e8f0; background: white; border-radius: 8px; }
        QTabBar::tab { padding: 9px 16px; color: #64748b; }
        QTabBar::tab:selected { color: #2563eb; font-weight: 600; }
    )");
}
}
