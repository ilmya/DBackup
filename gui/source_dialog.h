#pragma once

#include <QDir>
#include <QFileDialog>

inline void configureSourceDialog(QFileDialog &dialog, bool directory) {
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setDirectory(QDir::homePath());
    dialog.setFileMode(directory ? QFileDialog::Directory : QFileDialog::ExistingFiles);
    dialog.setOption(QFileDialog::ShowDirsOnly, directory);
}
