#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchSignalPath(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "updateSignalPathNodeParam")
    {
        c.HandleUpdateSignalPathNodeParamRequest(msg);
        return true;
    }
    if (type == "updateSignalPathNodeBypass")
    {
        c.HandleUpdateSignalPathNodeBypassRequest(msg);
        return true;
    }
    if (type == "updateSignalPathNodeConfig")
    {
        c.HandleUpdateSignalPathNodeConfigRequest(msg);
        return true;
    }
    if (type == "updateNodeResource")
    {
        c.HandleUpdateNodeResourceRequest(msg);
        return true;
    }
    if (type == "browseNodeResource")
    {
        c.HandleBrowseNodeResourceRequest(msg);
        return true;
    }
    if (type == "addSignalPathNode")
    {
        c.HandleAddSignalPathNodeRequest(msg);
        return true;
    }
    if (type == "splitSignalPathEdge")
    {
        c.HandleSplitSignalPathEdgeRequest(msg);
        return true;
    }
    if (type == "collapseSignalPathSplit")
    {
        c.HandleCollapseSignalPathSplitRequest(msg);
        return true;
    }
    if (type == "replaceSignalPathNode")
    {
        c.HandleReplaceSignalPathNodeRequest(msg);
        return true;
    }
    if (type == "reorderSignalPathNode")
    {
        c.HandleReorderSignalPathNodeRequest(msg);
        return true;
    }
    if (type == "deleteSignalPathNode")
    {
        c.HandleDeleteSignalPathNodeRequest(msg);
        return true;
    }
    if (type == "setNodeEnabled")
    {
        c.HandleSetNodeEnabledRequest(msg);
        return true;
    }
    if (type == "setNodeParam")
    {
        c.HandleSetNodeParamRequest(msg);
        return true;
    }
    if (type == "loadNodeResource")
    {
        c.HandleLoadNodeResourceRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
