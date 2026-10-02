/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "task_profiles.h"
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/test_utils.h>
#include <gtest/gtest.h>
#include <mntent.h>
#include <processgroup/processgroup.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <thread>

using ::android::base::ERROR;
using ::android::base::LogFunction;
using ::android::base::LogId;
using ::android::base::LogSeverity;
using ::android::base::SetLogger;
using ::android::base::Split;
using ::android::base::VERBOSE;
using ::testing::TestWithParam;
using ::testing::Values;

namespace {

bool IsCgroupV2MountedRw() {
    std::unique_ptr<FILE, int (*)(FILE*)> mnts(setmntent("/proc/mounts", "re"), endmntent);
    if (!mnts) {
        LOG(ERROR) << "Failed to open /proc/mounts";
        return false;
    }
    struct mntent* mnt;
    while ((mnt = getmntent(mnts.get()))) {
        if (strcmp(mnt->mnt_type, "cgroup2") != 0) {
            continue;
        }
        const std::vector<std::string> options = Split(mnt->mnt_opts, ",");
        return std::count(options.begin(), options.end(), "ro") == 0;
    }
    return false;
}

class ScopedLogCapturer {
  public:
    struct log_args {
        LogId log_buffer_id;
        LogSeverity severity;
        std::string tag;
        std::string file;
        unsigned int line;
        std::string message;
    };

    // Constructor. Installs a new logger and saves the currently active logger.
    ScopedLogCapturer() {
        saved_severity_ = SetMinimumLogSeverity(android::base::VERBOSE);
        saved_logger_ = SetLogger([this](LogId log_buffer_id, LogSeverity severity, const char* tag,
                                         const char* file, unsigned int line, const char* message) {
            if (saved_logger_) {
                saved_logger_(log_buffer_id, severity, tag, file, line, message);
            }
            log_.emplace_back(log_args{.log_buffer_id = log_buffer_id,
                                       .severity = severity,
                                       .tag = tag,
                                       .file = file,
                                       .line = line,
                                       .message = message});
        });
    }
    // Destructor. Restores the original logger and log level.
    ~ScopedLogCapturer() {
        SetLogger(std::move(saved_logger_));
        SetMinimumLogSeverity(saved_severity_);
    }
    ScopedLogCapturer(const ScopedLogCapturer&) = delete;
    ScopedLogCapturer& operator=(const ScopedLogCapturer&) = delete;
    // Returns the logged lines.
    const std::vector<log_args>& Log() const { return log_; }

  private:
    LogSeverity saved_severity_;
    LogFunction saved_logger_;
    std::vector<log_args> log_;
};

// cgroup attribute at the top level of the cgroup hierarchy.
class ProfileAttributeMock : public IProfileAttribute {
  public:
    ProfileAttributeMock(const std::string& file_name) : file_name_(file_name) {}
    ~ProfileAttributeMock() override = default;
    void Reset(const CgroupControllerWrapper&, const std::string&, const std::string&) override {
        CHECK(false);
    }
    const CgroupControllerWrapper* controller() const override {
        CHECK(false);
        return {};
    }
    const std::string& file_name() const override { return file_name_; }
    bool GetPathForProcess(uid_t, pid_t pid, std::string* path) const override {
        return GetPathForTask(pid, path);
    }
    bool GetPathForTask(int, std::string* path) const override {
#ifdef __ANDROID__
        CHECK(CgroupGetControllerPath(CGROUPV2_HIERARCHY_NAME, path));
        CHECK_GT(path->length(), 0);
        if (path->rbegin()[0] != '/') {
            *path += "/";
        }
#else
        // Not Android.
        *path = "/sys/fs/cgroup/";
#endif
        *path += file_name_;
        return true;
    };

    bool GetPathForUID(uid_t, std::string*) const override { return false; }

  private:
    const std::string file_name_;
};

struct TestParam {
    const char* attr_name;
    const char* attr_value;
    bool optional_attr;
    bool result;
    LogSeverity log_severity;
    const char* log_prefix;
    const char* log_suffix;
};

class SetAttributeFixture : public TestWithParam<TestParam> {
  public:
    ~SetAttributeFixture() = default;
};

TEST_P(SetAttributeFixture, SetAttribute) {
    // Treehugger runs host tests inside a container either without cgroupv2
    // support or with the cgroup filesystem mounted read-only.
    if (!IsCgroupV2MountedRw()) {
        GTEST_SKIP();
        return;
    }
    const TestParam params = GetParam();
    ScopedLogCapturer captured_log;
    ProfileAttributeMock pa(params.attr_name);
    SetAttributeAction a(&pa, params.attr_value, params.optional_attr);
    EXPECT_EQ(a.ExecuteForProcess(getuid(), getpid()), params.result);
    auto log = captured_log.Log();
    if (params.log_prefix || params.log_suffix) {
        ASSERT_EQ(log.size(), 1);
        EXPECT_EQ(log[0].severity, params.log_severity);
        if (params.log_prefix) {
            EXPECT_EQ(log[0].message.find(params.log_prefix), 0);
        }
        if (params.log_suffix) {
            EXPECT_NE(log[0].message.find(params.log_suffix), std::string::npos);
        }
    } else {
        ASSERT_EQ(log.size(), 0);
    }
}

class TaskProfileFixture : public TestWithParam<TestParam> {
  public:
    ~TaskProfileFixture() = default;
};

class RecordingAction : public ProfileAction {
  public:
    explicit RecordingAction(bool result = true) : result_(result) {}

    const char* Name() const override { return "Recording"; }
    bool ExecuteForProcess(uid_t, pid_t) const override {
        ++process_executions;
        return result_;
    }
    bool ExecuteForTask(pid_t) const override {
        ++task_executions;
        return result_;
    }
    void EnableResourceCaching(ResourceCacheType cache_type) override {
        ++cache_enables[cache_type];
    }
    void DropResourceCaching(ResourceCacheType cache_type) override {
        ++cache_drops[cache_type];
    }

    mutable std::atomic<int> process_executions = 0;
    mutable std::atomic<int> task_executions = 0;
    std::atomic<int> cache_enables[RCT_COUNT] = {};
    std::atomic<int> cache_drops[RCT_COUNT] = {};

  private:
    bool result_;
};

TEST(TaskProfileTest, ResourceCachesAreIndependentAndConcurrencySafe) {
    TaskProfile profile("test_profile");
    auto action = std::make_unique<RecordingAction>();
    RecordingAction* action_ptr = action.get();
    profile.Add(std::move(action));

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&profile]() {
            profile.EnableResourceCaching(ProfileAction::RCT_TASK);
            profile.EnableResourceCaching(ProfileAction::RCT_PROCESS);
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(action_ptr->cache_enables[ProfileAction::RCT_TASK], 1);
    EXPECT_EQ(action_ptr->cache_enables[ProfileAction::RCT_PROCESS], 1);

    profile.DropResourceCaching(ProfileAction::RCT_TASK);
    EXPECT_EQ(action_ptr->cache_drops[ProfileAction::RCT_TASK], 1);
    EXPECT_EQ(action_ptr->cache_drops[ProfileAction::RCT_PROCESS], 0);

    // Dropping one cache type must not prevent the other from being dropped.
    profile.DropResourceCaching(ProfileAction::RCT_PROCESS);
    EXPECT_EQ(action_ptr->cache_drops[ProfileAction::RCT_PROCESS], 1);
}

TEST(TaskProfileTest, AggregatePreservesOrderAndPropagatesFailure) {
    auto failing_profile = std::make_shared<TaskProfile>("failing");
    auto failing_action = std::make_unique<RecordingAction>(false);
    RecordingAction* failing_action_ptr = failing_action.get();
    failing_profile->Add(std::move(failing_action));

    auto succeeding_profile = std::make_shared<TaskProfile>("succeeding");
    auto succeeding_action = std::make_unique<RecordingAction>();
    RecordingAction* succeeding_action_ptr = succeeding_action.get();
    succeeding_profile->Add(std::move(succeeding_action));

    TaskProfile aggregate("aggregate");
    aggregate.Add(std::make_unique<ApplyProfileAction>(
            std::vector<std::shared_ptr<TaskProfile>>{failing_profile, succeeding_profile}));

    EXPECT_FALSE(aggregate.ExecuteForTask(getpid()));
    EXPECT_EQ(failing_action_ptr->task_executions, 1);
    EXPECT_EQ(succeeding_action_ptr->task_executions, 1);
}

TEST(TaskProfileTest, WriteFileProcessApplicationPropagatesWriteFailure) {
    WriteFileAction action("/no/such/task-profile-test-file", "", "<pid>", false);

    EXPECT_FALSE(action.ExecuteForProcess(getuid(), getpid()));
}

TEST(TaskProfileTest, NormalSchedulerPolicyWithoutNiceHandlesFailure) {
    SetSchedulerPolicyAction action(SCHED_OTHER);

    EXPECT_FALSE(action.ExecuteForTask(-1));
}

TEST_P(TaskProfileFixture, TaskProfile) {
    // Treehugger runs host tests inside a container without cgroupv2 support.
    if (!IsCgroupV2MountedRw()) {
        GTEST_SKIP();
        return;
    }
    const TestParam params = GetParam();
    ProfileAttributeMock pa(params.attr_name);
    // Test simple profile with one action
    std::shared_ptr<TaskProfile> tp = std::make_shared<TaskProfile>("test_profile");
    tp->Add(std::make_unique<SetAttributeAction>(&pa, params.attr_value, params.optional_attr));
    EXPECT_EQ(tp->IsValidForProcess(getuid(), getpid()), params.result);
    EXPECT_EQ(tp->IsValidForTask(getpid()), params.result);
    // Test aggregate profile
    TaskProfile tp2("meta_profile");
    std::vector<std::shared_ptr<TaskProfile>> profiles = {tp};
    tp2.Add(std::make_unique<ApplyProfileAction>(profiles));
    EXPECT_EQ(tp2.IsValidForProcess(getuid(), getpid()), params.result);
    EXPECT_EQ(tp2.IsValidForTask(getpid()), params.result);
}

class CompactMemcgActionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        process_path_ = ConvertUidPidToPath(temp_dir_.path, getuid(), getpid(), true);
        ASSERT_TRUE(std::filesystem::create_directories(process_path_));
        current_path_ = process_path_ + "/memory.current";
        reclaim_path_ = process_path_ + "/memory.reclaim";
    }

    void WriteCurrent(const std::string& value) {
        ASSERT_TRUE(android::base::WriteStringToFile(value, current_path_));
    }

    void CreateReclaimFile() { ASSERT_TRUE(android::base::WriteStringToFile("", reclaim_path_)); }

    TemporaryDir temp_dir_;
    std::string process_path_;
    std::string current_path_;
    std::string reclaim_path_;
};

TEST_F(CompactMemcgActionTest, MissingMemoryReclaimIsInvalid) {
    WriteCurrent("4096\n");
    CompactMemcgAction action(CompactMemcgAction::FULL, temp_dir_.path);

    EXPECT_FALSE(action.IsValidForProcess(getuid(), getpid()));
}

TEST_F(CompactMemcgActionTest, MissingMemoryReclaimDegradesCleanlyAtExecution) {
    WriteCurrent("4096\n");
    CompactMemcgAction action(CompactMemcgAction::FULL, temp_dir_.path);
    ScopedLogCapturer captured_log;

    EXPECT_TRUE(action.ExecuteForProcess(getuid(), getpid()));
    for (const auto& log : captured_log.Log()) {
        EXPECT_LT(log.severity, ERROR) << log.message;
    }
}

TEST_F(CompactMemcgActionTest, FullReclaimWritesCurrentUsage) {
    WriteCurrent("4096\n");
    CreateReclaimFile();
    CompactMemcgAction action(CompactMemcgAction::FULL, temp_dir_.path);

    EXPECT_TRUE(action.IsValidForProcess(getuid(), getpid()));
    EXPECT_TRUE(action.ExecuteForProcess(getuid(), getpid()));

    std::string value;
    ASSERT_TRUE(android::base::ReadFileToString(reclaim_path_, &value));
    EXPECT_EQ(value, "4096");
}

TEST_F(CompactMemcgActionTest, AnonReclaimUsesOptionalSwappinessSyntax) {
    WriteCurrent("8192\n");
    CreateReclaimFile();
    CompactMemcgAction action(CompactMemcgAction::ANON, temp_dir_.path);

    EXPECT_TRUE(action.IsValidForProcess(getuid(), getpid()));
    EXPECT_TRUE(action.ExecuteForProcess(getuid(), getpid()));

    std::string value;
    ASSERT_TRUE(android::base::ReadFileToString(reclaim_path_, &value));
    EXPECT_EQ(value, "8192 swappiness=200");
}

TEST_F(CompactMemcgActionTest, SupportedOptionalSyntaxIsNotReprobed) {
    WriteCurrent("8192\n");
    CreateReclaimFile();
    CompactMemcgAction action(CompactMemcgAction::ANON, temp_dir_.path);

    ASSERT_TRUE(action.IsValidForProcess(getuid(), getpid()));
    ASSERT_TRUE(android::base::WriteStringToFile("sentinel", reclaim_path_));
    EXPECT_TRUE(action.IsValidForProcess(getuid(), getpid()));

    std::string value;
    ASSERT_TRUE(android::base::ReadFileToString(reclaim_path_, &value));
    EXPECT_EQ(value, "sentinel");
}

TEST_F(CompactMemcgActionTest, FileReclaimUsesOptionalSwappinessSyntax) {
    WriteCurrent("16384\n");
    CreateReclaimFile();
    CompactMemcgAction action(CompactMemcgAction::FILE, temp_dir_.path);

    EXPECT_TRUE(action.IsValidForProcess(getuid(), getpid()));
    EXPECT_TRUE(action.ExecuteForProcess(getuid(), getpid()));

    std::string value;
    ASSERT_TRUE(android::base::ReadFileToString(reclaim_path_, &value));
    EXPECT_EQ(value, "16384 swappiness=0");
}

TEST_F(CompactMemcgActionTest, MissingMemoryCurrentRemainsAnError) {
    CreateReclaimFile();
    CompactMemcgAction action(CompactMemcgAction::FULL, temp_dir_.path);
    ScopedLogCapturer captured_log;

    EXPECT_FALSE(action.ExecuteForProcess(getuid(), getpid()));
    ASSERT_EQ(captured_log.Log().size(), 1U);
    EXPECT_EQ(captured_log.Log()[0].severity, ERROR);
    EXPECT_EQ(captured_log.Log()[0].message.find("Failed to read"), 0U);
}

TEST_F(CompactMemcgActionTest, GenuineReclaimWriteFailureRemainsAnError) {
    WriteCurrent("4096\n");
    ASSERT_TRUE(std::filesystem::create_directory(reclaim_path_));
    CompactMemcgAction action(CompactMemcgAction::FULL, temp_dir_.path);
    ScopedLogCapturer captured_log;

    EXPECT_FALSE(action.ExecuteForProcess(getuid(), getpid()));
    ASSERT_EQ(captured_log.Log().size(), 1U);
    EXPECT_EQ(captured_log.Log()[0].severity, ERROR);
    EXPECT_EQ(captured_log.Log()[0].message.find("Could not write"), 0U);
}

// Test the four combinations of optional_attr {false, true} and cgroup attribute { does not exist,
// exists }.
INSTANTIATE_TEST_SUITE_P(
        SetAttributeTestSuite, SetAttributeFixture,
        Values(
                // Test that attempting to write into a non-existing cgroup attribute fails and also
                // that an error message is logged.
                TestParam{.attr_name = "no-such-attribute",
                          .attr_value = ".",
                          .optional_attr = false,
                          .result = false,
                          .log_severity = ERROR,
                          .log_prefix = "No such cgroup attribute"},
                // Test that attempting to write into an optional non-existing cgroup attribute
                // results in the return value 'true' and also that no messages are logged.
                TestParam{.attr_name = "no-such-attribute",
                          .attr_value = ".",
                          .optional_attr = true,
                          .result = true},
                // Test that attempting to write an invalid value into an existing optional cgroup
                // attribute fails and also that it causes an error
                // message to be logged.
                TestParam{.attr_name = "cgroup.procs",
                          .attr_value = "-1",
                          .optional_attr = true,
                          .result = false,
                          .log_severity = ERROR,
                          .log_prefix = "Failed to write",
                          .log_suffix = geteuid() == 0 ? "Invalid argument" : "Permission denied"},
                // Test that attempting to write into an existing optional read-only cgroup
                // attribute fails and also that it causes an error message to be logged.
                TestParam{
                        .attr_name = "cgroup.controllers",
                        .attr_value = ".",
                        .optional_attr = false,
                        .result = false,
                        .log_severity = ERROR,
                        .log_prefix = "Failed to write",
                        .log_suffix = geteuid() == 0 ? "Invalid argument" : "Permission denied"}));

// Test TaskProfile IsValid calls.
INSTANTIATE_TEST_SUITE_P(
        TaskProfileTestSuite, TaskProfileFixture,
        Values(
                // Test operating on non-existing cgroup attribute fails.
                TestParam{.attr_name = "no-such-attribute",
                          .attr_value = ".",
                          .optional_attr = false,
                          .result = false},
                // Test operating on optional non-existing cgroup attribute succeeds.
                TestParam{.attr_name = "no-such-attribute",
                          .attr_value = ".",
                          .optional_attr = true,
                          .result = true},
                // Test operating on existing cgroup attribute succeeds.
                TestParam{.attr_name = "cgroup.procs",
                          .attr_value = ".",
                          .optional_attr = false,
                          .result = true},
                // Test operating on optional existing cgroup attribute succeeds.
                TestParam{.attr_name = "cgroup.procs",
                          .attr_value = ".",
                          .optional_attr = true,
                          .result = true}));
}  // namespace
