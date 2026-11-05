#pragma once

#include "core/archive.h"
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;

class BackupOptionsWidget final : public QWidget {
 public:
    explicit BackupOptionsWidget(QWidget *parent = nullptr, bool allowRepository = true);
    bool incremental() const;
    bool encryptionEnabled() const;
    PackOptions options() const;
    void setOptions(bool repository, bool encrypted, PackFormat format, Compression compression);

 private:
    void updateEnabled();
    QButtonGroup *mode_ = nullptr;
    QButtonGroup *format_ = nullptr;
    QButtonGroup *compression_ = nullptr;
    QWidget *formatChoices_ = nullptr;
    QWidget *compressionChoices_ = nullptr;
    QCheckBox *compress_ = nullptr;
    QCheckBox *encrypt_ = nullptr;
    QCheckBox *showPassword_ = nullptr;
    QLineEdit *password_ = nullptr;
    QLabel *repositoryHint_ = nullptr;
    QLabel *repositoryEncryption_ = nullptr;
    QLabel *outputHint_ = nullptr;
    bool allowRepository_ = true;
};
