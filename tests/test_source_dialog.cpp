#include "gui/source_dialog.h"
#include <QApplication>
#include <gtest/gtest.h>

TEST(SourceDialog, StartsAtHomeForFilesAndDirectories) {
    for (bool directory : {false, true}) {
        QFileDialog dialog;
        configureSourceDialog(dialog, directory);
        EXPECT_EQ(dialog.directory().absolutePath(), QDir::homePath());
        EXPECT_TRUE(dialog.testOption(QFileDialog::DontUseNativeDialog));
        EXPECT_EQ(dialog.fileMode(), directory ? QFileDialog::Directory : QFileDialog::ExistingFiles);
    }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
