/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/channel/basic/channel.h>

#include <algorithm>
#include <list>

#include <tensorpipe/channel/error.h>
#include <tensorpipe/channel/helpers.h>
#include <tensorpipe/common/callback.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/error.h>
#include <tensorpipe/common/error_macros.h>
#include <tensorpipe/proto/channel/basic.pb.h>

namespace tensorpipe {
namespace channel {
namespace basic {

class Channel::Impl : public std::enable_shared_from_this<Channel::Impl> {
 public:
  Impl(
      std::shared_ptr<Context::PrivateIface>,
      std::shared_ptr<transport::Connection>);

  // Called by the channel's constructor.
  void init();

  void send(
      const void* ptr,
      size_t length,
      TDescriptorCallback descriptorCallback,
      TSendCallback callback);

  void recv(
      TDescriptor descriptor,
      void* ptr,
      size_t length,
      TRecvCallback callback);

  void close();

 private:
  std::mutex mutex_;
  std::thread::id currentLoop_{std::thread::id()};
  std::deque<std::function<void()>> pendingTasks_;

  bool inLoop_();
  void deferToLoop_(std::function<void()> fn);

  void sendFromLoop_(
      const void* ptr,
      size_t length,
      TDescriptorCallback descriptorCallback,
      TSendCallback callback);

  // Receive memory region from peer.
  void recvFromLoop_(
      TDescriptor descriptor,
      void* ptr,
      size_t length,
      TRecvCallback callback);

  void initFromLoop_();

  void closeFromLoop_();

  // Arm connection to read next protobuf packet.
  void readPacket_();

  // Called when a protobuf packet was received.
  void onPacket_(const proto::Packet& packet);

  // Called when protobuf packet is a request.
  void onRequest_(const proto::Request& request);

  // Called when protobuf packet is a reply.
  void onReply_(const proto::Reply& reply);

  std::shared_ptr<Context::PrivateIface> context_;
  std::shared_ptr<transport::Connection> connection_;
  Error error_{Error::kSuccess};
  ClosingReceiver closingReceiver_;

  // Increasing identifier for send operations.
  uint64_t id_{0};

  // State capturing a single send operation.
  struct SendOperation {
    const uint64_t id;
    const void* ptr;
    size_t length;
    TSendCallback callback;
  };

  // State capturing a single recv operation.
  struct RecvOperation {
    const uint64_t id;
    void* ptr;
    size_t length;
    TRecvCallback callback;
  };

  std::list<SendOperation> sendOperations_;
  std::list<RecvOperation> recvOperations_;

  // Called if send operation was successful.
  void sendCompleted(const uint64_t);

  // Called if recv operation was successful.
  void recvCompleted(const uint64_t);

  // Helpers to prepare callbacks from transports
  LazyCallbackWrapper<Impl> lazyCallbackWrapper_{*this};
  EagerCallbackWrapper<Impl> eagerCallbackWrapper_{*this};

  // Helper function to process transport error.
  // Shared between read and write callback entry points.
  void handleError_();

  // For some odd reason it seems we need to use a qualified name here...
  template <typename T>
  friend class tensorpipe::LazyCallbackWrapper;
  template <typename T>
  friend class tensorpipe::EagerCallbackWrapper;
};

Channel::Channel(
    ConstructorToken /* unused */,
    std::shared_ptr<Context::PrivateIface> context,
    std::shared_ptr<transport::Connection> connection)
    : impl_(std::make_shared<Impl>(std::move(context), std::move(connection))) {
  impl_->init();
}

Channel::Impl::Impl(
    std::shared_ptr<Context::PrivateIface> context,
    std::shared_ptr<transport::Connection> connection)
    : context_(std::move(context)),
      connection_(std::move(connection)),
      closingReceiver_(context_, context_->getClosingEmitter()) {}

bool Channel::Impl::inLoop_() {
  return currentLoop_ == std::this_thread::get_id();
}

void Channel::Impl::deferToLoop_(std::function<void()> fn) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    pendingTasks_.push_back(std::move(fn));
    if (currentLoop_ != std::thread::id()) {
      return;
    }
    currentLoop_ = std::this_thread::get_id();
  }

  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (pendingTasks_.empty()) {
        currentLoop_ = std::thread::id();
        return;
      }
      task = std::move(pendingTasks_.front());
      pendingTasks_.pop_front();
    }
    task();
  }
}

void Channel::send(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  impl_->send(ptr, length, std::move(descriptorCallback), std::move(callback));
}

void Channel::Impl::send(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  deferToLoop_([this,
                ptr,
                length,
                descriptorCallback{std::move(descriptorCallback)},
                callback{std::move(callback)}]() mutable {
    sendFromLoop_(
        ptr, length, std::move(descriptorCallback), std::move(callback));
  });
}

// Send memory region to peer.
void Channel::Impl::sendFromLoop_(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  TP_DCHECK(inLoop_());
  proto::Descriptor pbDescriptor;

  const auto id = id_++;
  pbDescriptor.set_operation_id(id);
  sendOperations_.emplace_back(
      SendOperation{id, ptr, length, std::move(callback)});

  descriptorCallback(Error::kSuccess, saveDescriptor(pbDescriptor));
}

// Receive memory region from peer.
void Channel::recv(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  impl_->recv(std::move(descriptor), ptr, length, std::move(callback));
}

void Channel::Impl::recv(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  deferToLoop_([this,
                descriptor{std::move(descriptor)},
                ptr,
                length,
                callback{std::move(callback)}]() mutable {
    recvFromLoop_(std::move(descriptor), ptr, length, std::move(callback));
  });
}

void Channel::Impl::recvFromLoop_(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  TP_DCHECK(inLoop_());
  proto::Descriptor pbDescriptor;
  loadDescriptor(pbDescriptor, descriptor);
  const auto id = pbDescriptor.operation_id();

  recvOperations_.emplace_back(
      RecvOperation{id, ptr, length, std::move(callback)});

  // Ask peer to start sending data now that we have a target pointer.
  auto packet = std::make_shared<proto::Packet>();
  proto::Request* pbRequest = packet->mutable_request();
  pbRequest->set_operation_id(id);
  connection_->write(
      *packet, lazyCallbackWrapper_([packet](Impl& /* unused */) {}));
  return;
}

void Channel::Impl::init() {
  deferToLoop_([this]() { initFromLoop_(); });
}

void Channel::Impl::initFromLoop_() {
  TP_DCHECK(inLoop_());
  closingReceiver_.activate(*this);
  readPacket_();
}

void Channel::close() {
  impl_->close();
}

Channel::~Channel() {
  close();
}

void Channel::Impl::close() {
  deferToLoop_([this]() { closeFromLoop_(); });
}

void Channel::Impl::closeFromLoop_() {
  TP_DCHECK(inLoop_());
  if (!error_) {
    error_ = TP_CREATE_ERROR(ChannelClosedError);
    handleError_();
  }
}

void Channel::Impl::readPacket_() {
  TP_DCHECK(inLoop_());
  auto packet = std::make_shared<proto::Packet>();
  connection_->read(*packet, lazyCallbackWrapper_([packet](Impl& impl) {
    impl.onPacket_(*packet);
  }));
}

void Channel::Impl::onPacket_(const proto::Packet& packet) {
  TP_DCHECK(inLoop_());
  if (packet.has_request()) {
    onRequest_(packet.request());
  } else if (packet.has_reply()) {
    onReply_(packet.reply());
  } else {
    TP_THROW_ASSERT() << "Packet is not a request nor a reply.";
  }

  // Wait for next request.
  readPacket_();
}

void Channel::Impl::onRequest_(const proto::Request& request) {
  TP_DCHECK(inLoop_());
  // Find the send operation matching the request's operation ID.
  const auto id = request.operation_id();
  auto it = std::find_if(
      sendOperations_.begin(), sendOperations_.end(), [id](const auto& op) {
        return op.id == id;
      });
  TP_THROW_ASSERT_IF(it == sendOperations_.end())
      << "Expected send operation with ID " << id << " to exist.";

  // Reference to operation.
  auto& op = *it;

  // Write packet announcing the payload.
  auto pbPacketOut = std::make_shared<proto::Packet>();
  proto::Reply* pbReply = pbPacketOut->mutable_reply();
  pbReply->set_operation_id(id);
  connection_->write(
      *pbPacketOut, lazyCallbackWrapper_([pbPacketOut](Impl& /* unused */) {}));

  // Write payload.
  connection_->write(op.ptr, op.length, eagerCallbackWrapper_([id](Impl& impl) {
                       impl.sendCompleted(id);
                     }));
}

void Channel::Impl::onReply_(const proto::Reply& reply) {
  TP_DCHECK(inLoop_());
  // Find the recv operation matching the reply's operation ID.
  const auto id = reply.operation_id();
  auto it = std::find_if(
      recvOperations_.begin(), recvOperations_.end(), [id](const auto& op) {
        return op.id == id;
      });
  TP_THROW_ASSERT_IF(it == recvOperations_.end())
      << "Expected recv operation with ID " << id << " to exist.";

  // Reference to operation.
  auto& op = *it;

  // Read payload into specified memory region.
  connection_->read(
      op.ptr,
      op.length,
      eagerCallbackWrapper_(
          [id](Impl& impl, const void* /* unused */, size_t /* unused */) {
            impl.recvCompleted(id);
          }));
}

void Channel::Impl::sendCompleted(const uint64_t id) {
  TP_DCHECK(inLoop_());
  auto it = std::find_if(
      sendOperations_.begin(), sendOperations_.end(), [id](const auto& op) {
        return op.id == id;
      });
  TP_THROW_ASSERT_IF(it == sendOperations_.end())
      << "Expected send operation with ID " << id << " to exist.";

  // Move operation to stack.
  auto op = std::move(*it);
  sendOperations_.erase(it);

  op.callback(error_);
}

void Channel::Impl::recvCompleted(const uint64_t id) {
  TP_DCHECK(inLoop_());
  auto it = std::find_if(
      recvOperations_.begin(), recvOperations_.end(), [id](const auto& op) {
        return op.id == id;
      });
  TP_THROW_ASSERT_IF(it == recvOperations_.end())
      << "Expected recv operation with ID " << id << " to exist.";

  // Move operation to stack.
  auto op = std::move(*it);
  recvOperations_.erase(it);

  op.callback(error_);
}

void Channel::Impl::handleError_() {
  TP_DCHECK(inLoop_());
  // Close the connection so that all current operations will be aborted. This
  // will cause their callbacks to be invoked, and only then we'll invoke ours.
  connection_->close();
}

} // namespace basic
} // namespace channel
} // namespace tensorpipe
