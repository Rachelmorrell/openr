/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/tests/OpenrThriftServerWrapper.h"

namespace openr {

OpenrThriftServerWrapper::OpenrThriftServerWrapper(
    std::string const& nodeName,
    Decision* decision,
    Fib* fib,
    KvStore* kvStore,
    LinkMonitor* linkMonitor,
    PersistentStore* configStore,
    PrefixManager* prefixManager,
    MonitorSubmitUrl const& monitorSubmitUrl,
    fbzmq::Context& context)
    : nodeName_(nodeName),
      monitorSubmitUrl_(monitorSubmitUrl),
      context_(context),
      decision_(decision),
      fib_(fib),
      kvStore_(kvStore),
      linkMonitor_(linkMonitor),
      configStore_(configStore),
      prefixManager_(prefixManager) {
  CHECK(!nodeName_.empty());
}

void
OpenrThriftServerWrapper::run() {
  // Create main-event-loop
  evbThread_ = std::thread([&]() { evb_.run(); });
  evb_.waitUntilRunning();

  // create openrCtrlHandler
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    openrCtrlHandler_ = std::make_shared<OpenrCtrlHandler>(
        nodeName_,
        std::unordered_set<std::string>{},
        &evb_,
        decision_,
        fib_,
        kvStore_,
        linkMonitor_,
        configStore_,
        prefixManager_,
        monitorSubmitUrl_,
        context_);
  });

  // setup openrCtrlThrift server for client to connect to
  std::shared_ptr<apache::thrift::ThriftServer> server =
      std::make_shared<apache::thrift::ThriftServer>();
  server->setNumIOWorkerThreads(1);
  server->setNumAcceptThreads(1);
  server->setPort(0);
  server->setInterface(openrCtrlHandler_);
  openrCtrlThriftServerThread_.start(std::move(server));

  LOG(INFO) << "Successfully started openr-ctrl thrift server";
}

void
OpenrThriftServerWrapper::stop() {
  // ATTN: it is user's responsibility to close the queue passed
  //       to OpenrThrifyServerWrapper before calling stop()
  openrCtrlHandler_.reset();
  evb_.stop();
  evbThread_.join();
  openrCtrlThriftServerThread_.stop();

  LOG(INFO) << "Successfully stopped openr-ctrl thrift server";
}

} // namespace openr
