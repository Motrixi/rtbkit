#pragma once

#include "rtbkit/common/augmentor_interface.h"
#include "rtbkit/core/router/augmentation_loop.h"

namespace RTBKIT {

struct ZMQAugmentorInterface : public AugmentorInterface
{

    ZMQAugmentorInterface(
        std::string serviceName = "augmentorService",
        std::shared_ptr<ServiceProxies> proxies = std::make_shared<ServiceProxies>(),
        Json::Value const & json = Json::Value());
 
    ~ZMQAugmentorInterface();

    void init(Router * r = nullptr);
    void start();
    void shutdown();

    virtual size_t numAugmenting() const;

    virtual void augment(const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
                 Date timeout,
                 const OnFinished & onFinished);

    virtual void* getLoop() {return (void*)&augmentationLoop;}

    virtual void sleepUntilIdle();

private :

    AugmentationLoop augmentationLoop;

};

}
