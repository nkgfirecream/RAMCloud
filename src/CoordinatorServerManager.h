/* Copyright (c) 2009-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_COORDINATORSERVERMANAGER_H
#define RAMCLOUD_COORDINATORSERVERMANAGER_H

#include <Client/Client.h>

#include "Common.h"
#include "CoordinatorServerList.h"
#include "ServerInformation.pb.h"
#include "ServerUpdate.pb.h"
#include "StateServerDown.pb.h"

namespace RAMCloud {

class CoordinatorService;

using LogCabin::Client::Entry;
using LogCabin::Client::EntryId;
using LogCabin::Client::NO_ID;

/**
 * Handles all server configuration details on behalf of the coordinator.
 * Provides an interface to the coordinator to service rpcs that modify
 * server configuration.
 */
class CoordinatorServerManager {

  PUBLIC:
    explicit CoordinatorServerManager(CoordinatorService& coordinatorService);
    ~CoordinatorServerManager();

    bool assignReplicationGroup(uint64_t replicationId,
                                const vector<ServerId>& replicationGroupIds);
    void createReplicationGroup();
    ServerId enlistServer(ServerId replacesId,
                          ServiceMask serviceMask,
                          const uint32_t readSpeed, const uint32_t writeSpeed,
                          const char* serviceLocator);
    void enlistServerRecover(ProtoBuf::ServerInformation* state,
                             EntryId entryId);
    void enlistedServerRecover(ProtoBuf::ServerInformation* state,
                               EntryId entryId);
    ProtoBuf::ServerList getServerList(ServiceMask serviceMask);
    bool hintServerDown(ServerId serverId);
    void removeReplicationGroup(uint64_t groupId);
    void sendServerList(ServerId serverId);
    void serverDown(ServerId serverId);
    void serverDownRecover(ProtoBuf::StateServerDown* state,
                           EntryId entryId);
    void setMinOpenSegmentId(ServerId serverId, uint64_t segmentId);
    void setMinOpenSegmentIdRecover(ProtoBuf::ServerUpdate* state,
                                    EntryId entryId);
    bool verifyServerFailure(ServerId serverId);


  PRIVATE:

    /**
     * Defines methods and stores data to enlist a server.
     */
    class EnlistServer {
      public:
          EnlistServer(CoordinatorServerManager &manager,
                       ServerId newServerId,
                       ServiceMask serviceMask,
                       const uint32_t readSpeed,
                       const uint32_t writeSpeed,
                       const char* serviceLocator)
              : manager(manager),
                newServerId(newServerId),
                serviceMask(serviceMask),
                readSpeed(readSpeed), writeSpeed(writeSpeed),
                serviceLocator(serviceLocator) {}
          ServerId execute();
          ServerId complete(EntryId entryId);

      private:
          /**
           * Reference to the instance of coordinator server manager
           * initializing this class.
           * Used to get access to CoordinatorService& service.
           */
          CoordinatorServerManager &manager;
          /**
           * The id assigned to the enlisting server.
           */
          ServerId newServerId;
    	  /**
    	   * Services supported by the enlisting server.
    	   */
          ServiceMask serviceMask;
          /**
    	   * Read speed of the enlisting server.
    	   */
          const uint32_t readSpeed;
    	  /**
    	   * Write speed of the enlisting server.
    	   */
          const uint32_t writeSpeed;
    	  /**
    	   * Service Locator of the enlisting server.
    	   */
          const char* serviceLocator;
          DISALLOW_COPY_AND_ASSIGN(EnlistServer);
    };

    /**
     * Defines methods and stores data to force a server out of the cluster.
     */
    class ServerDown {
        public:
            ServerDown(CoordinatorServerManager &manager,
                       ServerId serverId)
                : manager(manager), serverId(serverId) {}
            void execute();
            void complete(EntryId entryId);
        private:
            /**
             * Reference to the instance of coordinator server manager
             * initializing this class.
             * Used to get access to CoordinatorService& service.
             */
            CoordinatorServerManager &manager;
            /**
             * ServerId of the server that is suspected to be down.
             */
            ServerId serverId;
            DISALLOW_COPY_AND_ASSIGN(ServerDown);
    };

    /**
     * Defines methods and stores data to set minOpenSegmentId of server
     * with id serverId to segmentId.
     */
    class SetMinOpenSegmentId {
        public:
            SetMinOpenSegmentId(CoordinatorServerManager &manager,
                                ServerId serverId,
                                uint64_t segmentId)
                : manager(manager), serverId(serverId), segmentId(segmentId) {}
            void execute();
            void complete(EntryId entryId);
        private:
            /**
             * Reference to the instance of coordinator server manager
             * initializing this class.
             * Used to get access to CoordinatorService& service.
             */
            CoordinatorServerManager &manager;
            /**
             * ServerId of the server whose minOpenSegmentId will be set.
             */
            ServerId serverId;
            /**
             * The minOpenSegmentId to be set.
             */
            uint64_t segmentId;
            DISALLOW_COPY_AND_ASSIGN(SetMinOpenSegmentId);
    };

    /**
     * Reference to the coordinator service initializing this class.
     * Used to get access to the context, serverList and recoveryManager
     * in coordinator service.
     */
    CoordinatorService& service;

    /**
     * The id of the next replication group to be created. The replication
     * group is a set of backups that store all of the replicas of a segment.
     * NextReplicationId starts at 1 and is never reused.
     * Id 0 is reserved for nodes that do not belong to a replication group.
     */
    uint64_t nextReplicationId;

    /**
     * Used for testing only. If true, the HINT_SERVER_DOWN handler will
     * assume that the server has failed (rather than checking for itself).
     */
    bool forceServerDownForTesting;

    /**
     * Provides monitor-style protection for all operations in the
     * CoordinatorServerManger.
     * A Lock for this mutex must be held to read or modify any server
     * configuration.
     */
    mutable std::mutex mutex;
    typedef std::lock_guard<std::mutex> Lock;

    DISALLOW_COPY_AND_ASSIGN(CoordinatorServerManager);
};

} // namespace RAMCloud

#endif // RAMCLOUD_COORDINATORSERVERMANAGER_H