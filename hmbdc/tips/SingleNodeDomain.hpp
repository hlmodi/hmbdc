#include "hmbdc/Copyright.hpp"
#pragma once
#include "hmbdc/tips/Node.hpp"
#include "hmbdc/tips/Domain.hpp"

namespace hmbdc { namespace tips {


namespace detail {
template <typename CcNode, typename RecvMessageTuple>
struct ContextCallForwarder {
    CcNode* node = nullptr;
    template <app::MessageC Message>
    void send(Message&& message) {
        if constexpr (index_in_tuple<
            typename std::decay<Message>::type, RecvMessageTuple>::value
                != std::tuple_size<RecvMessageTuple>::value) {
            node->handleMessageCb(std::forward<Message>(message));
        }
    }

    template <app::MessageC Message>
    bool trySend(Message&& message) {
        if constexpr (index_in_tuple<
            typename std::decay<Message>::type, RecvMessageTuple>::value
                != std::tuple_size<RecvMessageTuple>::value) {
            node->handleMessageCb(std::forward<Message>(message));
        }
        return true;
    }

    void sendJustBytesInPlace(uint16_t tag, void const* bytes, size_t, app::hasMemoryAttachment* att) {
        if constexpr (index_in_tuple<app::JustBytes, RecvMessageTuple>::value
                != std::tuple_size<RecvMessageTuple>::value) {
            node->handleJustBytesCb(tag, (uint8_t const*)bytes, att);
        }
    }
    void invokedCb(size_t n) {
        if constexpr (std::is_base_of<time::TimerManager, CcNode>::value) {
            node->checkTimers(hmbdc::time::SysTime::now());
        }
        node->invokedCb(n);
    }
    
    void messageDispatchingStartedCb(size_t const*p) {node->messageDispatchingStartedCb(p);}
    void stoppedCb(std::exception const& e) {node->stoppedCb(e);}
    bool droppedCb() {return node->droppedCb();}
    void stop(){}
    void join(){}
};
}

/**
 * @brief Replace Domain template with SingleNodeDomain if the Domain object holds exact one Node
 * We call this kind of Domain SingleNodeDomain. SingleNodeDomain is powered by a single pump thread.
 * Lower latency can be achieved due to one less thread hop compared to Regular Node
 * 
 * @tparam CcNode the Node type the Domain is to hold
 * @tparam RecvMessageTupleIn see Domain documentation
 * @tparam IpcProp see Domain documentation
 * @tparam NetProp see Domain documentation
 * @tparam DefaultAttachmentAllocator see Domain documentation
 */
template <typename CcNode
    , app::MessageTupleC RecvMessageTupleIn
    , typename IpcProp
    , typename NetProp
    , typename AttachmentAllocator = DefaultAttachmentAllocator>
struct SingleNodeDomain 
: private Domain<RecvMessageTupleIn
    , IpcProp, NetProp, detail::ContextCallForwarder<CcNode, RecvMessageTupleIn>
    , AttachmentAllocator> {
public:
    /**
     * @brief Construct a new SingleNodeDomain object
     * 
     * @param cfg - used to construct Domain, see Domain documentation
     */
    SingleNodeDomain(app::Config cfg)
    : SingleNodeDomain::Domain(cfg.put("pumpRunMode", "delayed")) {
    }

/**
     * @brief add THE Node within this Domain as a thread - handles its subscribing here too
     * @details should only call this once since it is a SingleNodeDomain
     * @tparam Node a concrete Node type that send and/or recv Messages
     * @param node the instance of the node - the Domain does not manage the object lifespan
     * @return the SingleNodeDomain itself
     */
    SingleNodeDomain& add(CcNode& node) {
        if (this->threadCtx_.node) {
            HMBDC_THROW(std::logic_error, "previously added a Node")
        }
        this->threadCtx_.node = &node;
        node.setDomain(*this);
        this->addPubSubFor(node);
        return *this;
    }

    /**
     * @brief exposed from Domain - see Domain documentation
     * 
     */
    using SingleNodeDomain::Domain::getDftConfig;
    using SingleNodeDomain::Domain::startPumping;
    using SingleNodeDomain::Domain::pumpOnce;
    using SingleNodeDomain::Domain::addPubSubFor;
    using SingleNodeDomain::Domain::ipcPartyDetectedCount;
    using SingleNodeDomain::Domain::netSendingPartyDetectedCount;
    using SingleNodeDomain::Domain::netRecvingPartyDetectedCount;
    using SingleNodeDomain::Domain::ipcSubscribingPartyCount;
    using SingleNodeDomain::Domain::netSubscribingPartyCount;
    using SingleNodeDomain::Domain::ownIpcTransport;
    using SingleNodeDomain::Domain::publish;
    using SingleNodeDomain::Domain::tryPublish;
    using SingleNodeDomain::Domain::publishJustBytes;
    using SingleNodeDomain::Domain::allocateInShmFor0cpy;
    using SingleNodeDomain::Domain::stop;
    using SingleNodeDomain::Domain::join;
};

}}
