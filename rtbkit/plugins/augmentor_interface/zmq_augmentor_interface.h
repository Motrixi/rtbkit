#pragma once

#include "rtbkit/common/augmentor_interface.h"
#include "augmentation_loop.h"
#include "soa/service/zmq.hpp"

namespace RTBKIT {

struct ZMQAugmentorInterface : public AugmentorInterface
{

    friend class AugmentationLoop;

    ZMQAugmentorInterface(
        std::string serviceName = "augmentorService",
        std::shared_ptr<ServiceProxies> proxies = std::make_shared<ServiceProxies>(),
        Json::Value const & json = Json::Value());
 
    ~ZMQAugmentorInterface();

    void init();
    void start();
    void shutdown();

    virtual size_t numAugmenting() const;

    virtual void augment(const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
                 Date timeout,
                 const OnFinished & onFinished);

    virtual void* getLoop() {return (void*)&augmentationLoop;}

    virtual void sleepUntilIdle();

    ZmqNamedClientBus& getZmqNamedClientBus();

private :

    AugmentationLoop augmentationLoop;

    /// Connection to all of our augmentors
    ZmqNamedClientBus toAugmentors;

};

}
