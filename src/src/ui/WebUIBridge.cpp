#include "WebUIBridge.h"

#include <filesystem>
#include <sstream>

#include "IControls.h"
#include "IWebViewControl.h"

namespace namguitar
{

namespace
{
std::string EscapeForJavaScriptString(const std::string& input)
{
  std::string escaped;
  escaped.reserve(input.size() + 16);
  for (char c : input)
  {
    switch (c)
    {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}
} // namespace

void WebUIBridge::Initialize(iplug::igraphics::IGraphics& graphics, const std::filesystem::path& resourceRoot)
{
  using namespace iplug::igraphics;

  const std::filesystem::path htmlPath = resourceRoot / "ui" / "index.html";
  IRECT bounds = graphics.GetBounds();

  mWebView = new IWebViewControl(
    bounds,
    true,
    [this, htmlPath](IWebViewControl*) {
      LoadWebContent(htmlPath);
    },
    [this](IWebViewControl*, const char* jsonMsg) {
      if (mHandler)
      {
        mHandler(jsonMsg ? jsonMsg : "");
      }
    },
    true, // enable dev tools
    false);

  graphics.AttachControl(mWebView);
}

void WebUIBridge::RegisterMessageHandler(MessageHandler handler)
{
  mHandler = std::move(handler);
}

void WebUIBridge::RegisterLogHandler(LogHandler handler)
{
  mLogger = std::move(handler);
}

void WebUIBridge::EnqueueMessage(const std::string& message)
{
  std::lock_guard<std::mutex> lock(mQueueMutex);
  mPendingMessages.push(message);
}

void WebUIBridge::PumpMessages()
{
  if (!mWebView)
  {
    return;
  }

  std::queue<std::string> messages;
  {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    std::swap(messages, mPendingMessages);
  }

  while (!messages.empty())
  {
    const auto& message = messages.front();
    std::string script = "window.postMessage(\"" + EscapeForJavaScriptString(message) + "\", '*');";
    mWebView->EvaluateJavaScript(script.c_str());
    if (mLogger)
    {
      mLogger("Sent UI message: " + message);
    }
    messages.pop();
  }
}

void WebUIBridge::LoadWebContent(const std::filesystem::path& htmlPath)
{
  if (!mWebView)
  {
    return;
  }

  if (!std::filesystem::exists(htmlPath))
  {
    if (mLogger)
    {
      mLogger("Failed to open UI html at " + htmlPath.generic_string());
    }
    return;
  }
  const std::string pathString = htmlPath.generic_string();
  mWebView->LoadFile(pathString.c_str());
}

} // namespace namguitar
