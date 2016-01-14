#include "zmq_augmentor_interface.h"

using namespace Datacratic;
using namespace RTBKIT;

ZMQAugmentorInterface::ZMQAugmentorInterface(
        std::string serviceName,
        std::shared_ptr<ServiceProxies> proxies,
        Json::Value const & json):
            AugmentorInterface(proxies, serviceName),
            augmentationLoop(proxies, serviceName),
            toAugmentors(getZmqContext()){
}
 
ZMQAugmentorInterface::~ZMQAugmentorInterface(){
}

void ZMQAugmentorInterface::init(){
    registerServiceProvider(serviceName(), { "rtbRouterAugmentation" });

    toAugmentors.init(getServices()->config, serviceName() + "/augmentors");

    toAugmentors.clientMessageHandler
        = [&] (const std::vector<std::string> & message)
        {
            //cerr << "got augmentor message " << message << endl;
            augmentationLoop.handleAugmentorMessage(message);
        };

    toAugmentors.bindTcp(getServices()->ports->getRange("augmentors"));

    toAugmentors.onConnection = [=] (const std::string & client)
        {
            std::cerr << "augmentor " << client << " has connected" << std::endl;
        };

    // These events show up on the zookeeper thread so redirect them to our
    // message loop thread.
    toAugmentors.onDisconnection = [=] (const std::string & client)
        {
            std::cerr << "augmentor " << client << " has disconnected" << std::endl;
            augmentationLoop.disconnections.push(client);
        };

    augmentationLoop.init(this);
}

void ZMQAugmentorInterface::start(){
    augmentationLoop.start();
}

void ZMQAugmentorInterface::shutdown(){
    augmentationLoop.shutdown();
    toAugmentors.shutdown();
}

void ZMQAugmentorInterface::sleepUntilIdle(){
    augmentationLoop.sleepUntilIdle();
}

ZmqNamedClientBus& ZMQAugmentorInterface::getZmqNamedClientBus(){
    return toAugmentors;
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

