/* Copyright (c) 2010-2016 Stanford University
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

#ifndef RAMCLOUD_DRIVER_H
#define RAMCLOUD_DRIVER_H

#include <memory>
#include <vector>

#include "Common.h"
#include "Buffer.h"

#undef CURRENT_LOG_MODULE
#define CURRENT_LOG_MODULE RAMCloud::TRANSPORT_MODULE

namespace RAMCloud {

class ServiceLocator;

/**
 * Used by transports to send and receive unreliable datagrams. This is an
 * abstract class; see UdpDriver for an example of a concrete implementation.
 */
class Driver {
  public:

    /**
     * A base class for Driver-specific network addresses.
     */
    class Address {
      protected:
        Address() {}
        Address(const Address& other) {}
      public:
        virtual ~Address() {}
        /**
         * Copies an address.
         * \return
         *      An address that the caller must free later.
         */
        virtual Address* clone() const = 0;

        /**
         * Return a string describing the contents of this Address (for
         * debugging, logging, etc).
         */
        virtual string toString() const = 0;
    };

    typedef std::unique_ptr<Address> AddressPtr;

    /**
     * Represents an incoming packet.
     *
     * A Received typically refers to resources owned by the driver, such
     * as a packet buffer. These resources will be returned to the driver
     * when the Received is destroyed. However, if the transport wishes to
     * retain ownership of the packet buffer after the Received is destroyed
     * (e.g. while the RPC is being processed), then it may call the
     * steal method to take over responsibility for the packet buffer.
     * If it does this, it must eventually call the Driver's release
     * method to return the packet buffer.
     */
    class Received {
      public:
        /**
         * Construct a Received that contains no data and is not
         * associated with a Driver.
         */
        Received()
            : sender(NULL)
            , driver(0)
            , len(0)
            , payload(0)
        {}

        Received(const Address* sender, Driver *driver, uint32_t len,
                char *payload)
            : sender(sender)
            , driver(driver)
            , len(len)
            , payload(payload)
        {}

        // Move constructor (needed for using in std::vectors)
        Received(Received&& other)
            : sender(other.sender)
            , driver(other.driver)
            , len(other.len)
            , payload(other.payload)
        {
            other.sender = NULL;
            other.driver = NULL;
            other.len = 0;
            other.payload = NULL;
        }

       ~Received();

        void* getRange(uint32_t offset, uint32_t length);

        /**
         * Allows data at a given offset into the Received to be treated as a
         * specific type.
         *
         * \tparam T
         *      The type to treat the data at payload + offset as.
         * \param offset
         *      Offset in bytes of the desired object within the payload.
         * \return
         *      Pointer to a T object at the desired offset, or NULL if
         *      the requested object doesn't fit entirely within the payload.
         */
        template<typename T>
        T*
        getOffset(uint32_t offset)
        {
            return static_cast<T*>(getRange(offset, sizeof(T)));
        }

        char *steal(uint32_t *len);

        /// Address from which this data was received. The object referred
        /// to by this pointer will be stable as long as the packet data
        /// is stable (i.e., if steal() is invoked, then the Address will
        /// live until release is invoked).
        const Address* sender;

        /// Driver the packet came from, where resources should be returned.
        Driver *driver;

        /// Length in bytes of received data.
        uint32_t len;

        /// The start of the received data.  If non-NULL then we must return
        /// this storage to the driver when this object is deleted.  NULL means
        /// someone else (e.g. the Transport module) has taken responsibility
        /// for it.
        char *payload;

        /// Counts the total number of times that steal has been invoked
        /// across all Received objects. Intended for unit testing; only
        /// updated if compiled for TESTING.
        static uint32_t stealCount;

      private:
        DISALLOW_COPY_AND_ASSIGN(Received);
    };

    /**
     * A Buffer::Chunk that is comprised of memory for incoming packets,
     * owned by the Driver but loaned to a Buffer during the processing of an
     * incoming RPC so the message doesn't have to be copied.
     *
     * PayloadChunk behaves like any other Buffer::Chunk except it returns
     * its memory to the Driver when the Buffer is deleted.
     */
    class PayloadChunk : public Buffer::Chunk {
      public:
        static PayloadChunk* prependToBuffer(Buffer* buffer,
                                             char* data,
                                             uint32_t dataLength,
                                             Driver* driver,
                                             char* payload);
        static PayloadChunk* appendToBuffer(Buffer* buffer,
                                            char* data,
                                            uint32_t dataLength,
                                            Driver* driver,
                                            char* payload);
        ~PayloadChunk();
      private:
        PayloadChunk(void* data,
                     uint32_t dataLength,
                     Driver* driver,
                     char* const payload);

        /// Return the PayloadChunk memory here.
        Driver* const driver;

        /// The memory backing the chunk and which is to be returned.
        char* const payload;

        friend class Buffer;      // allocAux must call private constructor.
        DISALLOW_COPY_AND_ASSIGN(PayloadChunk);
    };

    virtual ~Driver();

    /// \copydoc Transport::dumpStats
    virtual void dumpStats() {}

    /**
     * The maximum number of bytes this Driver can transmit in a single
     * packet, including both header and payload.
     */
    virtual uint32_t getMaxPacketSize() = 0;

    /**
     * This method provides a hint to transports about how many bytes
     * they should send. The driver will operate most efficiently if
     * transports don't send more bytes than indicated by the return value.
     * This will keep the output queue relatively short, which allows better
     * scheduling (e.g., a new short message or control packet can preempt
     * a long ongoing message). At the same time, it will allow enough
     * buffering so that the output queue doesn't completely run dry
     * (resulting in unused network bandwidth) before the transport's poller
     * checks this method again.
     * \param currentTime
     *      Current time, in Cycles::rdtsc ticks.
     * \return
     *      Number of bytes that can be transmitted without creating
     *      a long output queue in the driver. Value may be negative
     *      if transport has ignored this method and transmitted too
     *      many bytes.
     */
    virtual int getTransmitQueueSpace(uint64_t currentTime)
    {
        // Default: no throttling of transmissions (probably not a good
        // idea).
        return 10000000;
    }

    /**
     * Invoked by a transport when it has finished processing the data
     * in an incoming packet; used by drivers to recycle packet buffers
     * at a safe time.
     *
     * \param payload
     *      The first byte of a packet that was previously "stolen"
     *      (i.e., the payload field from the Received object used to
     *      pass the packet to the transport when it was received).
     */
    virtual void release(char *payload);

    template<typename T>
    void release(T* payload) {
        release(reinterpret_cast<char*>(payload));
    }

    /**
     * Return a new Driver-specific network address for the given service
     * locator.
     * This function may be called from worker threads and should contain any
     * necessary synchronization.
     * \param serviceLocator
     *      See above.
     * \return
     *      An address that must be deleted later by the caller.
     * \throw NoSuchKeyException
     *      Service locator option missing.
     * \throw BadValueException
     *      Service locator option malformed.
     */
    virtual Address* newAddress(const ServiceLocator* serviceLocator) = 0;

    /**
     * Checks to see if any packets have arrived that have not already
     * been returned by this method; if so, it returns some or all of
     * them.
     * \param maxPackets
     *      The maximum number of packets that should be returned by
     *      this application
     * \param receivedPackets
     *      Returned packets are appended to this vector, one Received
     *      object per packet, in order of packet arrival.
     */
    virtual void receivePackets(int maxPackets,
            std::vector<Received>* receivedPackets) = 0;

    /**
     * Associates a contiguous region of memory to a NIC so that the memory
     * addresses within that region become direct memory accessible (DMA) for
     * the NIC. This method must be implemented in the driver code if
     * the NIC needs to do zero copy transmit of buffers within that region of
     * memory.
     * \param base
     *     pointer to the beginning of the memory region that is to be
     *     registered to the NIC.
     * \param bytes
     *     The total size in bytes of the memory region starting at \a base
     *     that is to be registered with the NIC.
     */
    virtual void registerMemory(void* base, size_t bytes) {}

    /**
     * Send a single packet out over this Driver. The packet will not
     * necessarily have been transmitted before this method returns.  If an
     * error occurs, this method will log the error and return without
     * sending anything; this method does not throw exceptions.
     *
     * header provides a means to slip data onto the front of the packet
     * without having to pay for a prepend to the Buffer containing the
     * packet payload data.
     *
     * \param recipient
     *      The address the packet should go to.
     * \param header
     *      Bytes placed in the packet ahead of those from payload. The
     *      driver will make a copy of this data, so the caller need not
     *      preserve it after the method returns, even if the packet hasn't
     *      yet been transmitted.
     * \param headerLen
     *      Length in bytes of the data in header.
     * \param payload
     *      A buffer iterator describing the bytes for the payload (the
     *      portion of the packet after the header).  May be NULL to
     *      indicate "no payload". Note: caller must preserve the buffer
     *      data (but not the actual iterator) even after the method returns,
     *      since the data may not yet have been transmitted.
     */
    virtual void sendPacket(const Address* recipient,
                            const void* header,
                            uint32_t headerLen,
                            Buffer::Iterator *payload) = 0;

    /**
     * Alternate form of sendPacket.
     *
     * \param recipient
     *      Where to send the packet.
     * \param header
     *      Contents of this object will be placed in the packet ahead
     *      of payload.  The driver will make a copy of this data, so
     *      the caller need not preserve it after the method returns, even
     *      if the packet hasn't yet been transmitted.
     * \param payload
     *      A buffer iterator positioned at the bytes for the payload to
     *      follow the headerLen bytes from header.  May be NULL to
     *      indicate "no payload". Note: caller must preserve the buffer
     *      data (but not the actual iterator) even after the method returns,
     *      since the data may not yet have been transmitted.
     */
    template<typename T>
    void sendPacket(const Address* recipient,
                            const T* header,
                            Buffer::Iterator *payload)
    {
        sendPacket(recipient, header, sizeof(T), payload);
    }

    /**
     * Return the ServiceLocator for this Driver. If the Driver
     * was not provided static parameters (e.g. fixed TCP or UDP port),
     * this function will return a SerivceLocator with those dynamically
     * allocated attributes.
     *
     * Enlisting the dynamic ServiceLocator with the Coordinator permits
     * other hosts to contact dynamically addressed services.
     */
    virtual string getServiceLocator() = 0;

    /**
     * The maximum amount of time it should take to drain the transmit
     * queue for this driver when it is completely full (i.e.,
     * getTransmitQueueSpace returns 0).  See the documentation for
     * getTransmitQueueSpace for motivation. Specified in units of
     * nanoseconds.
     */
    static const uint32_t MAX_DRAIN_TIME = 2000;
};

/**
 * Thrown if a Driver cannot be initialized properly.
 */
struct DriverException: public Exception {
    explicit DriverException(const CodeLocation& where)
        : Exception(where) {}
    DriverException(const CodeLocation& where, std::string msg)
        : Exception(where, msg) {}
    DriverException(const CodeLocation& where, int errNo)
        : Exception(where, errNo) {}
    DriverException(const CodeLocation& where, string msg, int errNo)
        : Exception(where, msg, errNo) {}
};


}  // namespace RAMCloud

#endif
