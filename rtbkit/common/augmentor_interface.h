#pragma once

#include "soa/service/service_base.h"
#include "soa/service/zmq_endpoint.h"
#include "soa/service/typed_message_channel.h"
#include "soa/service/loop_monitor.h"
#include "rtbkit/common/auction_events.h"
#include "rtbkit/core/router/router_types.h"
#include "rtbkit/core/post_auction/events.h"
#include "rtbkit/common/plugin_interface.h"

namespace RTBKIT {

class Router;

struct AugmentorInterface : public ServiceBase
{

    // Information about an auction being augmented
    struct AugmentationInfo {
        AugmentationInfo() {}

        AugmentationInfo(const std::shared_ptr<Auction> & auction,
                         Date lossTimeout)
            : auction(auction), lossTimeout(lossTimeout) {}

        std::shared_ptr<Auction> auction;   ///< Our copy of the auction
        Date lossTimeout;                     ///< When we send a loss if
        std::vector<GroupPotentialBidders> potentialGroups; ///< One per group
    };

    typedef boost::function<void (const std::shared_ptr<AugmentationInfo> &)>
        OnFinished;

    struct Entry {
        std::shared_ptr<AugmentationInfo> info;
        std::set<std::string> outstanding;
        std::map<std::string, std::set<std::string> > augmentorAgents;
        OnFinished onFinished;
        Date timeout;
    };

    AugmentorInterface(ServiceBase & parent,
                    std::string const & serviceName = "augmentorService");

    AugmentorInterface(std::shared_ptr<ServiceProxies> proxies = std::make_shared<ServiceProxies>(),
                    std::string const & serviceName = "augmentorService");

    AugmentorInterface(const AugmentorInterface &other) = delete;
    AugmentorInterface &operator=(const AugmentorInterface &other) = delete;

    void setInterfaceName(const std::string &name);
    std::string interfaceName() const;

    virtual void init() = 0;
    virtual void shutdown();

    virtual void start();

    virtual size_t numAugmenting() const = 0;

    virtual void augment(const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
                 Date timeout,
                 const OnFinished & onFinished) = 0;

    // This is an ugly hack to fix the interface
    // with the old augmentation loop
    virtual void* getLoop() {return nullptr;}

    virtual void sleepUntilIdle(){return;}

    virtual void registerLoopMonitor(LoopMonitor *monitor) const { }
    //
    // factory
    //
    static std::shared_ptr<AugmentorInterface>
    create(std::string serviceName,
           std::shared_ptr<ServiceProxies> const & proxies,
           Json::Value const & json);

    typedef std::function<AugmentorInterface * (std::string serviceName,
                                             std::shared_ptr<ServiceProxies> const & proxies,
                                             Json::Value const & json)> Factory;
  
    // FIXME: this is being kept just for compatibility reasons.
    // we don't want to break compatibility now, although this interface does not make
    // sense any longer  
    // so any use of it should be considered deprecated
    static void registerFactory(std::string const & name, Factory factory)
    {
      PluginInterface<AugmentorInterface>::registerPlugin(name, factory);
    }

  
    /** plugin interface needs to be able to request the root name of the plugin library */
    static const std::string libNameSufix() {return "augmentor";};

    std::string name;
    Router * router;
};

}
