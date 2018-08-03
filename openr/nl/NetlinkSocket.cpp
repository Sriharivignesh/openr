#include "NetlinkSocket.h"
#include "NetlinkException.h"

namespace {
const folly::StringPiece kRouteObjectStr("route/route");
const folly::StringPiece kLinkObjectStr("route/link");

// Socket buffer size for netlink sockets we create
// We use 2MB, default is 32KB
const size_t kNlSockRecvBuf{2 * 1024 * 1024};

} // anonymous namespace

namespace openr {
namespace fbnl {

NetlinkSocket::NetlinkSocket(
  fbzmq::ZmqEventLoop* evl,
  std::unique_ptr<EventsHandler> handler)
  : evl_(evl),
    handler_(std::move(handler)) {
  CHECK(evl_ != nullptr) << "Missing event loop.";
  CHECK(handler_ != nullptr) << "Missing subscription handler.";

  // Create netlink socket for only notification subscription
  subSock_ = nl_socket_alloc();
  CHECK(subSock_ != nullptr) << "Failed to create netlink socket.";

  // Create netlink socket for periodic refresh of our caches
  reqSock_ = nl_socket_alloc();
  CHECK(reqSock_ != nullptr) << "Failed to create netlink socket.";

  int err = nl_connect(reqSock_, NETLINK_ROUTE);
  CHECK_EQ(err, 0) << "Failed to connect nl socket. Error " << nl_geterror(err);

  // Create cache manager using notification socket
  err = nl_cache_mngr_alloc(
    subSock_, NETLINK_ROUTE, NL_AUTO_PROVIDE, &cacheManager_);
  CHECK_EQ(err, 0)
    << "Failed to create cache manager. Error: " << nl_geterror(err);

  // Set high buffers on netlink socket (especially on sub socket) so that
  // bulk events can also be received
  err = nl_socket_set_buffer_size(reqSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on reqSock_";
  err = nl_socket_set_buffer_size(subSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on subSock_";


  // Request a route cache to be created and registered with cache manager
  // route event handler is provided which has this object as opaque data so
  // we can get object state back in this static callback
  err = nl_cache_mngr_add(
    cacheManager_, kRouteObjectStr.data(), routeCacheCB, this, &routeCache_);
  if (err != 0 || !routeCache_) {
    CHECK(false) << "Failed to add neighbor cache to manager. Error: "
                 << nl_geterror(err);
  }

  // Add link cache
  err = nl_cache_mngr_add(
      cacheManager_, kLinkObjectStr.data(), linkCacheCB, this, &linkCache_);
  if (err != 0 || !linkCache_) {
    CHECK(false)
      << "Failed to add link cache to manager. Error: " << nl_geterror(err);
  }

  // Get socket FD to monitor for updates
  int socketFd = nl_cache_mngr_get_fd(cacheManager_);
  CHECK_NE(socketFd, -1) << "Failed to get socket fd";

  // Anytime this socket has data, have libnl process it
  // Our registered handlers will be invoked..
  evl_->addSocketFd(socketFd, POLLIN, [this](int) noexcept {
    int lambdaErr = nl_cache_mngr_data_ready(cacheManager_);
    if (lambdaErr < 0) {
      LOG(ERROR) << "Error processing data on netlink socket. Error: "
      << nl_geterror(lambdaErr);
    } else {
      VLOG(2) << "Processed " << lambdaErr << " netlink messages.";
    }
  });
}

NetlinkSocket::~NetlinkSocket() {
  VLOG(2) << "NetlinkSocket destroy cache";

  evl_->removeSocketFd(nl_cache_mngr_get_fd(cacheManager_));

  // Manager will release our caches internally
  nl_cache_mngr_free(cacheManager_);
  nl_socket_free(subSock_);
  nl_socket_free(reqSock_);

  routeCache_ = nullptr;
  linkCache_ = nullptr;
  cacheManager_ = nullptr;
  subSock_ = nullptr;
  reqSock_ = nullptr;
}

void NetlinkSocket::routeCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  reinterpret_cast<NetlinkSocket*>(data)->handleRouteEvent(obj, action, true);
}

void NetlinkSocket::handleRouteEvent(
    nl_object* obj, int action, bool runHandler) noexcept {
  CHECK_NOTNULL(obj);
  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && (objectStr != kRouteObjectStr)) {
    LOG(ERROR)
      << "Invalid nl_object type expect route/route, actual: " << objectStr;
    return;
  }
  struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
  try {
    doUpdateRouteCache(routeObj, action);
  } catch (const std::exception& ex) {
    LOG(ERROR) << "UpdateCacheFailed";
  }

  if (runHandler && eventFlags_[NEIGH_EVENT]) {
    RouteBuilder builder;
    EventVariant event = builder.buildFromObject(routeObj);
    handler_->handleEvent(action, event);
  }
}

void NetlinkSocket::linkCacheCB(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  reinterpret_cast<NetlinkSocket*>(data)->handleLinkEvent(obj, action, true);
}

void NetlinkSocket::handleLinkEvent(
     struct nl_object* , int , bool) noexcept {
  // TODO handl link events in subscription implementation
}

void NetlinkSocket::doUpdateRouteCache(struct rtnl_route* obj, int action) {
  RouteBuilder builder;
  auto route = builder.buildFromObject(obj);
  // Skip cached route entries and any routes not in the main table
  int flags = route.getFlags().hasValue() ? route.getFlags().value() : 0;
  if (route.getRouteTable() != RT_TABLE_MAIN || flags & RTM_F_CLONED) {
    return;
  }

  uint8_t protocol = route.getProtocolId();
  // Multicast routes do not belong to our proto
  // Save it in our local copy and move on
  const folly::CIDRNetwork& prefix = route.getDestination();
  if (prefix.first.isMulticast()) {
    if (route.getNextHops().size() != 1) {
      LOG(ERROR) << "Unexpected nextHops for multicast address: "
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    auto maybeIfIndex = route.getNextHops()[0].getIfIndex();
    if (!maybeIfIndex.hasValue()) {
      LOG(ERROR) << "Invalid NextHop"
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    const std::string& ifName = getIfName(maybeIfIndex.value()).get();
    auto key = std::make_pair(prefix, ifName);
    auto& mcastRoutes = mcastRoutesCache_[protocol];

    mcastRoutes.erase(key);
    if (NL_ACT_DEL != action) {
      mcastRoutes.emplace(std::make_pair(key, std::move(route)));
    }
    return;
  }

  // Handle link scope routes
  if (route.getScope() == RT_SCOPE_LINK) {
    if (route.getNextHops().size() != 1) {
      LOG(ERROR) << "Unexpected nextHops for link scope route: "
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    auto maybeIfIndex = route.getNextHops()[0].getIfIndex();
    if (!maybeIfIndex.hasValue()) {
      LOG(ERROR) << "Invalid NextHop"
                 << folly::IPAddress::networkToString(prefix);
      return;
    }
    const std::string& ifName = getIfName(maybeIfIndex.value()).get();
    auto key = std::make_pair(prefix, ifName);
    auto& linkRoutes = linkRoutesCache_[protocol];

    linkRoutes.erase(key);
    if (NL_ACT_DEL != action) {
      linkRoutes.emplace(std::make_pair(key, std::move(route)));
    }
    return;
  }

  // Ideally link-local routes should never be programmed
  if (prefix.first.isLinkLocal()) {
    return;
  }

  auto& unicastRoutes = unicastRoutesCache_[protocol];
  unicastRoutes.erase(prefix);
  if (NL_ACT_DEL != action) {
    unicastRoutes.emplace(std::make_pair(prefix, std::move(route)));
  }
}

folly::Future<folly::Unit> NetlinkSocket::addRoute(Route route) {
  auto prefix = route.getDestination();
  VLOG(3) << "NetlinkSocket add route"
          << folly::IPAddress::networkToString(prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this, p = std::move(promise),
        dest = std::move(prefix),
        r = std::move(route)]() mutable {
        try {
          uint8_t type = r.getType();
          switch (type) {
            case RTN_UNICAST:
            doAddUpdateUnicastRoute(std::move(r));
            break;
            case RTN_MULTICAST:
            doAddMulticastRoute(std::move(r));
            break;
            default:
            throw NetlinkException(
              folly::sformat("Unsupported route type {}", (int)type));
          }
          p.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding routes to "
                     << folly::IPAddress::networkToString(dest)
                     << ". Exception: " << folly::exceptionStr(ex);
          p.setException(ex);
        }
      });
  return future;
}

void NetlinkSocket::doAddUpdateUnicastRoute(Route route) {
  checkUnicastRoute(route);
  const auto& dest = route.getDestination();
  if (dest.first.isMulticast() || dest.first.isLinkLocal()) {
    throw NetlinkException(
      folly::sformat("Invalid unicast route type for: {}",
      folly::IPAddress::networkToString(dest)));
  }

  // Create new set of nexthops to be programmed. Existing + New ones
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  bool isV4 = dest.first.isV4();
  auto iter = unicastRoutes.find(dest);
  // Same route
  if (iter != unicastRoutes.end() && iter->second == route) {
    return;
  }

  if (isV4) {
    int err =
      rtnl_route_add(reqSock_, route.fromNetlinkRoute(), NLM_F_REPLACE);
    if (0 != err) {
      throw NetlinkException(folly::sformat(
          "Could not add V4 Route to: {} Error: {}",
          folly::IPAddress::networkToString(dest),
          nl_geterror(err)));
    }
  } else {
    // We need to explicitly add new V6 routes & remove old routes
    // With IPv6, if new route being requested has different properties
    // (like gateway or metric or..) the existing one will not be replaced,
    // instead a new route will be created, which may cause underlying kernel
    // crash when releasing netdevices
    if (iter != unicastRoutes.end()) {
      int err = rtnl_route_delete(reqSock_, iter->second.fromNetlinkRoute(), 0);
      if (0 != err && -NLE_OBJ_NOTFOUND != err) {
        throw NetlinkException(
          folly::sformat("Failed to delete route {} Error: {}",
          folly::IPAddress::networkToString(dest),
          nl_geterror(err)));
      }
    }
    int err = rtnl_route_add(reqSock_, route.fromNetlinkRoute(), 0);
    if (0 != err) {
      throw NetlinkException(
        folly::sformat("Could not add V6 Route to: {} Error: {}",
                       folly::IPAddress::networkToString(dest),
                       nl_geterror(err)));
    }
  }

  // Cache new nexthops in our local-cache if everything is good
  unicastRoutes.erase(dest);
  unicastRoutes.emplace(std::make_pair(dest, std::move(route)));
}

folly::Future<folly::Unit>
NetlinkSocket::delRoute(Route route) {
  VLOG(3) << "NetlinkSocket deleting unicast route";
  auto prefix = route.getDestination();
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        p = std::move(promise),
        r = std::move(route),
        dest = std::move(prefix)
      ]() mutable {
        try {
          uint8_t type = r.getType();
          switch (type) {
            case RTN_UNICAST:
            doDeleteUnicastRoute(std::move(r));
            break;
            case RTN_MULTICAST:
            doDeleteMulticastRoute(std::move(r));
            break;
            default:
            throw NetlinkException(
              folly::sformat("Unsupported route type {}", (int)type));
          }
          p.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error deleting routes to "
                     << folly::IPAddress::networkToString(dest)
                     << " Error: " << folly::exceptionStr(ex);
          p.setException(ex);
        }
      });
  return future;
}

void NetlinkSocket::checkUnicastRoute(const Route &route) {
  const auto& prefix = route.getDestination();
  if (prefix.first.isMulticast() || prefix.first.isLinkLocal()) {
    throw NetlinkException(
      folly::sformat("Invalid unicast route type for: {}",
      folly::IPAddress::networkToString(prefix)));
  }
}

void NetlinkSocket::doDeleteUnicastRoute(Route route) {
  checkUnicastRoute(route);

  const auto& prefix = route.getDestination();
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  if (unicastRoutes.count(prefix) == 0) {
    LOG(ERROR) << "Trying to delete non-existing prefix "
               << folly::IPAddress::networkToString(prefix);
    return;
  }

  int err = rtnl_route_delete(reqSock_, route.fromNetlinkRoute(), 0);
  // Mask off NLE_OBJ_NOTFOUND error because Netlink automatically withdraw
  // some routes when interface goes down
  if (err != 0 && -NLE_OBJ_NOTFOUND != err) {
    throw NetlinkException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(route.getDestination()),
        nl_geterror(err)));
  }

  // Update local cache with removed prefix
  unicastRoutes.erase(route.getDestination());
}

void NetlinkSocket::doAddMulticastRoute(Route route) {
  checkMulticastRoute(route);

  auto& mcastRoutes = mcastRoutesCache_[route.getProtocolId()];
  auto prefix = route.getDestination();
  auto ifName = route.getRouteIfName().value();
  auto key = std::make_pair(prefix, ifName);
  if (mcastRoutes.count(key)) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " exists for interface: " << ifName;
    return;
  }

  VLOG(3)
      << "Adding multicast route: " << folly::IPAddress::networkToString(prefix)
      << " for interface: " << ifName;

  int err = rtnl_route_add(reqSock_, route.fromNetlinkRoute(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to add multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }
  mcastRoutes.emplace(key, std::move(route));
}

void NetlinkSocket::checkMulticastRoute(const Route &route) {
  auto prefix = route.getDestination();
  if (not prefix.first.isMulticast()) {
    throw NetlinkException(
      folly::sformat("Invalid multicast address {}",
      folly::IPAddress::networkToString(prefix)));
  }
  if (not route.getRouteIfName().hasValue()) {
    throw NetlinkException(
      folly::sformat("Need set Iface name for multicast address {}",
      folly::IPAddress::networkToString(prefix)));
  }
}

void NetlinkSocket::doDeleteMulticastRoute(Route route) {
  checkMulticastRoute(route);

  auto& mcastRoutes = mcastRoutesCache_[route.getProtocolId()];
  auto prefix = route.getDestination();
  auto ifName = route.getRouteIfName().value();
  auto key = std::make_pair(prefix, ifName);
  auto iter = mcastRoutes.find(key);
  if (iter == mcastRoutes.end()) {
    // This could be kernel proto or our proto. we dont care
    LOG(WARNING)
        << "Multicast route: " << folly::IPAddress::networkToString(prefix)
        << " doesn't exists for interface: " << ifName;
    return;
  }

  VLOG(3) << "Deleting multicast route: "
          << folly::IPAddress::networkToString(prefix)
          << " for interface: " << ifName;

  int err = rtnl_route_delete(reqSock_, iter->second.fromNetlinkRoute(), 0);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to delete multicast route {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        nl_geterror(err)));
  }

  mcastRoutes.erase(iter);
}

folly::Future<folly::Unit>
NetlinkSocket::syncUnicastRoutes(
    uint8_t protocolId, NlUnicastRoutes newRouteDb) {
  VLOG(3) << "Netlink syncing Unicast Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        p = std::move(promise),
        syncDb = std::move(newRouteDb),
        protocolId
      ]() mutable {
        try {
          doSyncUnicastRoutes(protocolId, std::move(syncDb));
          p.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                     << folly::exceptionStr(ex);
          p.setException(ex);
        }
      });
  return future;
}

void NetlinkSocket::doSyncUnicastRoutes(
    uint8_t protocolId, NlUnicastRoutes syncDb) {
  auto& unicastRoutes = unicastRoutesCache_[protocolId];

  // Go over routes that are not in new routeDb, delete
  std::unordered_set<folly::CIDRNetwork> toDelete;
  for (auto const& kv : unicastRoutes) {
    if (syncDb.find(kv.first) == syncDb.end()) {
      VLOG(3) << "FUCK TO DEL " << folly::IPAddress::networkToString(kv.first);
      toDelete.insert(kv.first);
    }
  }
  // Delete routes from kernel
  for (auto it = toDelete.begin(); it != toDelete.end(); ++it) {
    auto const& prefix = *it;
    auto iter = unicastRoutes.find(prefix);
    if (iter == unicastRoutes.end()) {
      continue;
    }
    try {
      RouteBuilder builder;
      doDeleteUnicastRoute(
        builder.buildFromObject(iter->second.fromNetlinkRoute()));
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
        "Could not del Route to: {} Error: {}",
        folly::IPAddress::networkToString(prefix),
        folly::exceptionStr(err)));
    }
  }

  // Go over routes in new routeDb, update/add
  for (auto const& kv : syncDb) {
    auto const& prefix = kv.first;
    try {
      RouteBuilder builder;
      VLOG(3) << "FUCK TO ADD " << folly::IPAddress::networkToString(kv.first);
      doAddUpdateUnicastRoute(
        builder.buildFromObject(kv.second.fromNetlinkRoute()));
    } catch (std::exception const& err) {
      throw std::runtime_error(folly::sformat(
          "Could not update Route to: {} Error: {}",
          folly::IPAddress::networkToString(prefix),
          folly::exceptionStr(err)));
    }
  }
}

folly::Future<folly::Unit>
NetlinkSocket::syncLinkRoutes(uint8_t protocolId, NlLinkRoutes newRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [ this,
        p = std::move(promise),
        syncDb = std::move(newRouteDb),
        protocolId
      ]() mutable {
        try {
          doSyncLinkRoutes(protocolId, std::move(syncDb));
          p.setValue();
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing link routeDb with Fib: "
                     << folly::exceptionStr(ex);
          p.setException(ex);
        }
      });
  return future;
}

void NetlinkSocket::doSyncLinkRoutes(uint8_t protocolId, NlLinkRoutes syncDb) {
  auto& linkRoutes = linkRoutesCache_[protocolId];
  std::vector<std::pair<folly::CIDRNetwork, std::string>> toDel;
  for (const auto& route : linkRoutes) {
    if (!syncDb.count(route.first)) {
      toDel.emplace_back(route.first);
    }
  }
  for (const auto& routeToDel : toDel) {
    auto iter = linkRoutes.find(routeToDel);
    if (iter == linkRoutes.end()) {
      continue;
    }
    int err = rtnl_route_delete(reqSock_, iter->second.fromNetlinkRoute(), 0);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Could not del link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(routeToDel.first),
          routeToDel.second,
          nl_geterror(err)));
    }
  }

  for (const auto& routeToAdd : syncDb) {
    if (linkRoutes.count(routeToAdd.first)) {
      continue;
    }
    int err = rtnl_route_add(reqSock_, routeToAdd.second.fromNetlinkRoute(), 0);
    if (err != 0) {
      throw NetlinkException(folly::sformat(
          "Could not add link Route to: {} dev {} Error: {}",
          folly::IPAddress::networkToString(routeToAdd.first.first),
          routeToAdd.first.second,
          nl_geterror(err)));
    }
  }
  linkRoutes.swap(syncDb);
}

folly::Future<NlUnicastRoutes>
NetlinkSocket::getCachedUnicastRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedUnicastRoutes by protocol "
          << (int)protocolId;
  folly::Promise<NlUnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      auto iter = unicastRoutesCache_.find(protocolId);
      NlUnicastRoutes ret;
      if (iter != unicastRoutesCache_.end()) {
        const NlUnicastRoutes& routes = iter->second;
        RouteBuilder builder;
        for (const auto& route : routes) {
          ret.emplace(std::make_pair(
            route.first,
            builder.buildFromObject(route.second.fromNetlinkRoute())));
          builder.reset();
        }
      }
      p.setValue(std::move(ret));
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting unicast route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlMulticastRoutes>
NetlinkSocket::getCachedMulticastRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedMulticastRoutes by protocol "
          << (int)protocolId;
  folly::Promise<NlMulticastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      auto iter = mcastRoutesCache_.find(protocolId);
      NlMulticastRoutes ret;
      if (iter != mcastRoutesCache_.end()) {
        const NlMulticastRoutes& routes = iter->second;
        RouteBuilder builder;
        for (const auto& route : routes) {
          ret.emplace(std::make_pair(
            route.first,
            builder.buildFromObject(route.second.fromNetlinkRoute())));
          builder.reset();
        }
      }
      p.setValue(std::move(ret));
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting mcast route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlLinkRoutes>
NetlinkSocket::getCachedLinkRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedLinkRoutes by protocol "
          << (int)protocolId;

  folly::Promise<NlLinkRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise),
     protocolId]() mutable {
    try {
      auto iter = linkRoutesCache_.find(protocolId);
      NlLinkRoutes ret;
      if (iter != linkRoutesCache_.end()) {
        const NlLinkRoutes& routes = iter->second;
        RouteBuilder builder;
        for (const auto& route : routes) {
          ret.emplace(std::make_pair(
            route.first,
            builder.buildFromObject(route.second.fromNetlinkRoute())));
          builder.reset();
        }
      }
      p.setValue(std::move(ret));
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error getting link route cache: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<int64_t> NetlinkSocket::getRouteCount() const {
  VLOG(3) << "NetlinkSocket get routes number";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
    [this,
     p = std::move(promise)]() mutable {
     int64_t count = 0;
     for (const auto& routes: unicastRoutesCache_) {
       count += routes.second.size();
     }
     p.setValue(count);
  });
  return future;
}

folly::Future<folly::Unit> NetlinkSocket::addIfAddress(fbnl::IfAddress) {
  folly::Promise<folly::Unit> promise;
  // TODO Need implement
  return promise.getFuture();
}

folly::Future<folly::Unit> NetlinkSocket::delIfAddress(fbnl::IfAddress) {
  folly::Promise<folly::Unit> promise;
  // TODO Need implement
  return promise.getFuture();
}

folly::Future<folly::Unit> NetlinkSocket::syncIfAddress(
   int, std::vector<fbnl::IfAddress>, int, int) {
  folly::Promise<folly::Unit> promise;
  // TODO Need implement
  return promise.getFuture();
}

folly::Future<std::vector<fbnl::IfAddress>> NetlinkSocket::getIfAddrs(
     int , int , int) {
  folly::Promise<std::vector<fbnl::IfAddress>> promise;
  // TODO Need implement
  return promise.getFuture();
}

folly::Future<int> NetlinkSocket::getIfIndex(const std::string& ifName) const {
  folly::Promise<int> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifStr = ifName.c_str()]() mutable {
        int ifIndex = rtnl_link_name2i(linkCache_, ifStr);
        p.setValue(ifIndex);
      });
  return future;
}

folly::Future<std::string> NetlinkSocket::getIfName(int ifIndex) const {
  folly::Promise<std::string> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex]() mutable {
        std::array<char, IFNAMSIZ> ifNameBuf;
        std::string ifName(
          rtnl_link_i2name(
            linkCache_, ifIndex, ifNameBuf.data(), ifNameBuf.size()));
        p.setValue(ifName);
      });
  return future;
}

folly::Future<NlLinks> NetlinkSocket::getAllLinks() {
  folly::Promise<NlLinks> promise;
  // TODO Need implement
  return promise.getFuture();
}

folly::Future<NlNeighbors> NetlinkSocket::getAllReachableNeighbors() {
  folly::Promise<NlNeighbors> promise;
  // TODO Need implement
  return promise.getFuture();
}

void NetlinkSocket::subscribeEvent(NetlinkEventType) {
  // TODO Need implement
}

void NetlinkSocket::unsubscribeEvent(NetlinkEventType) {
  // TODO Need implement
}

void NetlinkSocket::subscribeAllEvents() {
  // TODO Need implement
}

void NetlinkSocket::unsubscribeAllEvents() {
  // TODO Need implement
}

} // namespace fbnl
} // namespace openr
