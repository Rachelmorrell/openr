/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.OpenrConfig
namespace py3 openr.thrift

include "BgpConfig.thrift"

struct KvstoreConfig {
  # kvstore
  1: i32 flood_msg_per_sec = 0
  2: i32 flood_msg_burst_size = 0
  3: i32 key_ttl_ms = 300000 # 5min 300*1000
  4: i32 sync_interval_s = 60
  5: i32 ttl_decrement_ms = 1

  6: optional bool set_leaf_node
  7: optional list<string> key_prefix_filters
  8: optional list<string> key_originator_id_filters

  # flood optimization
  9: optional bool enable_flood_optimization
  10: optional bool is_flood_root
  11: optional bool use_flood_optimization
}

struct LinkMonitorConfig {
  1: i32 linkflap_initial_backoff_ms = 60000 # link flap options
  2: i32 linkflap_max_backoff_ms = 300000
  3: bool use_rtt_metric = true
  4: list<string> include_interface_regexes = []
  5: list<string> exclude_interface_regexes = []
  6: list<string> redistribute_interface_regexes = []
}

struct SparkConfig {
  1: i32 neighbor_discovery_port = 6666

  2: i32 hello_time_s = 20
  3: i32 fastinit_hello_time_ms = 500

  4: i32 keepalive_time_s = 2
  5: i32 hold_time_s = 10
  6: i32 graceful_restart_time_s = 30
}

struct WatchdogConfig {
  1: i32 interval_s = 20
  2: i32 threshold_s = 300
}

enum PrefixForwardingType {
  IP = 0
  SR_MPLS = 1
}

enum PrefixForwardingAlgorithm {
  SP_ECMP = 0
  KSP2_ED_ECMP = 1
}

struct PrefixAllocationConfig {
  1: string loopback_interface = "lo"
  2: string seed_prefix
  3: i32 allocate_prefix_len = 128
  4: bool static_prefix_allocation = false
  5: bool set_loopback_addr = false
  6: bool override_loopback_addr = false
}

struct AreaConfig {
  1: string area_id
}

struct OpenrConfig {
  1: string node_name
  2: string domain
  3: list<AreaConfig> areas = []
  # The IP address to bind to
  4: string listen_addr = "::"
  # Port for the OpenR ctrl thrift service
  5: i32 openr_ctrl_port = 2018

  6: optional bool dryrun
  7: optional bool enable_v4
  8: optional bool enable_netlink_fib_handler # netlink fib server
  9: optional bool enable_netlink_system_handler # netlink system server

  # time before decision start compute routes
  10: i32 eor_time_s = -1

  11: PrefixForwardingType prefix_forwarding_type = PrefixForwardingType.IP
  12: PrefixForwardingAlgorithm prefix_forwarding_algorithm = PrefixForwardingAlgorithm.SP_ECMP
  13: optional bool enable_segment_routing
  14: optional i32 prefix_min_nexthop

  # Config for different modules
  15: KvstoreConfig kvstore_config
  16: LinkMonitorConfig link_monitor_config
  17: SparkConfig spark_config
  # Watchdog
  18: bool enable_watchdog = true
  19: optional WatchdogConfig watchdog_config
  # prefix allocation
  20: optional bool enable_prefix_allocation
  21: optional PrefixAllocationConfig prefix_allocation_config

  # bgp
  100: optional bool enable_spr
  102: optional BgpConfig.BgpConfig bgp_config
  103: optional bool bgp_use_igp_metric
}
