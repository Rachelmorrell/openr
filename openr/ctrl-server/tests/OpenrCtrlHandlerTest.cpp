/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fbzmq/service/monitor/ZmqMonitor.h>
#include <fbzmq/zmq/Context.h>
#include <folly/init/Init.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/Constants.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/health-checker/HealthChecker.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/link-monitor/tests/MockNetlinkSystemHandler.h>
#include <openr/prefix-manager/PrefixManager.h>

using namespace openr;

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    const std::unordered_set<std::string> acceptablePeerNames;

    // Create zmq-monitor
    zmqMonitor = std::make_unique<fbzmq::ZmqMonitor>(
        monitorSubmitUrl_, "inproc://monitor_pub_url", context_);
    zmqMonitorThread_ = std::thread([&]() { zmqMonitor->run(); });

    // Create PersistentStore
    persistentStore = std::make_unique<PersistentStore>(
        nodeName,
        "/tmp/openr-ctrl-handler-test.bin",
        context_,
        Constants::kPersistentStoreInitialBackoff,
        Constants::kPersistentStoreMaxBackoff,
        true /* dryrun */);
    persistentStoreUrl_ = PersistentStoreUrl{persistentStore->inprocCmdUrl};
    persistentStoreThread_ = std::thread([&]() { persistentStore->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::PERSISTENT_STORE] =
        persistentStore;

    // Create KvStore module
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context_,
        nodeName,
        std::chrono::seconds(1),
        std::chrono::seconds(1),
        std::unordered_map<std::string, thrift::PeerSpec>(),
        folly::none,
        folly::none,
        std::chrono::milliseconds(1),
        true /* enableFloodOptimization */,
        true /* isFloodRoot */);
    kvStoreWrapper->run();
    moduleTypeToEvl_[thrift::OpenrModuleType::KVSTORE] =
        kvStoreWrapper->getKvStore();

    // Create Decision module
    decision = std::make_shared<Decision>(
        nodeName, /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        false, /* enableOrderedFib */
        false, /* bgpDryRun */
        false, /* bgpUseIgpMetric */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        folly::none,
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        DecisionPubUrl{decisionPubUrl_},
        monitorSubmitUrl_,
        context_);
    decisionThread_ = std::thread([&]() { decision->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::DECISION] = decision;

    // Create Fib module
    fib = std::make_shared<Fib>(
        nodeName,
        -1, /* thrift port */
        true, /* dryrun */
        false, /* periodic syncFib */
        true, /* enableSegmentRouting */
        false, /* enableOrderedFib */
        std::chrono::seconds(2),
        false, /* waitOnDecision */
        DecisionPubUrl{"inproc://decision-pub"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        context_);
    fibThread_ = std::thread([&]() { fib->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::FIB] = fib;

    // Create HealthChecker module
    healthChecker = std::make_shared<HealthChecker>(
        nodeName,
        thrift::HealthCheckOption::PingNeighborOfNeighbor,
        uint32_t{50}, /* health check pct */
        uint16_t{0}, /* make sure it binds to some open port */
        std::chrono::seconds(2),
        folly::none, /* maybeIpTos */
        AdjacencyDbMarker{Constants::kAdjDbMarker.str()},
        PrefixDbMarker{Constants::kPrefixDbMarker.str()},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        monitorSubmitUrl_,
        context_);
    healthCheckerThread_ = std::thread([&]() { healthChecker->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::HEALTH_CHECKER] = healthChecker;

    // Create PrefixManager module
    prefixManager = std::make_shared<PrefixManager>(
        nodeName,
        persistentStoreUrl_,
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        monitorSubmitUrl_,
        PrefixDbMarker{Constants::kPrefixDbMarker.str()},
        false /* create per prefix keys */,
        false,
        std::chrono::seconds(0),
        Constants::kKvStoreDbTtl,
        context_);
    prefixManagerThread_ = std::thread([&]() { prefixManager->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::PREFIX_MANAGER] = prefixManager;

    // Create MockNetlinkSystemHandler
    mockNlHandler =
        std::make_shared<MockNetlinkSystemHandler>(context_, platformPubUrl_);
    systemServer = std::make_shared<apache::thrift::ThriftServer>();
    systemServer->setNumIOWorkerThreads(1);
    systemServer->setNumAcceptThreads(1);
    systemServer->setPort(0);
    systemServer->setInterface(mockNlHandler);
    systemThriftThread.start(systemServer);

    // Create LinkMonitor
    re2::RE2::Options regexOpts;
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add("po.*", &regexErr);
    includeRegexList->Compile();

    linkMonitor = std::make_shared<LinkMonitor>(
        context_,
        nodeName,
        systemThriftThread.getAddress()->getPort(),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        nullptr,
        nullptr, // redistribute interface name
        std::vector<thrift::IpPrefix>{},
        false /* useRttMetric */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type mpls */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{Constants::kAdjDbMarker.str()},
        sparkCmdUrl_,
        sparkReportUrl_,
        monitorSubmitUrl_,
        persistentStoreUrl_,
        false,
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
        platformPubUrl_,
        lmPubUrl_,
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);
    linkMonitorThread_ = std::thread([&]() { linkMonitor->run(); });
    moduleTypeToEvl_[thrift::OpenrModuleType::LINK_MONITOR] = linkMonitor;

    // Create main-event-loop
    mainEvlThread_ = std::thread([&]() { mainEvl_.run(); });

    // we need to set thread manager as the createStreamGenerator will
    // call getBlockingThreadManager to execute
    tm = apache::thrift::concurrency::ThreadManager::newSimpleThreadManager(
        1, false);
    tm->threadFactory(
        std::make_shared<apache::thrift::concurrency::PosixThreadFactory>());
    tm->start();

    // Create open/r handler
    handler = std::make_unique<OpenrCtrlHandler>(
        nodeName,
        acceptablePeerNames,
        moduleTypeToEvl_,
        monitorSubmitUrl_,
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        mainEvl_,
        context_);
    handler->setThreadManager(tm.get());
  }

  void
  TearDown() override {
    // ATTN: moduleTypeToEvl_ maintains <shared_ptr> of OpenrEventLoop.
    //       Must cleanup. Otherwise, there will be additional ref count and
    //       cause OpenrEventLoop binding to the existing addr.
    moduleTypeToEvl_.clear();

    mainEvl_.stop();
    mainEvlThread_.join();
    handler.reset();
    tm->join();

    linkMonitor->stop();
    linkMonitorThread_.join();

    persistentStore->stop();
    persistentStoreThread_.join();

    prefixManager->stop();
    prefixManagerThread_.join();

    mockNlHandler->stop();
    systemThriftThread.stop();

    healthChecker->stop();
    healthCheckerThread_.join();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    kvStoreWrapper->stop();

    zmqMonitor->stop();
    zmqMonitorThread_.join();
  }

  thrift::PeerSpec
  createPeerSpec(const std::string& pubUrl, const std::string& cmdUrl) {
    thrift::PeerSpec peerSpec;
    peerSpec.pubUrl = pubUrl;
    peerSpec.cmdUrl = cmdUrl;
    return peerSpec;
  }

  thrift::PrefixEntry
  createPrefixEntry(const std::string& prefix, thrift::PrefixType prefixType) {
    thrift::PrefixEntry prefixEntry;
    prefixEntry.prefix = toIpPrefix(prefix);
    prefixEntry.type = prefixType;
    return prefixEntry;
  }

 private:
  const MonitorSubmitUrl monitorSubmitUrl_{"inproc://monitor-submit-url"};
  const DecisionPubUrl decisionPubUrl_{"inproc://decision-pub"};
  const SparkCmdUrl sparkCmdUrl_{"inproc://spark-req"};
  const SparkReportUrl sparkReportUrl_{"inproc://spark-report"};
  const PlatformPublisherUrl platformPubUrl_{"inproc://platform-pub-url"};
  const LinkMonitorGlobalPubUrl lmPubUrl_{"inproc://link-monitor-pub-url"};
  PersistentStoreUrl persistentStoreUrl_;

  fbzmq::Context context_;
  fbzmq::ZmqEventLoop mainEvl_;
  std::thread zmqMonitorThread_;
  std::thread decisionThread_;
  std::thread fibThread_;
  std::thread healthCheckerThread_;
  std::thread prefixManagerThread_;
  std::thread persistentStoreThread_;
  std::thread linkMonitorThread_;
  std::thread mainEvlThread_;
  apache::thrift::util::ScopedServerThread systemThriftThread;

 public:
  const std::string nodeName{"thanos@universe"};
  std::unique_ptr<fbzmq::ZmqMonitor> zmqMonitor;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::shared_ptr<HealthChecker> healthChecker;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;
  std::shared_ptr<apache::thrift::ThriftServer> systemServer;
  std::shared_ptr<PrefixManager> prefixManager;
  std::shared_ptr<PersistentStore> persistentStore;
  std::shared_ptr<LinkMonitor> linkMonitor;
  std::shared_ptr<apache::thrift::concurrency::ThreadManager> tm;
  std::unique_ptr<OpenrCtrlHandler> handler;
  std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
      moduleTypeToEvl_;
};

TEST_F(OpenrCtrlFixture, getMyNodeName) {
  auto ret = handler->semifuture_getMyNodeName().get();
  ASSERT_NE(nullptr, ret);
  EXPECT_EQ(nodeName, *ret);
}

TEST_F(OpenrCtrlFixture, PrefixManagerApis) {
  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("10.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("11.0.0.0/8", thrift::PrefixType::LOOPBACK),
        createPrefixEntry("20.0.0.0/8", thrift::PrefixType::BGP),
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_advertisePrefixes(
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("21.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_withdrawPrefixes(
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto ret =
        handler->semifuture_withdrawPrefixesByType(thrift::PrefixType::LOOPBACK)
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler
                   ->semifuture_syncPrefixesByType(
                       thrift::PrefixType::BGP,
                       std::make_unique<std::vector<thrift::PrefixEntry>>(
                           std::move(prefixes)))
                   .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    const std::vector<thrift::PrefixEntry> prefixes{
        createPrefixEntry("23.0.0.0/8", thrift::PrefixType::BGP),
    };
    auto ret = handler->semifuture_getPrefixes().get();
    EXPECT_NE(nullptr, ret);
    EXPECT_EQ(prefixes, *ret);
  }

  {
    auto ret =
        handler->semifuture_getPrefixesByType(thrift::PrefixType::LOOPBACK)
            .get();
    EXPECT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }
}

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    auto ret = handler->semifuture_getRouteDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(nodeName))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    const std::string testNode("avengers@universe");
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(testNode))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(testNode, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  {
    auto ret = handler->semifuture_getPerfDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
  }
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    auto ret = handler->semifuture_getDecisionAdjacencyDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }

  {
    auto ret = handler->semifuture_getDecisionPrefixDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }
}

TEST_F(OpenrCtrlFixture, HealthCheckerApis) {
  {
    auto ret = handler->semifuture_getHealthCheckerInfo().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->nodeInfo.size());
  }
}

TEST_F(OpenrCtrlFixture, KvStoreApis) {
  thrift::KeyVals keyVals;
  keyVals["key1"] = createThriftValue(1, "node1", std::string("value1"));
  keyVals["key11"] = createThriftValue(1, "node1", std::string("value11"));
  keyVals["key111"] = createThriftValue(1, "node1", std::string("value111"));
  keyVals["key2"] = createThriftValue(1, "node1", std::string("value2"));
  keyVals["key22"] = createThriftValue(1, "node1", std::string("value22"));
  keyVals["key222"] = createThriftValue(1, "node1", std::string("value222"));
  keyVals["key3"] = createThriftValue(1, "node3", std::string("value3"));
  keyVals["key33"] = createThriftValue(1, "node33", std::string("value33"));
  keyVals["key333"] = createThriftValue(1, "node33", std::string("value333"));

  //
  // Key set/get
  //

  {
    thrift::KeySetParams setParams;
    setParams.keyVals = keyVals;

    auto ret = handler
                   ->semifuture_setKvStoreKeyVals(
                       std::make_unique<thrift::KeySetParams>(setParams))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);

    setParams.solicitResponse = false;
    ret = handler
              ->semifuture_setKvStoreKeyVals(
                  std::make_unique<thrift::KeySetParams>(setParams))
              .get();
    ASSERT_TRUE(folly::Unit() == ret);

    ret = handler
              ->semifuture_setKvStoreKeyValsOneWay(
                  std::make_unique<thrift::KeySetParams>(setParams))
              .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    std::vector<std::string> filterKeys{"key11", "key2"};
    auto ret = handler
                   ->semifuture_getKvStoreKeyVals(
                       std::make_unique<std::vector<std::string>>(filterKeys))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(2, ret->keyVals.size());
    EXPECT_EQ(keyVals.at("key2"), ret->keyVals["key2"]);
    EXPECT_EQ(keyVals.at("key11"), ret->keyVals["key11"]);
  }

  {
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    auto ret = handler
                   ->semifuture_getKvStoreKeyValsFiltered(
                       std::make_unique<thrift::KeyDumpParams>(params))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->keyVals.size());
    EXPECT_EQ(keyVals.at("key3"), ret->keyVals["key3"]);
    EXPECT_EQ(keyVals.at("key33"), ret->keyVals["key33"]);
    EXPECT_EQ(keyVals.at("key333"), ret->keyVals["key333"]);
  }

  {
    thrift::KeyDumpParams params;
    params.prefix = "key3";
    params.originatorIds.insert("node3");

    auto ret = handler
                   ->semifuture_getKvStoreHashFiltered(
                       std::make_unique<thrift::KeyDumpParams>(params))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->keyVals.size());
    auto value3 = keyVals.at("key3");
    value3.value = folly::none;
    auto value33 = keyVals.at("key33");
    value33.value = folly::none;
    auto value333 = keyVals.at("key333");
    value333.value = folly::none;
    EXPECT_EQ(value3, ret->keyVals["key3"]);
    EXPECT_EQ(value33, ret->keyVals["key33"]);
    EXPECT_EQ(value333, ret->keyVals["key333"]);
  }

  //
  // Dual and Flooding APIs
  //

  {
    thrift::DualMessages messages;
    auto ret = handler
                   ->semifuture_processKvStoreDualMessage(
                       std::make_unique<thrift::DualMessages>(messages))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    thrift::FloodTopoSetParams params;
    params.rootId = nodeName;
    auto ret = handler
                   ->semifuture_updateFloodTopologyChild(
                       std::make_unique<thrift::FloodTopoSetParams>(params))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getSpanningTreeInfos().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(1, ret->infos.size());
    ASSERT_NE(ret->infos.end(), ret->infos.find(nodeName));
    EXPECT_EQ(0, ret->counters.neighborCounters.size());
    EXPECT_EQ(1, ret->counters.rootCounters.size());
    EXPECT_EQ(nodeName, ret->floodRootId);
    EXPECT_EQ(0, ret->floodPeers.size());

    thrift::SptInfo sptInfo = ret->infos.at(nodeName);
    EXPECT_EQ(0, sptInfo.cost);
    ASSERT_TRUE(sptInfo.parent.hasValue());
    EXPECT_EQ(nodeName, sptInfo.parent.value());
    EXPECT_EQ(0, sptInfo.children.size());
  }

  //
  // Peers APIs
  //

  const thrift::PeersMap peers{
      {"peer1", createPeerSpec("inproc:://peer1-pub", "inproc://peer1-cmd")},
      {"peer2", createPeerSpec("inproc:://peer2-pub", "inproc://peer2-cmd")},
      {"peer3", createPeerSpec("inproc:://peer3-pub", "inproc://peer3-cmd")}};

  {
    auto ret = handler
                   ->semifuture_addUpdateKvStorePeers(
                       std::make_unique<thrift::PeersMap>(peers))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getKvStorePeers().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(3, ret->size());
    EXPECT_EQ(peers.at("peer1"), (*ret)["peer1"]);
    EXPECT_EQ(peers.at("peer2"), (*ret)["peer2"]);
    EXPECT_EQ(peers.at("peer3"), (*ret)["peer3"]);
  }

  {
    std::vector<std::string> peersToDel{"peer2"};
    auto ret = handler
                   ->semifuture_deleteKvStorePeers(
                       std::make_unique<std::vector<std::string>>(peersToDel))
                   .get();
    ASSERT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getKvStorePeers().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(2, ret->size());
    EXPECT_EQ(peers.at("peer1"), (*ret)["peer1"]);
    EXPECT_EQ(peers.at("peer3"), (*ret)["peer3"]);
  }

  //
  // Subscribe API
  //

  {
    std::atomic<int> received{0};
    const std::string key{"snoop-key"};
    auto subscription = handler->subscribeKvStore().subscribe(
        [&received, key](thrift::Publication&& pub) {
          EXPECT_EQ(1, pub.keyVals.size());
          ASSERT_EQ(1, pub.keyVals.count(key));
          EXPECT_EQ("value1", pub.keyVals.at(key).value.value());
          EXPECT_EQ(received + 1, pub.keyVals.at(key).version);
          received++;
        });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(1, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(2, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(3, "node1", std::string("value1")));

    // Check we should receive-3 updates
    while (received < 3) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }

  //
  // Subscribe and Get API
  //

  {
    std::atomic<int> received{0};
    const std::string key{"snoop-key"};
    auto responseAndSubscription =
        handler->semifuture_subscribeAndGetKvStore().get();

    // Expect 10 keys in the initial dump
    EXPECT_EQ(10, responseAndSubscription.response.keyVals.size());

    auto subscription =
        std::move(responseAndSubscription.stream)
            .subscribe([&received, key](thrift::Publication&& pub) {
              EXPECT_EQ(1, pub.keyVals.size());
              ASSERT_EQ(1, pub.keyVals.count(key));
              EXPECT_EQ("value1", pub.keyVals.at(key).value.value());
              EXPECT_EQ(received + 4, pub.keyVals.at(key).version);
              received++;
            });
    EXPECT_EQ(1, handler->getNumKvStorePublishers());
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(4, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(5, "node1", std::string("value1")));
    kvStoreWrapper->setKey(
        key, createThriftValue(6, "node1", std::string("value1")));

    // Check we should receive-3 updates
    while (received < 3) {
      std::this_thread::yield();
    }

    // Cancel subscription
    subscription.cancel();
    std::move(subscription).detach();

    // Wait until publisher is destroyed
    while (handler->getNumKvStorePublishers() != 0) {
      std::this_thread::yield();
    }
  }
}

TEST_F(OpenrCtrlFixture, LinkMonitorApis) {
  // create an interface
  mockNlHandler->sendLinkEvent("po1011", 100, true);

  {
    auto ret = handler->semifuture_setNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_unsetNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_setInterfaceOverload(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_unsetInterfaceOverload(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_setInterfaceMetric(std::move(ifName), 110).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto ret =
        handler->semifuture_unsetInterfaceMetric(std::move(ifName)).get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto adjName = std::make_unique<std::string>("night@king");
    auto ret = handler
                   ->semifuture_setAdjacencyMetric(
                       std::move(ifName), std::move(adjName), 110)
                   .get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ifName = std::make_unique<std::string>("po1011");
    auto adjName = std::make_unique<std::string>("night@king");
    auto ret = handler
                   ->semifuture_unsetAdjacencyMetric(
                       std::move(ifName), std::move(adjName))
                   .get();
    EXPECT_TRUE(folly::Unit() == ret);
  }

  {
    auto ret = handler->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_FALSE(ret->isOverloaded);
    EXPECT_EQ(1, ret->interfaceDetails.size());
  }

  {
    auto ret = handler->semifuture_getOpenrVersion().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_LE(ret->lowestSupportedVersion, ret->version);
  }

  {
    auto ret = handler->semifuture_getBuildInfo().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_NE("", ret->buildMode);
  }
}

TEST_F(OpenrCtrlFixture, PersistentStoreApis) {
  {
    auto key = std::make_unique<std::string>("key1");
    auto value = std::make_unique<std::string>("value1");
    auto ret =
        handler->semifuture_setConfigKey(std::move(key), std::move(value))
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key2");
    auto value = std::make_unique<std::string>("value2");
    auto ret =
        handler->semifuture_setConfigKey(std::move(key), std::move(value))
            .get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key1");
    auto ret = handler->semifuture_eraseConfigKey(std::move(key)).get();
    EXPECT_EQ(folly::Unit(), ret);
  }

  {
    auto key = std::make_unique<std::string>("key2");
    auto ret = handler->semifuture_getConfigKey(std::move(key)).get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ("value2", *ret);
  }

  {
    auto key = std::make_unique<std::string>("key1");
    auto ret = handler->semifuture_getConfigKey(std::move(key));
    EXPECT_THROW(std::move(ret).get(), thrift::OpenrError);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
