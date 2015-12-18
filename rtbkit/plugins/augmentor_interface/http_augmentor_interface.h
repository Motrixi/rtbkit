#pragma once

#include "rtbkit/common/augmentor_interface.h"
#include "soa/service/typed_message_channel.h"
#include "soa/service/timeout_map.h"
#include "soa/service/http_client.h"
#include "soa/service/logs.h"
#include "soa/gc/gc_lock.h"

namespace RTBKIT {

struct Bids;

struct HttpAugmentorInterface : public AugmentorInterface
{

    /*****************************************************************************/
    /* AUGMENTOR CONFIG CLASSES                                                  */
    /*****************************************************************************/

    /** Information about a specific augmentor which belongs to an augmentor class.
     */
    struct AugmentorInstanceInfo {
        AugmentorInstanceInfo(const std::string& host="127.0.0.1",
                const std::string& path = "", int maxInFlight = 0) :
            host(host), path(path), numInFlight(0), maxInFlight(maxInFlight)
        {}

        std::string host;
        std::string path;
        std::shared_ptr<HttpClient> httpClientAugmentor;
        int numInFlight;
        int maxInFlight;
    };

    /** Information about a given class of augmentor. */
    struct AugmentorInfo {
        AugmentorInfo(const std::string& name = "") : name(name) {}

        std::string name;                   ///< What the augmentation is called
        std::vector<AugmentorInstanceInfo> instances;

        AugmentorInstanceInfo* findInstance(const std::string& path)
        {
            for (auto it = instances.begin(), end = instances.end();
                 it != end; ++it)
            {
                if (it->path == path) return &(*it);
            }
            return nullptr;

        }
    };

    /*****************************************************************************/
    /* INTERFACE IMPLEMENTATION                                                  */
    /*****************************************************************************/

    HttpAugmentorInterface(
        std::string serviceName = "augmentorService",
        std::shared_ptr<ServiceProxies> proxies = std::make_shared<ServiceProxies>(),
        Json::Value const & json = Json::Value());
 
    ~HttpAugmentorInterface();

    void init(Router * r = nullptr);
    void start();
    void sleepUntilIdle();
    void shutdown();

    size_t numAugmenting() const;

    void augment(const std::shared_ptr<AugmentorInterface::AugmentationInfo> & info,
                 Date timeout,
                 const OnFinished & onFinished);

    virtual void registerLoopMonitor(LoopMonitor *monitor) const;

    static Logging::Category print;
    static Logging::Category error;
    static Logging::Category trace;

private:

    void processOKResponse(const std::string & headers,
                std::string &body,
                const std::string & aid,
                std::string & augmentor,
                const std::string & name,
                std::string & augmentation,
                AugmentationList & augmentationList);

    /** List of auctions we're currently augmenting.  Once the augmentation
        process is finished the auction will be passed on.
    */
    typedef TimeoutMap<Id, std::shared_ptr<Entry> > Augmenting;
    Augmenting augmenting;

    /** Currently configured augmentors.  Indexed by the augmentor name. */
    typedef std::map<std::string, std::shared_ptr<AugmentorInfo>> AugmentorInfoMap;
    AugmentorInfoMap augmentors;

    /** A single entry in the augmentor info structure. */
    struct AugmentorInfoEntry {
        std::string name;
        std::shared_ptr<AugmentorInfo> info;
    };

    /** A read-only structure in which the augmentors are periodically published.
        Protected by RCU.
    */
    typedef std::vector<AugmentorInfoEntry> AllAugmentorInfo;

    /** Pointer to current version.  Protected by allAgentsGc. */
    AllAugmentorInfo * allAugmentors;

    /** RCU protection for allAgents. */
    mutable GcLock allAugmentorsGc;

    MessageLoop loop;
    int idle_;

    enum Format {
        FMT_STANDARD,
        FMT_DATACRATIC,
    };
    static Format readFormat(const std::string& fmt);

    /// We pick up augmentations to be done from here
    TypedMessageSink<std::shared_ptr<Entry>> inbox;

    void doAugmentation(const std::shared_ptr<Entry> & entry);

    void recordStats();

    void checkExpiries();

    void augmentationExpired(const Id & id, const Entry & entry);

    AugmentorInstanceInfo* pickInstance(AugmentorInfo& aug);

};

}
