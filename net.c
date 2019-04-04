#include "sandals.h"
#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

void configure_net(const struct sandals_request *request) {

    int s;
    struct ifreq ifr = { .ifr_name = "lo"};

    // bring lo interface up
    if ((s = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0)) == -1)
        fail(kStatusInternalError,
            "socket: %s", strerror(errno));

    if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1
        || (ifr.ifr_flags |= IFF_UP|IFF_RUNNING,
            ioctl(s, SIOCSIFFLAGS, &ifr) == -1
    )) fail(kStatusInternalError,
        "Enabling loopback network interface: %s", strerror(errno));

    close(s);

    // set hostname & domainname
    if (sethostname(request->host_name, strlen(request->host_name)) == -1)
        fail(kStatusInternalError, "sethostname: %s", strerror(errno));

    if (setdomainname(request->domain_name, strlen(request->domain_name)) == -1)
        fail(kStatusInternalError, "setdomainname: %s", strerror(errno));
}
