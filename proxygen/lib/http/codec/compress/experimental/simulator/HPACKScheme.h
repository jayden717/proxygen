/*
 *  Copyright (c) 2017-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "proxygen/lib/http/codec/compress/experimental/simulator/CompressionScheme.h"
#include <proxygen/lib/http/codec/compress/HPACKCodec.h>
#include <proxygen/lib/http/codec/compress/HPACKQueue.h>

namespace proxygen { namespace compress {


/**
 * Compression scheme for HPACK with a prepended sequence number
 */
class HPACKScheme : public CompressionScheme {
 public:
  explicit HPACKScheme(CompressionSimulator* sim)
      : CompressionScheme(sim) {
    client_.setEncodeHeadroom(2);
  }

  ~HPACKScheme() {
    CHECK_EQ(serverQueue_.getQueuedBytes(), 0);
  }

  // HPACK has no ACKs
  std::unique_ptr<Ack> getAck(uint16_t seqn) override { return nullptr; }
  void recvAck(std::unique_ptr<Ack> ack) override {}

  std::pair<bool, std::unique_ptr<folly::IOBuf>> encode(
    std::vector<compress::Header> allHeaders,
    SimStats& stats) override {
    auto block = client_.encode(allHeaders);
    block->prepend(sizeof(uint16_t));
    folly::io::RWPrivateCursor c(block.get());
    c.writeBE<uint16_t>(index++);
    stats.uncompressed += client_.getEncodedSize().uncompressed;
    stats.compressed += client_.getEncodedSize().compressed;
    // OOO is never allowed
    return {false, std::move(block)};
  }

  void decode(bool /*allowOOO*/, std::unique_ptr<folly::IOBuf> encodedReq,
              SimStats& stats, SimStreamingCallback& callback) override {
    folly::io::Cursor cursor(encodedReq.get());
    auto seqn = cursor.readBE<uint16_t>();
    callback.seqn = seqn;
    VLOG(1) << "Decoding request=" << callback.requestIndex << " header seqn="
            << seqn;
    auto len = cursor.totalLength();
    encodedReq->trimStart(sizeof(uint16_t));
    serverQueue_.enqueueHeaderBlock(seqn,
                                    std::move(encodedReq),
                                    len, &callback, false);
    callback.maybeMarkHolDelay();
    if (serverQueue_.getQueuedBytes() > stats.maxQueueBufferBytes) {
      stats.maxQueueBufferBytes = serverQueue_.getQueuedBytes();
    }
  }

  uint32_t getHolBlockCount() const override {
    return serverQueue_.getHolBlockCount();
  }

  HPACKCodec client_{TransportDirection::UPSTREAM};
  HPACKCodec server_{TransportDirection::DOWNSTREAM};
  HPACKQueue serverQueue_{server_};
};
}}