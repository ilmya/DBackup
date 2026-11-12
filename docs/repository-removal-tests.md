# 增量仓库移除：测试条目与结果

日期：2026-11-12。环境：WSL Ubuntu、普通用户、Qt 6.8.3；使用临时 HOME、独立 D-Bus 和测试密钥环。

执行命令：`bash build.sh test`。构建成功，CTest **171 项通过、0 失败、0 跳过**。测试总耗时 38.28 秒，不含构建。按 CTest 条目计数，部分条目内包含多个用例。

| 分类 | 条目数 | 结果 |
|---|---:|---|
| unit | 52 | PASS |
| integration | 114 | PASS |
| support | 5 | PASS |

## 本次变更与覆盖范围

- 删除 repository 增量仓库实现、构建引用及专用测试；保留完整备份和文件级增量版本链。
- 完整备份：自定义格式、tar、压缩、加密、恢复、损坏与取消、特殊文件、系统 tar 互通。
- 增量备份：新增/修改/删除、类型与硬链接变化、加密清单、跨格式版本链恢复、保留清理、损坏拒绝。
- 计划兼容：跳过旧仓库计划；正常加载不含 repository 字段的完整计划及旧字段增量计划；保存后移除旧仓库记录且不再写 repository 字段。
- 自动任务：完整计划、定时规则、实时增量触发，部分完成时不清理历史。
- GUI offscreen、测试 CLI、演示比较工具和验收目录；目录覆盖 78 个案例、27 个自动场景定义。

首次构建发现移动后的 archive_internal.h 内容全为零字节，恢复声明并更新引用路径后，全量测试通过。Windows 原生试跑的两个演示工具测试因缺少 Linux API 失败；下表采用随后在支持的 Linux 环境中的执行结果。

未执行真实桌面 GUI 人工往返、提权设备节点及本次大文件专项验收。旧仓库数据保留，但不再支持读取和恢复。

原始日志：`out/build-test/repository-removal-test.log`；机器可读目录：`out/build-test/test-catalog.json`。

## 逐项输出

| # | CTest 条目 | 分类 | 结果 | 秒 |
|---:|---|---|---|---:|
| 1 | `ArchiveServiceTest.WrappedArchiveCrossesChunkBoundary` | integration | PASS | 0.08 |
| 2 | `ArchiveServiceTest.EmptyDirectory` | integration | PASS | 0.02 |
| 3 | `ArchiveServiceTest.UnreadableSourceDoesNotReplaceExistingArchive` | integration | PASS | 0.02 |
| 4 | `ArchiveServiceTest.SingleFileRoundTrip` | integration | PASS | 0.03 |
| 5 | `ArchiveServiceTest.MultipleFilesAndDirectories` | integration | PASS | 0.04 |
| 6 | `ArchiveServiceTest.MtimeRestoration` | integration | PASS | 0.03 |
| 7 | `ArchiveServiceTest.PackNonexistentSource` | integration | PASS | 0.01 |
| 8 | `ArchiveServiceTest.UnpackInvalidArchive` | integration | PASS | 0.01 |
| 9 | `ArchiveServiceTest.UnpackNonexistentFile` | integration | PASS | 0.01 |
| 10 | `ArchiveServiceTest.ArchiveHeaderFormat` | integration | PASS | 0.03 |
| 11 | `ArchiveServiceTest.RejectsUnsupportedArchiveVersions` | integration | PASS | 0.01 |
| 12 | `ArchiveServiceTest.LargeFileRoundTrip` | integration | PASS | 0.04 |
| 13 | `ArchiveServiceTest.EmptyFileRoundTrip` | integration | PASS | 0.03 |
| 14 | `ArchiveServiceTest.CompressedEncryptedRoundTrip` | integration | PASS | 0.10 |
| 15 | `ArchiveServiceTest.FilterByExtensionAndSize` | integration | PASS | 0.02 |
| 16 | `ArchiveServiceTest.DirectoryMtimeRestoredAfterChildren` | integration | PASS | 0.03 |
| 17 | `ArchiveServiceTest.UnicodeAndEmojiFileNamesRoundTrip` | integration | PASS | 0.03 |
| 18 | `ArchiveServiceTest.MultipleSourceDirectoryTimesRoundTrip` | integration | PASS | 0.03 |
| 19 | `ArchiveServiceTest.FileAttributesRestoration` | integration | PASS | 0.03 |
| 20 | `ArchiveServiceTest.DetectsArchiveTampering` | integration | PASS | 0.02 |
| 21 | `ArchiveServiceTest.NoReplacePreservesExistingArchive` | integration | PASS | 0.02 |
| 22 | `ArchiveServiceTest.AtomicallyReplacesExistingArchive` | integration | PASS | 0.02 |
| 23 | `ArchiveServiceTest.ExcludesDestinationArchiveFromSource` | integration | PASS | 0.03 |
| 24 | `ArchiveServiceTest.SingleFileAsBackupSource` | integration | PASS | 0.03 |
| 25 | `ArchiveServiceTest.RejectsSourceEqualToDestination` | integration | PASS | 0.01 |
| 26 | `ArchiveServiceTest.MultipleBackupSources` | integration | PASS | 0.04 |
| 27 | `ArchiveServiceTest.FilterAcrossAllSixCategories` | integration | PASS | 0.03 |
| 28 | `ArchiveServiceTest.PreviewWithMultiExtensionAndWildcardRules` | integration | PASS | 0.02 |
| 29 | `ArchiveStreaming.AllFormatsAlgorithmsAndPasswordsCrossChunkBoundary` | integration | PASS | 1.46 |
| 30 | `ArchiveStreaming.MetadataInspectionDoesNotLoadLargeFileContent` | integration | PASS | 0.07 |
| 31 | `ArchiveStreaming.LegacyEnvelopeIsRejectedExplicitly` | integration | PASS | 0.01 |
| 32 | `ArchiveStreaming.SourceChangePreservesExistingArchiveAndCleansTemps` | integration | PASS | 0.02 |
| 33 | `FilterOptionsTest.StrictValidation` | unit | PASS | 0.01 |
| 34 | `FilterOptionsTest.RejectsInvalidProgrammaticConditions` | unit | PASS | 0.01 |
| 35 | `LinuxArchive.EncryptedRoundTripWithLinksAndFilter` | integration | PASS | 0.08 |
| 36 | `ArchiveV3Test.UsesAuthenticatedEncryptionAndSupportsCancellation` | integration | PASS | 0.06 |
| 37 | `CryptoTest.Sha256MatchesKnownVector` | unit | PASS | 0.01 |
| 38 | `CryptoTest.Pbkdf2IsDeterministicAndProducesRequestedLength` | unit | PASS | 0.01 |
| 39 | `CryptoTest.Pbkdf2MatchesKnownSha256Vector` | unit | PASS | 0.01 |
| 40 | `CryptoTest.AesGcmRoundTripAndRejectsModifiedTag` | unit | PASS | 0.01 |
| 41 | `CryptoTest.AesGcmAuthenticatesAssociatedData` | unit | PASS | 0.01 |
| 42 | `CryptoTest.RejectsInvalidKeyAndNonceSizes` | unit | PASS | 0.01 |
| 43 | `CryptoTest.AesGcmRoundTripAcrossChunkBoundaries` | unit | PASS | 0.02 |
| 44 | `ArchiveBlocks.RoundTripAndAuthenticatedMetadata` | integration | PASS | 0.54 |
| 45 | `ArchiveBlocks.RejectsTruncatedReorderedAndTamperedRecords` | integration | PASS | 0.12 |
| 46 | `ArchiveBlocks.RejectsDeletedDuplicatedAndReorderedBlocks` | integration | PASS | 0.13 |
| 47 | `ArchiveBlocks.ReportsDestinationWriteFailure` | integration | PASS | 0.01 |
| 48 | `ArchiveBlocks.TarMetadataOffsetsReferToStoredFileData` | integration | PASS | 0.01 |
| 49 | `ArchiveBlocks.AuthenticatedTarIsVerifiedBeforeRestore` | integration | PASS | 0.07 |
| 50 | `ArchiveBlocks.ReadsPreviousChunkedIdentifier` | integration | PASS | 0.03 |
| 51 | `PipelineAlgorithms.CompressionLimitAllowsTwoGibAndRejectsLarger` | unit | PASS | 0.01 |
| 52 | `PipelineAlgorithms.Md5Vectors` | unit | PASS | 0.01 |
| 53 | `PipelineAlgorithms.KnownHuffmanAndLz77Vectors` | unit | PASS | 0.01 |
| 54 | `PipelineAlgorithms.CompressionAndCorruption` | unit | PASS | 0.01 |
| 55 | `PipelineAlgorithms.DeflateCrossChecksZlib` | unit | PASS | 0.01 |
| 56 | `PipelineAlgorithms.AllFormatCombinations` | integration | PASS | 0.54 |
| 57 | `LinuxArchiveFormat.EncryptedCompressedRoundTripAndCorruption` | integration | PASS | 0.08 |
| 58 | `LinuxArchiveFormat.RejectsOldAndInvalidPaths` | integration | PASS | 0.01 |
| 59 | `TarRules.LargeFileHeaderUsesPaxSize` | unit | PASS | 0.01 |
| 60 | `TarRules.RejectsEmptyAndTruncatedInput` | unit | PASS | 0.01 |
| 61 | `TarRules.AcceptsEmptyArchive` | unit | PASS | 0.01 |
| 62 | `TarRules.RejectsInvalidChecksum` | unit | PASS | 0.01 |
| 63 | `TarRules.RejectsUnsafePathsAndUnsupportedTypes` | unit | PASS | 0.01 |
| 64 | `LinuxScan.PreservesLinksAndAppliesFileFilter` | integration | PASS | 0.01 |
| 65 | `LinuxScan.HonorsCancellation` | integration | PASS | 0.01 |
| 66 | `LinuxScan.PreviewCountsSpecialTypesAndFiltersFifo` | integration | PASS | 0.02 |
| 67 | `LinuxScan.PreviewReportsUnreadableFiles` | integration | PASS | 0.01 |
| 68 | `LinuxScan.NestedSelectionExcludesAndReincludesWithNewFiles` | integration | PASS | 0.01 |
| 69 | `BackupPolicyTest.MapsLinuxFileTypesAndHardlinkFilterCategory` | unit | PASS | 0.01 |
| 70 | `BackupPolicyTest.MatchesPathTypeNameOwnerExtensionSizeAndTimeRules` | unit | PASS | 0.01 |
| 71 | `BackupPolicyTest.AppliesTypeMasksAndIncludeExcludePathPatterns` | unit | PASS | 0.01 |
| 72 | `BackupPolicyTest.ResolvesHierarchicalSourceSelectionAndComponentBoundaries` | unit | PASS | 0.01 |
| 73 | `BackupPolicyTest.TogglesSelectionAndReturnsMinimalSelectedRoots` | unit | PASS | 0.01 |
| 74 | `BackupPolicyTest.ValidatesMetadataTypeAndDeviceNumberFields` | unit | PASS | 0.01 |
| 75 | `LinuxRestore.CancellationAfterContentWritePreservesExistingFile` | integration | PASS | 0.01 |
| 76 | `LinuxRestore.RestoresFilesAndLinks` | integration | PASS | 0.01 |
| 77 | `LinuxRestore.RejectsEscapesAndExistingSymlinkParents` | integration | PASS | 0.01 |
| 78 | `LinuxRestore.SkipsPermissionFailuresAndKeepsOtherContent` | integration | PASS | 0.01 |
| 79 | `LinuxFile.RelativePathsRejectTraversal` | unit | PASS | 0.01 |
| 80 | `LinuxFile.DirectoryCreationRejectsSymlinkTraversal` | integration | PASS | 0.01 |
| 81 | `LinuxFile.MetadataPreservesModeAndTime` | integration | PASS | 0.03 |
| 82 | `SpecialFiles.SocketRoundTripsInBothBackupFormats` | integration | PASS | 0.05 |
| 83 | `SpecialFiles.UnreadableSourceIsReportedWithoutLosingReadableFiles` | integration | PASS | 0.03 |
| 84 | `IncrementalDiff.FindsAddedChangedAndDeletedPaths` | integration | PASS | 0.01 |
| 85 | `IncrementalDiff.DetectsTypeAndLinkChanges` | integration | PASS | 0.01 |
| 86 | `IncrementalDiff.RejectsUnsafeAndDuplicatePathsWithoutReplacingOutput` | integration | PASS | 0.01 |
| 87 | `IncrementalScan.ComparesActualFilesAndPreservesOutputOnFailure` | integration | PASS | 0.01 |
| 88 | `IncrementalScan.RejectsPartialScan` | integration | PASS | 0.01 |
| 89 | `IncrementalScan.DetectsSameSizeAndTimestampContentChange` | integration | PASS | 0.01 |
| 90 | `IncrementalManifest.RoundTripsAndRejectsCorruption` | integration | PASS | 0.01 |
| 91 | `IncrementalManifest.RejectsInvalidChainsAndPaths` | integration | PASS | 0.01 |
| 92 | `IncrementalManifest.EncryptsAuthenticatesAndRejectsDowngrades` | integration | PASS | 0.08 |
| 93 | `IncrementalChain.ValidatesDependenciesAndDeletionRecords` | integration | PASS | 0.01 |
| 94 | `IncrementalChain.RetentionKeepsTheRequiredFullBackup` | integration | PASS | 0.01 |
| 95 | `IncrementalChain.CreatesListsAndRestoresVersionsWithoutOverwriting` | integration | PASS | 0.50 |
| 96 | `IncrementalChain.RestoresCustomAndTarVersionsInSameChain` | integration | PASS | 0.98 |
| 97 | `IncrementalChain.PrunesOnlyWholeObsoleteChains` | integration | PASS | 0.20 |
| 98 | `IncrementalChain.StrictRetentionRebasesOldestKeptVersion` | integration | PASS | 3.12 |
| 99 | `IncrementalChain.StartsNewFullVersionWhenHardLinkGroupChanges` | integration | PASS | 0.06 |
| 100 | `IncrementalChain.RestoresFileDirectoryAndSymlinkTransitions` | integration | PASS | 0.09 |
| 101 | `IncrementalChain.RejectsCorruptedArchiveAndCancelledVersion` | integration | PASS | 0.04 |
| 102 | `IncrementalChain.RestoresFifoAndSocketWithNotice` | integration | PASS | 0.04 |
| 103 | `IncrementalChain.RestoresChangesInsideReadOnlyDirectory` | integration | PASS | 0.07 |
| 104 | `IncrementalArchive.WritesSelectedHardLinkAsIndependentFile` | integration | PASS | 0.02 |
| 105 | `IncrementalArchive.RejectsUnknownPathsAndDoesNotOverwriteOnFailure` | integration | PASS | 0.01 |
| 106 | `IncrementalArchive.SupportsCompressedEncryptedTarAndCancellation` | integration | PASS | 0.08 |
| 107 | `IncrementalArchive.ChecksArchiveEntriesAgainstVersionState` | integration | PASS | 0.01 |
| 108 | `IncrementalArchive.BuildsAndChecksDeltaFromRealScans` | integration | PASS | 0.02 |
| 109 | `IncrementalArchive.VerifiesEncryptedArchiveAndRejectsWrongPassword` | integration | PASS | 0.08 |
| 110 | `JobManagerTest.InjectedClockRunsDueJobOnceAndAdvancesSchedule` | integration | PASS | 0.07 |
| 111 | `JobManagerTest.RemembersLastCompleteSuccessAcrossFailuresAndReload` | integration | PASS | 0.03 |
| 112 | `JobManagerTest.PersistsRealtimeOnlySelectionRulesAndPartialResult` | integration | PASS | 0.27 |
| 113 | `JobManagerTest.PersistsManualArchivePlanAndRejectsRealtimeArchive` | integration | PASS | 0.02 |
| 114 | `JobManagerTest.LoadsSupportedPlansAndSkipsLegacyRepositoryPlan` | integration | PASS | 0.02 |
| 115 | `JobManagerTest.PersistsAndExecutesRealtimeTarIncrementalPlan` | integration | PASS | 0.05 |
| 116 | `JobManagerTest.PersistsAndExecutesRealtimeCustomIncrementalPlan` | integration | PASS | 0.05 |
| 117 | `JobManagerTest.ValidatesPlanFieldsAndRejectsUnsafeIdentifiers` | integration | PASS | 0.02 |
| 118 | `JobManagerTest.SaveFailureDoesNotCreateOrChangeAPlan` | integration | PASS | 0.01 |
| 119 | `JobManagerTest.InvalidConfigurationIsReportedAndNeverOverwritten` | integration | PASS | 0.01 |
| 120 | `JobManagerTest.ArchivePlanCreatesIndependentTarAndKeepsLatestOnSuccess` | integration | PASS | 0.03 |
| 121 | `JobManagerTest.PartialArchivePlanDoesNotPrunePreviousArchive` | integration | PASS | 0.03 |
| 122 | `JobManagerTest.RebuildingRealtimePlanRequestsRescanAndHonorsPause` | integration | PASS | 0.45 |
| 123 | `JobManagerTest.EditingAnotherPlanKeepsPendingRealtimeChange` | integration | PASS | 3.28 |
| 124 | `JobManagerTest.RealtimeJobWatchesSelectedSingleFile` | integration | PASS | 3.28 |
| 125 | `JobManagerTest.RealtimeChangeCreatesTarIncrementAndRestoresIt` | integration | PASS | 3.30 |
| 126 | `JobManagerTest.RealtimeContinuesWhenSubdirectoryCannotBeRead` | integration | PASS | 3.27 |
| 127 | `JobManagerTest.RealtimeJobBacksUpSelectedSocket` | integration | PASS | 3.21 |
| 128 | `JobManagerTest.RepeatedManualRequestsQueueOnlyOneRerun` | integration | PASS | 0.03 |
| 129 | `JobManagerTest.RemovingRunningJobDoesNotExecuteQueuedOrphan` | integration | PASS | 0.02 |
| 130 | `JobManagerTest.DisablingJobCancelsPendingRerunAndAllowsReenable` | integration | PASS | 0.03 |
| 131 | `JobManagerTest.RejectsLegacyAndIncompleteJobConfiguration` | integration | PASS | 0.02 |
| 132 | `DirectoryWatcherTest.ReportsAFileSystemChange` | integration | PASS | 0.41 |
| 133 | `DirectoryWatcherTest.ObservesCreateModifyRenameAndDelete` | integration | PASS | 1.02 |
| 134 | `DirectoryWatcherTest.ReopensAndRequestsFullRescanAfterDirectoryReturns` | integration | PASS | 0.48 |
| 135 | `JobPolicy.AcceptsValidPlan` | unit | PASS | 0.01 |
| 136 | `JobPolicy.RejectsEmptyName` | unit | PASS | 0.01 |
| 137 | `JobPolicy.RejectsEmptySources` | unit | PASS | 0.01 |
| 138 | `JobPolicy.RejectsEmptyDestination` | unit | PASS | 0.01 |
| 139 | `JobPolicy.RejectsZeroRetention` | unit | PASS | 0.01 |
| 140 | `JobPolicy.RejectsInvalidCron` | unit | PASS | 0.01 |
| 141 | `JobPolicy.IgnoresCronWhenNotScheduled` | unit | PASS | 0.01 |
| 142 | `JobPolicy.RejectsContradictoryFilter` | unit | PASS | 0.01 |
| 143 | `JobPolicy.RejectsInvalidFormat` | unit | PASS | 0.01 |
| 144 | `JobPolicy.RejectsInvalidCompression` | unit | PASS | 0.01 |
| 145 | `JobPolicy.RejectsRealtimeFullBackup` | unit | PASS | 0.01 |
| 146 | `JobPolicy.AllowsRealtimeIncrementalFormatsAndRejectsInvalidModes` | unit | PASS | 0.01 |
| 147 | `JobPolicy.ValidatesIdentifier` | unit | PASS | 0.01 |
| 148 | `JobPolicy.RejectsScheduleWithoutPossibleDate` | unit | PASS | 0.37 |
| 149 | `JobRunPolicy.RejectsUnavailableExecution` | unit | PASS | 0.01 |
| 150 | `JobRunPolicy.StartsIdleJob` | unit | PASS | 0.01 |
| 151 | `JobRunPolicy.QueuesRunningJob` | unit | PASS | 0.01 |
| 152 | `JobRunPolicy.MergesRepeatedRequest` | unit | PASS | 0.01 |
| 153 | `JobRunPolicy.ExecutesEligiblePendingJob` | unit | PASS | 0.01 |
| 154 | `JobRunPolicy.SuppressesMissingDisabledOrCancelledJobs` | unit | PASS | 0.01 |
| 155 | `CronRules.RejectsInvalidFieldCountsAndRanges` | unit | PASS | 0.01 |
| 156 | `CronRules.MatchesRangesStepsAndWeekdays` | unit | PASS | 0.01 |
| 157 | `CronRules.FindsNextMinuteAndHonorsSearchLimit` | unit | PASS | 0.01 |
| 158 | `CronRules.RestrictedDayAndWeekdayUseOrAndSundaySeven` | unit | PASS | 0.01 |
| 159 | `CronRules.FindsLeapDayBeyondOneYear` | unit | PASS | 0.15 |
| 160 | `CronRules.SkipsNonexistentLocalTimeAtSpringForward` | unit | PASS | 0.01 |
| 161 | `CredentialUnavailableTest` | integration | PASS | 0.02 |
| 162 | `SourceDialogTest` | integration | PASS | 0.12 |
| 163 | `MainWindowLayoutTest` | integration | PASS | 0.45 |
| 164 | `BackupToolTestCliHelp` | integration | PASS | 0.01 |
| 165 | `AcceptanceToolSelfTest` | support | PASS | 0.52 |
| 166 | `DemoComparisonTest` | support | PASS | 0.07 |
| 167 | `DemoVerifySelfTest` | support | PASS | 0.27 |
| 168 | `AcceptanceFixtureGenerate` | support | PASS | 3.12 |
| 169 | `AcceptanceTestCliSmoke` | integration | PASS | 1.40 |
| 170 | `TarInterop` | integration | PASS | 1.23 |
| 171 | `AcceptanceScenarioCatalog` | support | PASS | 0.05 |
