//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_consumer/postinstall_runner_action.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>

#include <base/bind.h>
#include <base/files/file_util.h>
#if BASE_VER < 780000  // Android
#include <base/message_loop/message_loop.h>
#endif  // BASE_VER < 780000
#include <android-base/stringprintf.h>
#if BASE_VER >= 780000  // CrOS
#include <base/task/single_thread_task_executor.h>
#endif  // BASE_VER >= 780000
#include <brillo/message_loops/base_message_loop.h>
#include <brillo/message_loops/message_loop_utils.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "update_engine/common/constants.h"
#include "update_engine/common/fake_boot_control.h"
#include "update_engine/common/fake_hardware.h"
#include "update_engine/common/subprocess.h"
#include "update_engine/common/test_utils.h"
#include "update_engine/common/utils.h"
#include "update_engine/common/mock_dynamic_partition_control.h"

using brillo::MessageLoop;
using chromeos_update_engine::test_utils::ScopedLoopbackDeviceBinder;
using std::string;
using testing::_;
using testing::AtLeast;
using testing::Return;

namespace chromeos_update_engine {

class PostinstActionProcessorDelegate : public ActionProcessorDelegate {
 public:
  PostinstActionProcessorDelegate() = default;
  void ProcessingDone(const ActionProcessor* processor,
                      ErrorCode code) override {
    MessageLoop::current()->BreakLoop();
    processing_done_called_ = true;
  }
  void ProcessingStopped(const ActionProcessor* processor) override {
    MessageLoop::current()->BreakLoop();
    processing_stopped_called_ = true;
  }

  void ActionCompleted(ActionProcessor* processor,
                       AbstractAction* action,
                       ErrorCode code) override {
    if (action->Type() == PostinstallRunnerAction::StaticType()) {
      code_ = code;
      code_set_ = true;
    }
  }

  ErrorCode code_{ErrorCode::kError};
  bool code_set_{false};
  bool processing_done_called_{false};
  bool processing_stopped_called_{false};
};

class MockPostinstallRunnerActionDelegate
    : public PostinstallRunnerAction::DelegateInterface {
 public:
  MOCK_METHOD1(ProgressUpdate, void(double progress));
};

class PostinstallRunnerActionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_.SetAsCurrent();
    async_signal_handler_.Init();
    subprocess_.Init(&async_signal_handler_);
    // These tests use the postinstall files generated by "generate_images.sh"
    // stored in the "disk_ext2_unittest.img" image.
    postinstall_image_ =
        test_utils::GetBuildArtifactsPath("gen/disk_ext2_unittest.img");
    {
      auto mock_dynamic_control =
          std::make_unique<MockDynamicPartitionControl>();
      mock_dynamic_control_ = mock_dynamic_control.get();
      fake_boot_control_.SetDynamicPartitionControl(
          std::move(mock_dynamic_control));
    }
    ON_CALL(*mock_dynamic_control_, FinishUpdate(_))
        .WillByDefault(Return(true));
    ON_CALL(*mock_dynamic_control_, MapAllPartitions())
        .WillByDefault(Return(true));
    ON_CALL(*mock_dynamic_control_, UnmapAllPartitions())
        .WillByDefault(Return(true));
    ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
        .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::NONE)));
  }

  // Setup an action processor and run the PostinstallRunnerAction with a single
  // partition |device_path|, running the |postinstall_program| command from
  // there.
  void RunPostinstallAction(const string& device_path,
                            const string& postinstall_program,
                            bool powerwash_required,
                            bool save_rollback_data);

  void RunPostinstallActionWithInstallPlan(const InstallPlan& install_plan);

 public:
  void ResumeRunningAction() {
    ASSERT_NE(nullptr, postinstall_action_);
    postinstall_action_->ResumeAction();
  }

  void SuspendRunningAction() {
    if (!postinstall_action_ || !postinstall_action_->current_command_ ||
        test_utils::Readlink(android::base::StringPrintf(
            "/proc/%d/fd/0", postinstall_action_->current_command_)) !=
            "/dev/zero") {
      // We need to wait for the postinstall command to start and flag that it
      // is ready by redirecting its input to /dev/zero.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::SuspendRunningAction,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(100));
    } else {
      postinstall_action_->SuspendAction();
      // Schedule to be resumed in a little bit.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::ResumeRunningAction,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(100));
    }
  }

  void CancelWhenStarted() {
    if (!postinstall_action_ || !postinstall_action_->current_command_) {
      // Wait for the postinstall command to run.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::CancelWhenStarted,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(10));
    } else {
      CHECK(processor_);
      // Must |PostDelayedTask()| here to be safe that |FileDescriptorWatcher|
      // doesn't leak memory, do not directly call |StopProcessing()|.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(
              [](ActionProcessor* processor) { processor->StopProcessing(); },
              base::Unretained(processor_)),
          base::TimeDelta::FromMilliseconds(100));
    }
  }

 protected:
#if BASE_VER < 780000  // Android
  base::MessageLoopForIO base_loop_;
  brillo::BaseMessageLoop loop_{&base_loop_};
#else   // CrOS
  base::SingleThreadTaskExecutor base_loop_{base::MessagePumpType::IO};
  brillo::BaseMessageLoop loop_{base_loop_.task_runner()};
#endif  // BASE_VER < 780000
  brillo::AsynchronousSignalHandler async_signal_handler_;
  Subprocess subprocess_;

  // The path to the postinstall sample image.
  string postinstall_image_;

  FakeBootControl fake_boot_control_;
  FakeHardware fake_hardware_;
  MockDynamicPartitionControl* mock_dynamic_control_;
  PostinstActionProcessorDelegate processor_delegate_;

  // The PostinstallRunnerAction delegate receiving the progress updates.
  PostinstallRunnerAction::DelegateInterface* setup_action_delegate_{nullptr};

  // A pointer to the posinstall_runner action and the processor.
  PostinstallRunnerAction* postinstall_action_{nullptr};
  ActionProcessor* processor_{nullptr};
};

void PostinstallRunnerActionTest::RunPostinstallAction(
    const string& device_path,
    const string& postinstall_program,
    bool powerwash_required,
    bool save_rollback_data) {
  InstallPlan::Partition part;
  part.name = "part";
  part.target_path = device_path;
  part.readonly_target_path = device_path;
  part.run_postinstall = true;
  part.postinstall_path = postinstall_program;
  InstallPlan install_plan;
  install_plan.partitions = {part};
  install_plan.download_url = "http://127.0.0.1:8080/update";
  install_plan.powerwash_required = powerwash_required;
  RunPostinstallActionWithInstallPlan(install_plan);
}

void PostinstallRunnerActionTest::RunPostinstallActionWithInstallPlan(
    const chromeos_update_engine::InstallPlan& install_plan) {
  ActionProcessor processor;
  processor_ = &processor;
  auto feeder_action = std::make_unique<ObjectFeederAction<InstallPlan>>();
  feeder_action->set_obj(install_plan);
  auto runner_action = std::make_unique<PostinstallRunnerAction>(
      &fake_boot_control_, &fake_hardware_);
  postinstall_action_ = runner_action.get();
  base::FilePath temp_dir;
  TEST_AND_RETURN(base::CreateNewTempDirectory("postinstall", &temp_dir));
  postinstall_action_->SetMountDir(temp_dir.value());
  runner_action->set_delegate(setup_action_delegate_);
  BondActions(feeder_action.get(), runner_action.get());
  auto collector_action =
      std::make_unique<ObjectCollectorAction<InstallPlan>>();
  BondActions(runner_action.get(), collector_action.get());
  processor.EnqueueAction(std::move(feeder_action));
  processor.EnqueueAction(std::move(runner_action));
  processor.EnqueueAction(std::move(collector_action));
  processor.set_delegate(&processor_delegate_);

  loop_.PostTask(
      FROM_HERE,
      base::Bind(
          [](ActionProcessor* processor) { processor->StartProcessing(); },
          base::Unretained(&processor)));
  loop_.Run();
  ASSERT_FALSE(processor.IsRunning());
  postinstall_action_ = nullptr;
  processor_ = nullptr;
  EXPECT_TRUE(processor_delegate_.processing_stopped_called_ ||
              processor_delegate_.processing_done_called_);
  if (processor_delegate_.processing_done_called_) {
    // Validation check that the code was set when the processor finishes.
    EXPECT_TRUE(processor_delegate_.code_set_);
  }
}

TEST_F(PostinstallRunnerActionTest, ProcessProgressLineTest) {
  PostinstallRunnerAction action(&fake_boot_control_, &fake_hardware_);
  testing::StrictMock<MockPostinstallRunnerActionDelegate> mock_delegate_;
  action.set_delegate(&mock_delegate_);

  action.current_partition_ = 1;
  action.partition_weight_ = {1, 2, 5};
  action.accumulated_weight_ = 1;
  action.total_weight_ = 8;

  // 50% of the second action is 2/8 = 0.25 of the total.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.25));
  action.ProcessProgressLine("global_progress 0.5");
  testing::Mock::VerifyAndClearExpectations(&mock_delegate_);

  // 1.5 should be read as 100%, to catch rounding error cases like 1.000001.
  // 100% of the second is 3/8 of the total.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.375));
  action.ProcessProgressLine("global_progress 1.5");
  testing::Mock::VerifyAndClearExpectations(&mock_delegate_);

  // None of these should trigger a progress update.
  action.ProcessProgressLine("foo_bar");
  action.ProcessProgressLine("global_progress");
  action.ProcessProgressLine("global_progress ");
  action.ProcessProgressLine("global_progress NaN");
  action.ProcessProgressLine("global_progress Exception in ... :)");
}

// Test that postinstall succeeds in the simple case of running the default
// /postinst command which only exits 0.
TEST_F(PostinstallRunnerActionTest, RunAsRootSimpleTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));

  RunPostinstallAction(loop.dev(), kPostinstallDefaultScript, false, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
  EXPECT_TRUE(processor_delegate_.processing_done_called_);

  // Since powerwash_required was false, this should not trigger a powerwash.
  EXPECT_FALSE(fake_hardware_.IsPowerwashScheduled());
  EXPECT_FALSE(fake_hardware_.GetIsRollbackPowerwashScheduled());
}

TEST_F(PostinstallRunnerActionTest, RunAsRootRunSymlinkFileTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(loop.dev(), "bin/postinst_link", false, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}

TEST_F(PostinstallRunnerActionTest, RunAsRootPowerwashRequiredTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  // Run a simple postinstall program but requiring a powerwash.
  RunPostinstallAction(loop.dev(),
                       "bin/postinst_example",
                       /*powerwash_required=*/true,
                       false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);

  // Check that powerwash was scheduled.
  EXPECT_TRUE(fake_hardware_.IsPowerwashScheduled());
  EXPECT_FALSE(fake_hardware_.GetIsRollbackPowerwashScheduled());
}

// Runs postinstall from a partition file that doesn't mount, so it should
// fail.
TEST_F(PostinstallRunnerActionTest, RunAsRootCantMountTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  RunPostinstallAction("/dev/null", kPostinstallDefaultScript, false, false);
  EXPECT_EQ(ErrorCode::kPostInstallMountError, processor_delegate_.code_);

  // In case of failure, Postinstall should not signal a powerwash even if it
  // was requested.
  EXPECT_FALSE(fake_hardware_.IsPowerwashScheduled());
  EXPECT_FALSE(fake_hardware_.GetIsRollbackPowerwashScheduled());
}

TEST_F(PostinstallRunnerActionTest, RunAsRootSkipOptionalPostinstallTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  InstallPlan::Partition part;
  part.name = "part";
  part.target_path = "/dev/null";
  part.readonly_target_path = loop.dev();
  part.run_postinstall = true;
  part.postinstall_path = "non_existent_path";
  part.postinstall_optional = true;
  InstallPlan install_plan;
  install_plan.partitions = {part};
  install_plan.download_url = "http://127.0.0.1:8080/update";

  // Optional postinstalls will be skipped, and the postinstall action succeeds.
  RunPostinstallActionWithInstallPlan(install_plan);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);

  part.postinstall_optional = false;
  install_plan.partitions = {part};
  RunPostinstallActionWithInstallPlan(install_plan);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);
}

// Check that the failures from the postinstall script cause the action to
// fail.
TEST_F(PostinstallRunnerActionTest, RunAsRootErrScriptTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(loop.dev(), "bin/postinst_fail1", false, false);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);
}

// The exit code 3 and 4 are a specials cases that would be reported back to
// UMA with a different error code. Test those cases are properly detected.
TEST_F(PostinstallRunnerActionTest, RunAsRootFirmwareBErrScriptTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(loop.dev(), "bin/postinst_fail3", false, false);
  EXPECT_EQ(ErrorCode::kPostinstallBootedFromFirmwareB,
            processor_delegate_.code_);
}

// Check that you can't specify an absolute path.
TEST_F(PostinstallRunnerActionTest, RunAsRootAbsolutePathNotAllowedTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(loop.dev(), "/etc/../bin/sh", false, false);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);
}

#ifdef __ANDROID__
// Check that the postinstall file is labeled to the postinstall_exec label.
// SElinux labels are only set on Android.
TEST_F(PostinstallRunnerActionTest, RunAsRootCheckFileContextsTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(loop.dev(), "bin/self_check_context", false, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}

// Check that the postinstall file is relabeled to the default postinstall
// label. SElinux labels are only set on Android.
TEST_F(PostinstallRunnerActionTest, RunAsRootCheckDefaultFileContextsTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPostinstallAction(
      loop.dev(), "bin/self_check_default_context", false, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}
#endif  // __ANDROID__

// Check that you can suspend/resume postinstall actions.
TEST_F(PostinstallRunnerActionTest, RunAsRootSuspendResumeActionTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);

  // We need to wait for the child to run and setup its signal handler.
  loop_.PostTask(FROM_HERE,
                 base::Bind(&PostinstallRunnerActionTest::SuspendRunningAction,
                            base::Unretained(this)));
  RunPostinstallAction(loop.dev(), "bin/postinst_suspend", false, false);
  // postinst_suspend returns 0 only if it was suspended at some point.
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
  EXPECT_TRUE(processor_delegate_.processing_done_called_);
}

// Test that we can cancel a postinstall action while it is running.
TEST_F(PostinstallRunnerActionTest, RunAsRootCancelPostinstallActionTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  EXPECT_CALL(*mock_dynamic_control_, MapAllPartitions()).Times(AtLeast(1));

  // Wait for the action to start and then cancel it.
  CancelWhenStarted();
  RunPostinstallAction(loop.dev(), "bin/postinst_suspend", false, false);
  // When canceling the action, the action never finished and therefore we had
  // a ProcessingStopped call instead.
  EXPECT_FALSE(processor_delegate_.code_set_);
  EXPECT_TRUE(processor_delegate_.processing_stopped_called_);
}

// Test that we parse and process the progress reports from the progress
// file descriptor.
TEST_F(PostinstallRunnerActionTest, RunAsRootProgressUpdatesTest) {
  ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag)
      .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  EXPECT_CALL(*mock_dynamic_control_, MapAllPartitions())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_dynamic_control_, FinishUpdate(_)).Times(AtLeast(1));
  testing::StrictMock<MockPostinstallRunnerActionDelegate> mock_delegate_;
  testing::InSequence s;
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0));

  // The postinst_progress program will call with 0.25, 0.5 and 1.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.25));
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.5));
  EXPECT_CALL(mock_delegate_, ProgressUpdate(1.));

  EXPECT_CALL(mock_delegate_, ProgressUpdate(1.));

  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  setup_action_delegate_ = &mock_delegate_;
  RunPostinstallAction(loop.dev(), "bin/postinst_progress", false, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}

}  // namespace chromeos_update_engine
