/* Copyright (c) 2015-2016 Stanford University
 * Copyright (c) 2014-2015 Huawei Technologies Co. Ltd.
 * The original version of this module was contributed by Anthony Iliopoulos
 * at DBERC, Huawei
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define __STDC_LIMIT_MACROS
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#pragma GCC diagnostic ignored "-Wconversion"
#include <rte_config.h>
#include <rte_common.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_ring.h>
#pragma GCC diagnostic warning "-Wconversion"

#include "Common.h"
#include "Cycles.h"
#include "ShortMacros.h"
#include "DpdkDriver.h"
#include "NetUtil.h"
#include "ServiceLocator.h"
#include "StringUtil.h"
#include "TimeTrace.h"
#include "Util.h"

namespace RAMCloud
{

/*
 * Construct a DpdkDriver.
 *
 * \param context
 *      Overall information about the RAMCloud server or client.
 * \param localServiceLocator
 *      "devport" option specifies the physical port to use on the NIC,
 *      If the port is not specified, then by default the driver will
 *      use the first one. "mac" option specifies the MAC address. If
 *      the MAC address is specified to be all zeroes, then the default
 *      device MAC will be used.
 */

DpdkDriver::DpdkDriver(Context* context,
                       const ServiceLocator* localServiceLocator)
    : context(context)
    , packetBufPool()
    , packetBufsUtilized(0)
    , locatorString()
    , localMac()
    , portId(0)
    , packetPool(NULL)
    , loopbackRing(NULL)
    , bandwidthGbps(10)                   // Default bandwidth = 10 gbs
    , queueEstimator(0)
    , maxTransmitQueueSize(0)
{
    struct ether_addr mac;
    struct rte_eth_link link;
    uint8_t numPorts;
    struct rte_eth_conf portConf;
    int ret;

    // parse the locator string, if specified, and obtain the values
    // for various parameters.
    if (localServiceLocator != NULL) {
        locatorString = localServiceLocator->getOriginalString();
        try {
            localMac.construct(
                    localServiceLocator->getOption<const char*>("mac"));
        } catch (ServiceLocator::NoSuchKeyException& e) {
        }
        try {
            string localPort = localServiceLocator->getOption("devport");
            bool error;
            portId = downCast<uint8_t>(StringUtil::stringToInt(
                    localPort.c_str(), &error));
            if (error) {
                throw DriverException(HERE, format(
                        "Bad devport option in service locator: %s",
                        locatorString.c_str()));
            }
        } catch (ServiceLocator::NoSuchKeyException& e) {
        }
        try {
            bandwidthGbps = localServiceLocator->getOption<int>("gbs");
        } catch (ServiceLocator::NoSuchKeyException& e) {}
    }
    queueEstimator.setBandwidth(1000*bandwidthGbps);
    maxTransmitQueueSize = (uint32_t) (static_cast<double>(bandwidthGbps)
            * MAX_DRAIN_TIME / 8.0);
    uint32_t maxPacketSize = getMaxPacketSize();
    if (maxTransmitQueueSize < 2*maxPacketSize) {
        // Make sure that we advertise enough space in the transmit queue to
        // prepare the next packet while the current one is transmitting.
        maxTransmitQueueSize = 2*maxPacketSize;
    }

    // initialize the DPDK environment with some default parameters
    const char *argv[] = {"rc", "-c 1", "-n 1"};
    int argc = sizeof(argv) / sizeof(argv[0]);

    ret = rte_eal_init(argc, const_cast<char**>(argv));

    // create an memory pool for accommodating packet buffers
    packetPool = rte_mempool_create("mbuf_pool", NB_MBUF,
                                      MBUF_SIZE, 32,
                                      sizeof(struct rte_pktmbuf_pool_private),
                                      rte_pktmbuf_pool_init, NULL,
                                      rte_pktmbuf_init, NULL,
                                      rte_socket_id(), 0);

    if (!packetPool) {
        throw DriverException(HERE, format(
                "Failed to allocate memory for packet buffers: %s",
                rte_strerror(rte_errno)));
    }

    // ensure that DPDK was able to detect a compatible and available NIC
    numPorts = rte_eth_dev_count();

    if (numPorts <= portId) {
        throw DriverException(HERE, format(
                "Ethernet port %u doesn't exist (%u ports available)",
                portId, numPorts));
    }

    // if the locator string specified an all-zeroes MAC address,
    // use the default one by reading it from the NIC via DPDK.
    // Fix-up the locator string to reflect the real MAC address.
    if (!localMac || localMac->isNull()) {
        rte_eth_macaddr_get(portId, &mac);
        localMac.construct(mac.addr_bytes);
        locatorString = format("basic+dpdk:mac=%s,devport=%d",
                localMac->toString().c_str(), portId);
    }

    // configure some default NIC port parameters
    memset(&portConf, 0, sizeof(portConf));
    portConf.rxmode.max_rx_pkt_len = MAX_PAYLOAD_SIZE +
            sizeof(NetUtil::EthernetHeader);
    rte_eth_dev_configure(portId, 1, 1, &portConf);

    // setup a NIC/HW-based filter on the ethernet type so that
    // only FAST transport traffic is delivered from the NIC to
    // DPDK.
    struct rte_ethertype_filter ethertype_filter;

    ethertype_filter.ethertype = NetUtil::EthPayloadType::FAST;
    ethertype_filter.priority_en = 0;
    ethertype_filter.priority = 0;

    rte_eth_dev_add_ethertype_filter(portId, 0, &ethertype_filter, 0);

    // setup and initialize the receive and transmit NIC queues,
    // and activate the port.
    rte_eth_rx_queue_setup(portId, 0, NDESC, 0, NULL, packetPool);
    rte_eth_tx_queue_setup(portId, 0, NDESC, 0, NULL);
    rte_eth_dev_start(portId);

    // verify that there is an active link on the port
    rte_eth_link_get(portId, &link);
    if (!link.link_status) {
        throw DriverException(HERE,
                "Failed to detect a link on the Ethernet port");
    }

    // set the MTU that the NIC port should support
    ret = rte_eth_dev_set_mtu(portId, MAX_PAYLOAD_SIZE +
            sizeof(NetUtil::EthernetHeader));
    if (ret != 0) {
        throw DriverException(HERE, format(
                "Failed to set the MTU on the Ethernet port: %s",
                rte_strerror(rte_errno)));
    }

    // create an in-memory ring, used as a software loopback in order to handle
    // packets that are addressed to the localhost.
    loopbackRing = rte_ring_create("dpdk_loopback_ring", 4096,
            SOCKET_ID_ANY, 0);
    if (NULL == loopbackRing) {
        throw DriverException(HERE, format(
                "Failed to allocate loopback ring: %s",
                rte_strerror(rte_errno)));
    }

    LOG(NOTICE, "DpdkDriver locator: %s, bandwidth: %d Gbits/sec, "
            "maxTransmitQueueSize: %u bytes",
            locatorString.c_str(), bandwidthGbps, maxTransmitQueueSize);

    // DPDK during initialization (rte_eal_init()) pins the running thread
    // to a single processor. This becomes a problem as the master worker
    // threads are created after the initialization of the transport, and
    // thus inherit the (very) restricted affinity to a single core. This
    // essentially kills performance, as every thread is contenting for a
    // single core. Revert this, by restoring the affinity to the default
    // (all cores).
    Util::clearCpuAffinity();
}

/**
 * Destroy the DpdkDriver.
 */
DpdkDriver::~DpdkDriver()
{
    if (packetBufsUtilized != 0)
        LOG(ERROR, "DpdkDriver deleted with %d packets still in use",
            packetBufsUtilized);

    // Currently DPDK does not provide methods for freeing the various
    // allocated resources (e.g. ring, mempool) and releasing the NIC.
    // All we can do at this point is stop the packet reception
    // by disabling the activated NIC port.
    rte_eth_dev_stop(portId);
}

// See docs in Driver class.

uint32_t
DpdkDriver::getMaxPacketSize()
{
    return MAX_PAYLOAD_SIZE - sizeof(NetUtil::EthernetHeader);
}

// See docs in Driver class.
int
DpdkDriver::getTransmitQueueSpace(uint64_t currentTime)
{
    return maxTransmitQueueSize - queueEstimator.getQueueSize(currentTime);
}

// See docs in Driver class.
void
DpdkDriver::receivePackets(int maxPackets,
            std::vector<Received>* receivedPackets)
{
#define MAX_PACKETS_AT_ONCE 32
    if (maxPackets > MAX_PACKETS_AT_ONCE) {
        maxPackets = MAX_PACKETS_AT_ONCE;
    }
    struct rte_mbuf* mPkts[MAX_PACKETS_AT_ONCE];

    // attempt to dequeue a batch of received packets from the NIC
    // as well as from the loopback ring.
    uint32_t incomingPkts = rte_eth_rx_burst(portId, 0, mPkts,
            MAX_PACKETS_AT_ONCE/2);
    uint32_t loopbackPkts = rte_ring_count(loopbackRing);
    if (incomingPkts + loopbackPkts > MAX_PACKETS_AT_ONCE) {
        loopbackPkts = MAX_PACKETS_AT_ONCE - incomingPkts;
    }
    for (uint32_t i = 0; i < loopbackPkts; i++) {
        rte_ring_dequeue(loopbackRing,
                reinterpret_cast<void**>(&mPkts[incomingPkts + i]));
    }
    uint32_t totalPkts = incomingPkts + loopbackPkts;

    // Process received packets by constructing appropriate Received
    // objects and copying the payload from the DPDK packet buffers.
    for (uint32_t i = 0; i < totalPkts; i++) {
        struct rte_mbuf* m = mPkts[i];
        rte_prefetch0(rte_pktmbuf_mtod(m, void *));
        PacketBuf* buffer = packetBufPool.construct();
        packetBufsUtilized++;
        buffer->dpdkAddress.construct(
                rte_pktmbuf_mtod(m, uint8_t *) + sizeof(struct ether_addr));
        uint32_t length = rte_pktmbuf_data_len(m) -
                sizeof32(NetUtil::EthernetHeader);
        rte_memcpy(buffer->payload,
                static_cast<char*>(rte_pktmbuf_mtod(m, void *))
                + sizeof(NetUtil::EthernetHeader), length);
        receivedPackets->emplace_back(buffer->dpdkAddress.get(), this,
                length, buffer->payload);
        rte_pktmbuf_free(m);
    }
}

// See docs in Driver class.
void
DpdkDriver::release(char *payload)
{
    // Must sync with the dispatch thread, since this method could potentially
    // be invoked in a worker.
    Dispatch::Lock _(context->dispatch);

    // Note: the payload is actually contained in a PacketBuf structure,
    // which we return to a pool for reuse later.
    packetBufsUtilized--;
    assert(packetBufsUtilized >= 0);
    packetBufPool.destroy(
        reinterpret_cast<PacketBuf*>(payload - OFFSET_OF(PacketBuf, payload)));
}

// See docs in Driver class.
void
DpdkDriver::sendPacket(const Address *addr,
                       const void *header,
                       uint32_t headerLen,
                       Buffer::Iterator *payload)
{
    struct rte_mbuf *mbuf = NULL;
    char *data = NULL;

    uint32_t totalLength = headerLen +
            (payload ? payload->size() : 0);
    uint32_t datagramLength = totalLength + sizeof32(NetUtil::EthernetHeader);

    assert(totalLength <= MAX_PAYLOAD_SIZE);

    mbuf = rte_pktmbuf_alloc(packetPool);

    if (NULL == mbuf) {
        RAMCLOUD_CLOG(NOTICE,
                "Failed to allocate a packet buffer; dropping packet");
        return;
    }

    data = rte_pktmbuf_append(mbuf, downCast<uint16_t>(datagramLength));

    char *p = data;

    if (NULL == data) {
        RAMCLOUD_CLOG(NOTICE,
                "rte_pktmbuf_append call failed; dropping packet");
        rte_pktmbuf_free(mbuf);
        return;
    }

    NetUtil::EthernetHeader* ethHdr = reinterpret_cast<
            NetUtil::EthernetHeader*>(p);

    rte_memcpy(&ethHdr->destAddress,
            static_cast<const MacAddress*>(addr)->address, 6);
    rte_memcpy(&ethHdr->srcAddress, localMac->address, 6);
    ethHdr->etherType = HTONS(NetUtil::EthPayloadType::FAST);
    p += sizeof(*ethHdr);
    rte_memcpy(p, header, headerLen);
    p += headerLen;
    while (payload && !payload->isDone())
    {
        rte_memcpy(p, payload->getData(), payload->getLength());
        p += payload->getLength();
        payload->next();
    }

    // loopback if src mac == dst mac
    if (!memcmp(static_cast<const MacAddress*>(addr)->address,
            localMac->address, 6)) {
        rte_ring_enqueue(loopbackRing, mbuf);
    } else {
        rte_eth_tx_burst(portId, 0, &mbuf, 1);
    }
    queueEstimator.packetQueued(totalLength, Cycles::rdtsc());
}

string
DpdkDriver::getServiceLocator()
{
    LOG(NOTICE, "Locator string: %s ", locatorString.c_str());
    return locatorString;
}

} // namespace RAMCloud
