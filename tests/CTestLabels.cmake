# 为 CTest 测试分类并设置标签
# This file is included by CTest after GoogleTest has generated its test list
# Keep this allowlist small: all filesystem and cross-module cases are integration tests
set(_dbackup_unit_test_names
    CryptoTest.Sha256MatchesKnownVector
    CryptoTest.Pbkdf2IsDeterministicAndProducesRequestedLength
    CryptoTest.Pbkdf2MatchesKnownSha256Vector
    CryptoTest.AesGcmRoundTripAndRejectsModifiedTag
    CryptoTest.AesGcmAuthenticatesAssociatedData
    CryptoTest.AesGcmRoundTripAcrossChunkBoundaries
    CryptoTest.RejectsInvalidKeyAndNonceSizes
    PipelineAlgorithms.Md5Vectors
    PipelineAlgorithms.CompressionLimitAllowsTwoGibAndRejectsLarger
    PipelineAlgorithms.KnownHuffmanAndLz77Vectors
    PipelineAlgorithms.CompressionAndCorruption
    PipelineAlgorithms.DeflateCrossChecksZlib
    FilterOptionsTest.StrictValidation
    FilterOptionsTest.RejectsInvalidProgrammaticConditions
    LinuxFile.RelativePathsRejectTraversal
    BackupPolicyTest.MapsLinuxFileTypesAndHardlinkFilterCategory
    BackupPolicyTest.MatchesPathTypeNameOwnerExtensionSizeAndTimeRules
    BackupPolicyTest.AppliesTypeMasksAndIncludeExcludePathPatterns
    BackupPolicyTest.ResolvesHierarchicalSourceSelectionAndComponentBoundaries
    BackupPolicyTest.TogglesSelectionAndReturnsMinimalSelectedRoots
    BackupPolicyTest.ValidatesMetadataTypeAndDeviceNumberFields
)

foreach(_test IN LISTS BackupToolTests_TESTS BackupToolAutomationTests_TESTS)
    list(FIND _dbackup_unit_test_names "${_test}" _unit_index)
    if(NOT _unit_index EQUAL -1 OR _test MATCHES "^(JobPolicy|JobRunPolicy|CronRules|TarRules)\\.")
        set_tests_properties("${_test}" PROPERTIES LABELS unit)
    else()
        set_tests_properties("${_test}" PROPERTIES LABELS integration)
    endif()
endforeach()

unset(_test)
unset(_unit_index)
unset(_dbackup_unit_test_names)
