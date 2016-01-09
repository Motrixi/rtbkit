#include "zmq_augmentor_interface.h"

using namespace Datacratic;
using namespace RTBKIT;

ZMQAugmentorInterface::ZMQAugmentorInterface(
        std::string serviceName,
        std::shared_ptr<ServiceProxies> proxies,
        Json::Value const & json):
            AugmentorInterface(proxies, serviceName),
            augmentationLoop(proxies, serviceName){
}
 
ZMQAugmentorInterface::~ZMQAugmentorInterface(){
}

void ZMQAugmentorInterface::init(Router * r){
    registerServiceProvider(serviceName(), { "rtbRouterAugmentation" });
    augmentationLoop.init();
}

void ZMQAugmentorInterface::start(){
    augmentationLoop.start();
}

void ZMQAugmentorInterface::shutdown(){
    augmentationLoop.shutdown();
}

void ZMQAugmentorInterface::sleepUntilIdle(){
    augmentationLoop.sleepUntilIdle();
}

size_t ZMQAugmentorInterface::numAugmenting() const{
    return augmentationLoop.numAugmenting();
}

void ZMQAugmentorInterface::augment(
        const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
        Date timeout,
        const OnFinished & onFinished){
    augmentationLoop.augment(info, timeout, onFinished);
}

namespace {

struct AtInit {
    AtInit()
    {
      PluginInterface<AugmentorInterface>::registerPlugin("zmq",
          [](std::string const &serviceName,
             std::shared_ptr<ServiceProxies> const &proxies,
             Json::Value const &json)
          {
              return new ZMQAugmentorInterface(serviceName, proxies, json);
          });
    }
} atInit;

}

