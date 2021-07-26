// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#include "pw_thread_embos/snapshot.h"

#include <string_view>

#include "RTOS.h"
#include "pw_function/function.h"
#include "pw_log/log.h"
#include "pw_protobuf/encoder.h"
#include "pw_status/status.h"
#include "pw_thread/snapshot.h"
#include "pw_thread_embos/util.h"
#include "pw_thread_protos/thread.pwpb.h"

namespace pw::thread::embos {
namespace {

// TODO(amontanez): This might make unit testing codepaths that use this more
// challenging.
inline bool ThreadIsRunning(const OS_TASK& thread) {
  return OS_GetpCurrentTask() == &thread;
}

void CaptureThreadState(const OS_TASK& thread, Thread::StreamEncoder& encoder) {
  if (ThreadIsRunning(thread)) {
    PW_LOG_INFO("Thread state: RUNNING");
    encoder.WriteState(ThreadState::Enum::RUNNING);
    return;
  }

  // One byte is reserved for task status.
  //   - The lowest two bits are for a suspend counter.
  //   - The third-lowest bit is reserved for a "timeout." (ignored here)
  //   - The highest five bits indicate what the task is blocked on if non-zero.
  //
  // Note: embOS thread state is not part of the public API. This may not be
  // correct for all versions. This has been tested on embOS 4.22, and was
  // initially reported for embOS 5.06.
  //
  // Description of how `OS_TASK::Stat` is used by embOS:
  //   https://forum.segger.com/index.php/Thread/6548-ABANDONED-Task-state-values/?postID=23963#post23963
#if OS_VERSION_GENERIC < 42200 || OS_VERSION_GENERIC > 50600
#warning embOS thread state interpretation logic is not verfied as working on this version of embOS
#endif  // OS_VERSION_GENERIC < 42200 || OS_VERSION_GENERIC > 50600

  if ((thread.Stat & 0x3) != 0) {
    PW_LOG_INFO("Thread state: SUSPENDED");
    encoder.WriteState(ThreadState::Enum::SUSPENDED);
  } else if ((thread.Stat & 0xf8) == 0) {
    PW_LOG_INFO("Thread state: READY");
    encoder.WriteState(ThreadState::Enum::READY);
  } else {
    PW_LOG_INFO("Thread state: BLOCKED");
    encoder.WriteState(ThreadState::Enum::BLOCKED);
  }
}

}  // namespace

Status SnapshotThreads(void* running_thread_stack_pointer,
                       SnapshotThreadInfo::StreamEncoder& encoder,
                       ProcessThreadStackCallback& stack_dumper) {
  struct {
    void* running_thread_stack_pointer;
    SnapshotThreadInfo::StreamEncoder* encoder;
    ProcessThreadStackCallback* stack_dumper;
    Status thread_capture_status;
  } ctx;
  ctx.running_thread_stack_pointer = running_thread_stack_pointer;
  ctx.encoder = &encoder;
  ctx.stack_dumper = &stack_dumper;

  ThreadCallback thread_capture_cb([&ctx](const OS_TASK& thread) -> bool {
    Thread::StreamEncoder thread_encoder = ctx.encoder->GetThreadsEncoder();
    ctx.thread_capture_status.Update(
        SnapshotThread(thread,
                       ctx.running_thread_stack_pointer,
                       thread_encoder,
                       *ctx.stack_dumper));
    // Always iterate all threads.
    return true;
  });

  if (Status status = ForEachThread(thread_capture_cb);
      !status.ok() && !status.IsFailedPrecondition()) {
    PW_LOG_ERROR("Failed to iterate threads during snapshot capture: %d",
                 static_cast<int>(status.code()));
  }

  return ctx.thread_capture_status;
}

Status SnapshotThread(const OS_TASK& thread,
                      void* running_thread_stack_pointer,
                      Thread::StreamEncoder& encoder,
                      ProcessThreadStackCallback& thread_stack_callback) {
#if OS_TRACKNAME
  PW_LOG_INFO("Capturing thread info for %s", thread.Name);
  encoder.WriteName(std::as_bytes(std::span(std::string_view(thread.Name))));
#else
  PW_LOG_INFO("Capturing thread info for thread at 0x%08x", &thread);
#endif  // OS_TRACKNAME

  CaptureThreadState(thread, encoder);

#if OS_CHECKSTACK || OS_SUPPORT_MPU
  const StackContext thread_ctx = {
      .thread_name = thread.Name,

      .stack_low_addr = reinterpret_cast<uintptr_t>(thread.pStackBot),

      .stack_high_addr =
          reinterpret_cast<uintptr_t>(thread.pStackBot) + thread.StackSize,

      // If the thread is active, the stack pointer in the TCB is stale.
      .stack_pointer = reinterpret_cast<uintptr_t>(
          ThreadIsRunning(thread) ? running_thread_stack_pointer
                                  : thread.pStack),
  };

  return SnapshotStack(thread_ctx, encoder, thread_stack_callback);
#else
  PW_LOG_INFO("Stack pointer: 0x%08x", running_thread_stack_pointer);
  encoder.WriteStackPointer(reinterpret_cast<uintptr_t>(
      ThreadIsRunning(thread) ? running_thread_stack_pointer : thread.pStack));
  return encoder.status();
#endif  // OS_CHECKSTACK || OS_SUPPORT_MPU
}

}  // namespace pw::thread::embos
