// The transport where the platform offers none this code uses (Android, and Linux unless the
// product turned on GUITARFX_UICLIENT_CURL): every request answers status 0 and says why. A
// product with an HTTP stack of its own hands that in instead (HttpTransport).

#include "net/Http.h"

namespace guitarfx::net
{
namespace
{
class NoTransport final : public HttpTransport
{
  public:
    HttpResponse Perform(const HttpRequest&, CancelToken&) override
    {
        HttpResponse response;
        response.error = "Networking is not available in this build";
        return response;
    }
};
} // namespace

std::shared_ptr<HttpTransport> MakePlatformTransport()
{
    return std::make_shared<NoTransport>();
}
} // namespace guitarfx::net
