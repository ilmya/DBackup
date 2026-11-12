// 验证计划管理测试的程序入口
#include <gtest/gtest.h>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName("DBackup");
    QCoreApplication::setApplicationName("AutomationUnitTests");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
