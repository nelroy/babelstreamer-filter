// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  ServerLink.hpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude  Code (Sonnet)
//
///  Abstract: one server connection  shared between all running filter instances. The module
///         handles acquisition of a session token and changes of streaming status.
///         Filters must register with the ServerLink to make use of it. The actual object
///         is implemented as a singleton.
//
//=======================================================================

#pragma once

#include <obs-module.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using ServerLinkId = uint64_t;

/** What one registered filter contributes to the shared connection. */
struct ServerLinkParams
{
	std::string host;                   /*! BabelStreamer host name - master only*/
	uint16_t port = 0;                  /*! Port to access host - master only */
	bool useTls = false;                /*! use secure TLS link - master only */
	std::string deviceToken;            /*! device token to link to account - given by ServerLink*/
	bool streamGateEnabled = true;      /*! only connect when streaming if true */
	obs_source_t *source = nullptr;     /*! OBS source */
	std::function<void()> clearOverlay; /*! function to clear the text overlay */
};

class ServerLink
{
public:
    /**
     Register a filter with the shared connection.
     
     @param p ServerLinkParams structure
     
     @returns a server link id, used to identify the filter for the server link
     */
    ServerLinkId serverLinkAdd(ServerLinkParams p);
    
    /**
     Update settings on a filter registered to the server link. Ignored
     of id not previously registered
     
     @param id filter id (previous registered @see serverLinkAdd)
     @param p new server link parameters
     */
    void serverLinkUpdate(ServerLinkId id, ServerLinkParams p);
    
    /**
     Unregister a filter - ignored if filter not previously registered
     
     @param id filter id
     */
    void serverLinkRemove(ServerLinkId id);
    
    /**
     Activate the server link for this id. Ignore if filter not registered
     
     @param id filter id
     */
    void serverLinkSetActive(ServerLinkId id, bool active);
    
    /**
     Enqueue a JSON string for dispatch to the sender thread. Non blocking on I/O
     
     @param json string to enqueue
     */
    void serverLinkEnqueue(std::string json);
    
    /**
     Streamgate allows transcription when not streaming. This method hangs up the
     connection if gating applies and OBS isn't live
     */
    void serverLinkApplyStreamGate();
    
    /*
     Update the isStreaming flag which, together with the streamgate
     determines if we should be connecting to the server
     
     @param live if true then streaming
     */
    void setIsStreaming (bool live);
    
    /**
     Getter for the streaming flag set in @see setIsStreaming
     
     @returns status of flag
     */
    bool serverLinkIsStreaming();
    
    /**
     Get the current sever-provided babelstreamer  session key

     @returns session key, empty if non
     */
    std::string serverLinkSessionKey();

    /**
     How many registered filters are currently active (enabled), not just
     configured. Used to decide whether per-source speaker attribution is
     meaningful.

     @returns count of active filters
     */
    size_t serverLinkActiveCount();
    
    /**
     Implementation of singleton class for external users to get the instance
     
     @returns pointer to ServerLink object singleton
     
     */
    static ServerLink &getInstance();
    
private:
    /*
        make constructor private to implement singleton. Constructor and
        destructor are defined in ServerLink.cpp, where Pimpl is complete.
     */
    ServerLink();
    ~ServerLink();
    ServerLink(const ServerLink &) = delete;
    ServerLink &operator=(const ServerLink &) = delete;

    // Pimpl pattern for bulk of object. Held through a pointer so the
    // header only needs the forward declaration.
    class Pimpl;
    std::unique_ptr<Pimpl> pimpl;
};
