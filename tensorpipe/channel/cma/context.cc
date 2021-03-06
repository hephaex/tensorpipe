/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/channel/cma/context.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <list>
#include <mutex>

#include <tensorpipe/channel/cma/channel.h>
#include <tensorpipe/channel/error.h>
#include <tensorpipe/channel/helpers.h>
#include <tensorpipe/channel/registry.h>
#include <tensorpipe/common/callback.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/error_macros.h>
#include <tensorpipe/common/optional.h>
#include <tensorpipe/common/queue.h>
#include <tensorpipe/common/system.h>
#include <tensorpipe/proto/channel/cma.pb.h>

namespace tensorpipe {
namespace channel {
namespace cma {

namespace {

const std::string kChannelName{"cma"};

std::string generateDomainDescriptor() {
  std::ostringstream oss;
  auto bootID = getBootID();
  TP_THROW_ASSERT_IF(!bootID) << "Unable to read boot_id";

  // According to the man page of process_vm_readv and process_vm_writev,
  // permission to read from or write to another process is governed by a ptrace
  // access mode PTRACE_MODE_ATTACH_REALCREDS check. This consists in a series
  // of checks, some governed by the CAP_SYS_PTRACE capability, others by the
  // Linux Security Modules (LSMs), but the primary constraint is that the real,
  // effective, and saved-set user IDs of the target match the caller's real
  // user ID, and the same for group IDs. Since channels are bidirectional, we
  // end up needing these IDs to all be the same on both processes.

  // Combine boot ID, effective UID, and effective GID.
  oss << kChannelName;
  oss << ":" << bootID.value();
  // FIXME As domain descriptors are just compared for equality, we only include
  // the effective IDs, but we should abide by the rules above and make sure
  // that they match the real and saved-set ones too.
  oss << "/" << geteuid();
  oss << "/" << getegid();
  return oss.str();
}

std::shared_ptr<Context> makeCmaChannel() {
  return std::make_shared<Context>();
}

TP_REGISTER_CREATOR(TensorpipeChannelRegistry, cma, makeCmaChannel);

} // namespace

class Context::Impl : public Context::PrivateIface,
                      public std::enable_shared_from_this<Context::Impl> {
 public:
  Impl();

  const std::string& domainDescriptor() const;

  std::shared_ptr<channel::Channel> createChannel(
      std::shared_ptr<transport::Connection>,
      Channel::Endpoint);

  ClosingEmitter& getClosingEmitter() override;

  using copy_request_callback_fn = std::function<void(const Error&)>;

  void requestCopy(
      pid_t remotePid,
      void* remotePtr,
      void* localPtr,
      size_t length,
      copy_request_callback_fn fn) override;

  void close();

  void join();

  ~Impl() override = default;

 private:
  struct CopyRequest {
    pid_t remotePid;
    void* remotePtr;
    void* localPtr;
    size_t length;
    copy_request_callback_fn callback;
  };

  std::string domainDescriptor_;
  std::thread thread_;
  Queue<optional<CopyRequest>> requests_;
  std::atomic<bool> closed_{false};
  std::atomic<bool> joined_{false};
  ClosingEmitter closingEmitter_;

  void handleCopyRequests_();
};

Context::Context()
    : channel::Context(kChannelName),
      impl_(std::make_shared<Context::Impl>()) {}

Context::Impl::Impl()
    : domainDescriptor_(generateDomainDescriptor()), requests_(INT_MAX) {
  thread_ = std::thread(&Impl::handleCopyRequests_, this);
}

void Context::close() {
  impl_->close();
}

void Context::Impl::close() {
  if (!closed_.exchange(true)) {
    closingEmitter_.close();
    requests_.push(nullopt);
  }
}

void Context::join() {
  impl_->join();
}

void Context::Impl::join() {
  close();

  if (!joined_.exchange(true)) {
    thread_.join();
    // TP_DCHECK(requests_.empty());
  }
}

Context::~Context() {
  join();
}

ClosingEmitter& Context::Impl::getClosingEmitter() {
  return closingEmitter_;
}

const std::string& Context::domainDescriptor() const {
  return impl_->domainDescriptor();
}

const std::string& Context::Impl::domainDescriptor() const {
  return domainDescriptor_;
}

std::shared_ptr<channel::Channel> Context::createChannel(
    std::shared_ptr<transport::Connection> connection,
    Channel::Endpoint endpoint) {
  return impl_->createChannel(std::move(connection), endpoint);
}

std::shared_ptr<channel::Channel> Context::Impl::createChannel(
    std::shared_ptr<transport::Connection> connection,
    Channel::Endpoint /* unused */) {
  TP_THROW_ASSERT_IF(joined_);
  return std::make_shared<Channel>(
      Channel::ConstructorToken(),
      std::static_pointer_cast<PrivateIface>(shared_from_this()),
      std::move(connection));
}

void Context::Impl::requestCopy(
    pid_t remotePid,
    void* remotePtr,
    void* localPtr,
    size_t length,
    std::function<void(const Error&)> fn) {
  requests_.push(
      CopyRequest{remotePid, remotePtr, localPtr, length, std::move(fn)});
}

void Context::Impl::handleCopyRequests_() {
  setThreadName("TP_CMA_loop");
  while (true) {
    auto maybeRequest = requests_.pop();
    if (!maybeRequest.has_value()) {
      break;
    }
    CopyRequest request = std::move(maybeRequest).value();

    // Perform copy.
    struct iovec local {
      .iov_base = request.localPtr, .iov_len = request.length
    };
    struct iovec remote {
      .iov_base = request.remotePtr, .iov_len = request.length
    };
    auto nread =
        ::process_vm_readv(request.remotePid, &local, 1, &remote, 1, 0);
    if (nread == -1) {
      request.callback(TP_CREATE_ERROR(SystemError, "cma", errno));
    } else if (nread != request.length) {
      request.callback(TP_CREATE_ERROR(ShortReadError, request.length, nread));
    } else {
      request.callback(Error::kSuccess);
    }
  }
}

} // namespace cma
} // namespace channel
} // namespace tensorpipe
