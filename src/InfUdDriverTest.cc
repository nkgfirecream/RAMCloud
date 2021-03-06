/* Copyright (c) 2011-2016 Stanford University
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

#include "TestUtil.h"
#include "InfUdDriver.h"
#include "IpAddress.h"

namespace RAMCloud {
class InfUdDriverTest : public ::testing::Test {
  public:
    Context context;
    TestLog::Enable logEnabler;
    string packetData;
    Driver::Address *sender;

    InfUdDriverTest()
        : context()
        , logEnabler()
        , packetData()
        , sender(NULL)
    {}

    ~InfUdDriverTest() {
        delete sender;
    }

    // Used to wait for data to arrive on a driver; gives up if a long
    // time goes by with no data. Returns the contents of the incoming packet.
    const char *receivePacket(Driver* driver) {
        packetData.clear();
        uint64_t start = Cycles::rdtsc();
        while (true) {
            std::vector<Driver::Received> received;
            driver->receivePackets(1, &received);
            if (!received.empty()) {
                packetData.assign(received[0].payload, received[0].len);
                delete sender;
                sender = received[0].sender->clone();
                return packetData.c_str();
            }
            if (Cycles::toSeconds(Cycles::rdtsc() - start) > .1) {
                return "no packet arrived";
            }
        }
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(InfUdDriverTest);
};

TEST_F(InfUdDriverTest, basics) {
    // Send a packet from a client-style driver to a server-style
    // driver.
    ServiceLocator serverLocator("basic+infud:");
    InfUdDriver server(&context, &serverLocator, false);
    InfUdDriver *client =
            new InfUdDriver(&context, NULL, false);
    ServiceLocator sl(server.getServiceLocator());
    Driver::Address* serverAddress = client->newAddress(&sl);

    Buffer message;
    const char *testString = "This is a sample message";
    message.appendExternal(testString, downCast<uint32_t>(strlen(testString)));
    Buffer::Iterator iterator(&message);
    client->sendPacket(serverAddress, "header:", 7, &iterator);
    TestLog::reset();
    EXPECT_STREQ("header:This is a sample message",
            receivePacket(&server));
    EXPECT_EQ("", TestLog::get());

    // Send a response back in the other direction.
    message.reset();
    message.appendExternal("response", 8);
    Buffer::Iterator iterator2(&message);
    server.sendPacket(sender, "h:", 2, &iterator2);
    EXPECT_STREQ("h:response", receivePacket(client));
    delete serverAddress;
}

TEST_F(InfUdDriverTest, gbsOption) {
    Cycles::mockCyclesPerSec = 2e09;
    ServiceLocator serverLocator("basic+infud:gbs=40");
    InfUdDriver driver(&context, &serverLocator, false);
    EXPECT_EQ(2.5, driver.queueEstimator.bandwidth);
    EXPECT_EQ(40, driver.bandwidthGbps);
    EXPECT_EQ(10000u, driver.maxTransmitQueueSize);

    ServiceLocator serverLocator2("basic+infud:gbs=1");
    InfUdDriver driver2(&context, &serverLocator2, false);
    EXPECT_EQ(4016u, driver2.maxTransmitQueueSize);
    Cycles::mockCyclesPerSec = 0;
}

}  // namespace RAMCloud
