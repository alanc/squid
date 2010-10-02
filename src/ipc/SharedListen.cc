/*
 * $Id$
 *
 * DEBUG: section 54    Interprocess Communication
 *
 */

#include "config.h"
#include "base/TextException.h"
#include "comm.h"
#include "comm/Connection.h"
#include "ipc/Port.h"
#include "ipc/Messages.h"
#include "ipc/Kids.h"
#include "ipc/TypedMsgHdr.h"
#include "ipc/StartListening.h"
#include "ipc/SharedListen.h"

#include <map>

/// holds information necessary to handle JoinListen response
class PendingOpenRequest
{
public:
    Ipc::OpenListenerParams params; ///< actual comm_open_sharedListen() parameters
    AsyncCall::Pointer callback; // who to notify
};

/// maps ID assigned at request time to the response callback
typedef std::map<int, PendingOpenRequest> SharedListenRequestMap;
static SharedListenRequestMap TheSharedListenRequestMap;

static int
AddToMap(const PendingOpenRequest &por)
{
    // find unused ID using linear seach; there should not be many entries
    for (int id = 0; true; ++id) {
        if (TheSharedListenRequestMap.find(id) == TheSharedListenRequestMap.end()) {
            TheSharedListenRequestMap[id] = por;
            return id;
        }
    }
    assert(false); // not reached
    return -1;
}

Ipc::OpenListenerParams::OpenListenerParams()
{
    xmemset(this, 0, sizeof(*this));
}

bool
Ipc::OpenListenerParams::operator <(const OpenListenerParams &p) const
{
    if (sock_type != p.sock_type)
        return sock_type < p.sock_type;

    if (proto != p.proto)
        return proto < p.proto;

    // ignore flags and fdNote differences because they do not affect binding

    return addr.compareWhole(p.addr) < 0;
}



Ipc::SharedListenRequest::SharedListenRequest(): requestorId(-1), mapId(-1)
{
    // caller will then set public data members
}

Ipc::SharedListenRequest::SharedListenRequest(const TypedMsgHdr &hdrMsg)
{
    hdrMsg.getData(mtSharedListenRequest, this, sizeof(*this));
}

void Ipc::SharedListenRequest::pack(TypedMsgHdr &hdrMsg) const
{
    hdrMsg.putData(mtSharedListenRequest, this, sizeof(*this));
}


Ipc::SharedListenResponse::SharedListenResponse(const Comm::ConnectionPointer &c, int anErrNo, int aMapId):
        conn(c), errNo(anErrNo), mapId(aMapId)
{
}

Ipc::SharedListenResponse::SharedListenResponse(const TypedMsgHdr &hdrMsg):
        conn(NULL), errNo(0), mapId(-1)
{
    hdrMsg.getData(mtSharedListenResponse, this, sizeof(*this));
    conn = new Comm::Connection;
    conn->fd = hdrMsg.getFd();
    // other conn details are passed in OpenListenerParams and filled out by SharedListenJoin()
}

void Ipc::SharedListenResponse::pack(TypedMsgHdr &hdrMsg) const
{
    hdrMsg.putData(mtSharedListenResponse, this, sizeof(*this));
    hdrMsg.putFd(conn->fd);
}


void Ipc::JoinSharedListen(const OpenListenerParams &params,
                           AsyncCall::Pointer &callback)
{
    PendingOpenRequest por;
    por.params = params;
    por.callback = callback;

    SharedListenRequest request;
    request.requestorId = KidIdentifier;
    request.params = por.params;
    request.mapId = AddToMap(por);

    debugs(54, 3, HERE << "getting listening FD for " << request.params.addr <<
           " mapId=" << request.mapId);

    TypedMsgHdr message;
    request.pack(message);
    SendMessage(coordinatorAddr, message);
}

void Ipc::SharedListenJoined(const SharedListenResponse &response)
{
    Comm::ConnectionPointer c = response.conn;

    // Dont debugs c fully since only FD is filled right now.
    debugs(54, 3, HERE << "got listening FD " << c->fd << " errNo=" <<
           response.errNo << " mapId=" << response.mapId);

    Must(TheSharedListenRequestMap.find(response.mapId) != TheSharedListenRequestMap.end());
    PendingOpenRequest por = TheSharedListenRequestMap[response.mapId];
    Must(por.callback != NULL);
    TheSharedListenRequestMap.erase(response.mapId);

    if (Comm::IsConnOpen(c)) {
        OpenListenerParams &p = por.params;
        c->local = p.addr;
        c->flags = p.flags;
        // XXX: leave the comm AI stuff to comm_import_opened()?
        struct addrinfo *AI = NULL;
        p.addr.GetAddrInfo(AI);
        AI->ai_socktype = p.sock_type;
        AI->ai_protocol = p.proto;
        comm_import_opened(c, FdNote(p.fdNote), AI);
        p.addr.FreeAddrInfo(AI);
    }

    StartListeningCb *cbd = dynamic_cast<StartListeningCb*>(por.callback->getDialer());
    Must(cbd);
    cbd->conn = c;
    cbd->errNo = response.errNo;
    cbd->handlerSubscription = por.params.handlerSubscription;
    ScheduleCallHere(por.callback);
}
