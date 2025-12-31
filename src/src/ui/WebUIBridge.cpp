#include "WebUIBridge.h"

#include <iostream> // For std::cout

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

  std::cout << "[WebUI] Initialize called" << std::endl;

  const std::filesystem::path htmlPath = resourceRoot / "ui" / "index.html";
  IRECT bounds = graphics.GetBounds();

  if (mLogger) {
    mLogger("Initializing WebView with bounds: " + std::to_string(bounds.W()) + "x" + std::to_string(bounds.H()));
    mLogger("HTML path: " + htmlPath.generic_string());
  }

  mWebView = new IWebViewControl(
    bounds,
    true,
    [this, htmlPath](IWebViewControl*) {
      if (mLogger) {
        mLogger("WebView ready callback triggered");
      }
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

  if (mLogger) {
    mLogger("WebView control created, attaching to graphics");
  }

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
    // Call IPlugReceiveData directly instead of postMessage since the UI expects it
    std::string script = "if (window.IPlugReceiveData) { window.IPlugReceiveData(\"" + EscapeForJavaScriptString(message) + "\"); }";
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
  std::cout << "[WebUI] LoadWebContent called" << std::endl;
  if (!mWebView)
  {
    if (mLogger) {
      mLogger("WebView is null, cannot load content");
    }
    return;
  }

  if (!std::filesystem::exists(htmlPath))
  {
    if (mLogger)
    {
      mLogger("Failed to open UI html at " + htmlPath.generic_string() + " - file does not exist");
    }
    return;
  }

  if (mLogger) {
    mLogger("HTML file found, loading: " + htmlPath.generic_string());
  }

  mHtmlPath = htmlPath;
  const std::string pathString = htmlPath.generic_string();
  mWebView->LoadFile(pathString.c_str());

  if (mLogger) {
    mLogger("LoadFile called successfully");
  }

  // Set up JavaScript bridge after a short delay to ensure content is loaded
  // Use a timer or just call it after LoadFile
  SetupJavaScriptBridge();
}

void WebUIBridge::SetupJavaScriptBridge()
{
  if (!mWebView)
  {
    return;
  }

  if (mLogger) {
    mLogger("Setting up JavaScript bridge");
  }

  // Set up the JavaScript bridge
  const std::string bridgeScript = R"(
    window.NAMBridge = {
      postMessage: function(message) {
        IPlugSendMsg(JSON.stringify(message));
      }
    };
  )";

  mWebView->EvaluateJavaScript(bridgeScript.c_str(), [this](const char* result) {
    if (mLogger) {
      mLogger("JavaScript bridge setup completed");
    }
  });
}

} // namespace namguitar
