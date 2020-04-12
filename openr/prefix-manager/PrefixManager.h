/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>

#include <openr/common/Util.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {

class PrefixManager final : public OpenrEventLoop {
 public:
  PrefixManager(
      const std::string& nodeId,
      const PersistentStoreUrl& persistentStoreUrl,
      const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      const PrefixDbMarker& prefixDbMarker,
      bool createIpPrefix,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      const std::chrono::seconds prefixHoldTime,
      const std::chrono::milliseconds ttlKeyInKvStore,
      fbzmq::Context& zmqContext);

  // disable copying
  PrefixManager(PrefixManager const&) = delete;
  PrefixManager& operator=(PrefixManager const&) = delete;

  // get prefix add counter
  int64_t getPrefixAddCounter();

  // get prefix withdraw counter
  int64_t getPrefixWithdrawCounter();

 private:
  // Update persistent store with non-ephemeral prefix entries
  void persistPrefixDb();

  // Update kvstore with both ephemeral and non-ephemeral prefixes
  void updateKvStore();

  // update all IP keys in KvStore
  void updateKvStorePrefixKeys();

  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& request) override;

  // helpers to modify prefix db, returns true if the db is modified
  bool addOrUpdatePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixesByType(thrift::PrefixType type);
  // replace all prefixes of @type w/ @prefixes
  bool syncPrefixesByType(
      thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  // Determine if any prefix entry is persistent (non-ephemeral) in input
  bool isAnyInputPrefixPersistent(
      const std::vector<thrift::PrefixEntry>& prefixes) const;

  // Determine if any prefix entry is persistent (non-ephemeral) in prefixMap_
  bool isAnyExistingPrefixPersistent(
      const std::vector<thrift::PrefixEntry>& prefixes) const;

  // Determine if any prefix entry is persistent (non-ephemeral) by type in
  // prefixMap_
  bool isAnyExistingPrefixPersistentByType(thrift::PrefixType type) const;

  // Submit internal state counters to monitor
  void submitCounters();

  // prefix counter for a given key
  int64_t getCounter(const std::string& key);

  // key prefix callback
  void processKeyPrefixUpdate(
      const std::string& key, folly::Optional<thrift::Value> value) noexcept;

  // add prefix entry in kvstore
  void advertisePrefix(const thrift::PrefixEntry& prefixEntry);

  // called when withdrawing a prefix, add prefix DB into kvstore with
  // delete prefix DB flag set to true
  void advertisePrefixWithdraw(const thrift::PrefixEntry& prefixEntry);

  // serialize prefixDb. This also adds miscellaneous information like perf
  // events
  std::string serializePrefixDb(thrift::PrefixDatabase&& prefixDb);

  // this node name
  const std::string nodeId_;

  // client to interact with ConfigStore
  PersistentStoreClient configStoreClient_;

  const PrefixDbMarker prefixDbMarker_;

  // create IP keys
  bool perPrefixKeys_{false};

  // enable convergence performance measurement for Adjacencies update
  const bool enablePerfMeasurement_{false};

  // Hold timepoint. Prefix database will not be advertised until we pass this
  // timepoint.
  std::chrono::steady_clock::time_point prefixHoldUntilTimePoint_;

  // Throttled version of updateKvStore. It batches up multiple calls and
  // send them in one go!
  std::unique_ptr<fbzmq::ZmqThrottle> updateKvStoreThrottled_;

  // TTL for a key in the key value store
  const std::chrono::milliseconds ttlKeyInKvStore_;

  // kvStoreClient for persisting our prefix db
  KvStoreClient kvStoreClient_;

  // the current prefix db this node is advertising
  std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry> prefixMap_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // IP perfixes to advertisze to kvstore (either add or delete)
  std::vector<std::pair<thrift::IpPrefix, thrift::PrefixType>>
      prefixesToUpdate_{};
}; // PrefixManager

} // namespace openr
