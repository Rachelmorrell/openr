/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>

#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkRoute.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

// Receive socket buffer for netlink socket
constexpr uint32_t kNetlinkSockRecvBuf{1 * 1024 * 1024};

// Maximum number of in-flight messages. `kMinIovMsg` indicates the soft
// requirement for sending bufferred messages.
constexpr size_t kMaxIovMsg{500};
constexpr size_t kMinIovMsg{200};

// Timeout for an ack from kernel for netlink messages we sent. The response for
// big request (e.g. adding 5k routes or getting 10k routes) is sent back in
// multiple parts. If we don't receive any part of below specified timeout, we
// assume kernel is not responsive.
constexpr std::chrono::milliseconds kNlRequestAckTimeout{1000};

// Timeout for an overall netlink request e.g. addRoute, delRoute
constexpr std::chrono::milliseconds kNlRequestTimeout{30000};

/**
 * TODO: Document this class
 * TODO: Move to another file
 */
class NetlinkProtocolSocket {
 public:
  explicit NetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl);

  // create socket and add to eventloop
  void init();

  // receive messages from netlink socket
  void recvNetlinkMessage();

  // send message to netlink socket
  void sendNetlinkMessage();

  ~NetlinkProtocolSocket();

  // Set netlinkSocket Link event callback
  void setLinkEventCB(std::function<void(fbnl::Link, bool)> linkEventCB);

  // Set netlinkSocket Addr event callback
  void setAddrEventCB(std::function<void(fbnl::IfAddress, bool)> addrEventCB);

  // Set netlinkSocket Addr event callback
  void setNeighborEventCB(
      std::function<void(fbnl::Neighbor, bool)> neighborEventCB);

  // process message
  void processMessage(
      const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead);

  // synchronous add route and nexthop paths
  ResultCode addRoute(const openr::fbnl::Route& route);

  // synchronous delete route
  ResultCode deleteRoute(const openr::fbnl::Route& route);

  // synchronous add label route
  ResultCode addLabelRoute(const openr::fbnl::Route& route);

  // synchronous delete label route
  ResultCode deleteLabelRoute(const openr::fbnl::Route& route);

  // synchronous add given list of IP or label routes and their nexthop paths
  ResultCode addRoutes(const std::vector<openr::fbnl::Route> routes);

  // synchronous delete a list of given IP or label routes
  ResultCode deleteRoutes(const std::vector<openr::fbnl::Route> routes);

  // synchronous add interface address
  ResultCode addIfAddress(const openr::fbnl::IfAddress& ifAddr);

  // synchronous delete interface address
  ResultCode deleteIfAddress(const openr::fbnl::IfAddress& ifAddr);

  // get netlink request statuses
  ResultCode getReturnStatus(
      std::vector<folly::Future<int>>& futures,
      std::unordered_set<int> ignoredErrors,
      std::chrono::milliseconds timeout = kNlRequestAckTimeout);

  // get all link interfaces from kernel using Netlink
  std::vector<fbnl::Link> getAllLinks();

  // get all interface addresses from kernel using Netlink
  std::vector<fbnl::IfAddress> getAllIfAddresses();

  // get all neighbors from kernel using Netlink
  std::vector<fbnl::Neighbor> getAllNeighbors();

  // get all routes from kernel using Netlink
  std::vector<fbnl::Route> getAllRoutes();

 private:
  NetlinkProtocolSocket(NetlinkProtocolSocket const&) = delete;
  NetlinkProtocolSocket& operator=(NetlinkProtocolSocket const&) = delete;

  // add netlink message to the queue
  void addNetlinkMessage(std::vector<std::unique_ptr<NetlinkMessage>> nlmsg);

  // process ack message
  void processAck(uint32_t ack, int status);

  fbzmq::ZmqEventLoop* evl_{nullptr};

  // Event callbacks
  std::function<void(fbnl::Link, bool)> linkEventCB_;

  std::function<void(fbnl::IfAddress, bool)> addrEventCB_;

  std::function<void(fbnl::Neighbor, bool)> neighborEventCB_;

  // netlink socket
  int nlSock_{-1};

  // PID Of the endpoint
  uint32_t pid_{UINT_MAX};

  // source addr
  struct sockaddr_nl saddr_;

  // Next available sequence number to use. It is possible to wrap this around,
  // and should be fine. We put hard check to avoid conflict between pending
  // seq number with next sequence number.
  // NOTE: We intentionally start from sequence from 1 and not 0. Notification
  // messages from kernel are not associated with any sequence number and they
  // have `nlmsg_seq` set to `0`. There are two message exchanges over nlSock.
  // 1) REQ-REP (for querying data e.g. links/routes from kernel) -- Here we
  //    send request with non-zero sequence number. The messages sent from
  //    kernel in reply will bear the appropriate sequence numbers
  // 2) PUSH (notification message from kernel) -- This notification is from
  //    kernel on any event. There is no sequence number associated with it and
  //    value of nlh->nlmsg_seq will set to 0.
  uint32_t nextNlSeqNum_{1};

  // Netlink message queue. Every add/del/get call for route/addr/neighbor/link
  // translates into one or more NetlinkMessages. These messages are first
  // stored in the queue and sent to kernel in rate limiting fashion. When ack
  // for in-flight messages is received, subsequent messages are sent.
  std::queue<std::unique_ptr<NetlinkMessage>> msgQueue_;

  // Sequence number to NetlinkMesage request mapping. Each in-flight message
  // sent to kernel, is assigned a unique sequence-number and stored in this
  // map. On receipt of ack from kernel (either success or error) we clear the
  // corresponding entry from this map.
  std::unordered_map<uint32_t, std::shared_ptr<NetlinkMessage>> nlSeqNumMap_;

  // Timer to help keep track of timeout of messages sent to kernel. It also
  // ensures the aliveness of the netlink socket-fd. Timer is
  // - Started when a new message is sent
  // - Reset whenever we receive update about one of the pending ack
  // - Cleared when there is no pending ack in nlSeqNumMap_
  // When timer fires, it is an indication that we didn't receive the ack for
  // one of the entry in nlSeqNoMap, for at-least past kNlRequestAckTimeout
  // time. Netlink socket is re-initiaited on timeout for any of our pending
  // message, and `nlSeqNumMap_` is cleared.
  std::unique_ptr<fbzmq::ZmqTimeout> nlMessageTimer_{nullptr};

  /**
   * We maintain a temporary cache of Link, Address, Neighbor and Routes from
   * the kernel, which are solely used for the getAll... methods. These caches
   * are cleared when we invoke a new getAllLinks/Addresses/Neighbors/Routes
   */
  std::vector<fbnl::Link> linkCache_{};
  std::vector<fbnl::IfAddress> addressCache_{};
  std::vector<fbnl::Neighbor> neighborCache_{};
  std::vector<fbnl::Route> routeCache_{};
};

} // namespace openr::fbnl
