#pragma once

#include <Server/IServer.h>
#include <Server/HTTP/HTTPRequestHandler.h>
#include <Server/HTTP/HTTPRequestHandlerFactory.h>
#include <Coordination/KeeperDispatcher.h>

namespace DB
{

class Context;
class IServer;

class KeeperReadinessHandler : public HTTPRequestHandler, WithContext
{
private:
    std::shared_ptr<KeeperDispatcher> keeper_dispatcher;

public:
    explicit KeeperReadinessHandler(std::shared_ptr<KeeperDispatcher> keeper_dispatcher_)
        : keeper_dispatcher(keeper_dispatcher_)
    {
    }

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;
};

HTTPRequestHandlerFactoryPtr
createKeeperHTTPControlMainHandlerFactory(
    const Poco::Util::AbstractConfiguration & config,
    std::shared_ptr<KeeperDispatcher> keeper_dispatcher,
    const std::string & name);

}
