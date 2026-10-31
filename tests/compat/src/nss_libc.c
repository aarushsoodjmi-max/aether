#define _GNU_SOURCE
#include "t.h"
#include <pwd.h>
#include <grp.h>
#include <netdb.h>
#include <locale.h>
#include <iconv.h>
#include <langinfo.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
int main(void) {
    struct passwd *pw = getpwnam("root"); CHECK(pw && pw->pw_uid == 0 && strcmp(pw->pw_dir, "/root") == 0);   /* libnss_files via nsswitch.conf */
    CHECK(getpwuid(0) != NULL); struct group *gr = getgrnam("root"); CHECK(gr && gr->gr_gid == 0);
    char hn[256]; CHECK(gethostname(hn, sizeof hn) == 0 && hn[0]);
    struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res; int rc = getaddrinfo("localhost", "80", &hints, &res); if (rc) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc)); return 1; }
    CHECK(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK)); freeaddrinfo(res);
    CHECK(setlocale(LC_ALL, "C") != NULL && strcmp(nl_langinfo(CODESET), "ANSI_X3.4-1968") == 0);
    iconv_t cd = iconv_open("ISO-8859-1", "UTF-8"); if (cd == (iconv_t)-1) { fprintf(stderr, "iconv_open failed: %s (gconv modules missing?)\n", strerror(errno)); return 1; }
    char in[] = "caf\xc3\xa9", out[16] = {0}; char *ip = in, *op = out; size_t il = strlen(in), ol = sizeof out - 1;
    CHECK(iconv(cd, &ip, &il, &op, &ol) != (size_t)-1); CHECK((unsigned char)out[3] == 0xE9 && out[4] == 0); iconv_close(cd);
    unsetenv("TZ"); tzset(); time_t t = 86400 * 365; struct tm tmv; CHECK(localtime_r(&t, &tmv) != NULL); CHECK(tmv.tm_year == 71 && tmv.tm_hour == 0);   /* no tzdata => UTC */
    char sb[64]; CHECK(strftime(sb, sizeof sb, "%Y-%m-%d %H:%M:%S", gmtime_r(&t, &tmv)) > 0 && strcmp(sb, "1971-01-01 00:00:00") == 0);
    CHECK(fabs(sin(M_PI / 2) - 1.0) < 1e-12 && fabs(pow(2.0, 10.0) - 1024.0) < 1e-9 && fabs(sqrt(2.0) * sqrt(2.0) - 2.0) < 1e-12);
    char *e = NULL; CHECK(asprintf(&e, "%05.1f|%x|%s", 3.14159, 255u, "z") > 0 && strcmp(e, "003.1|ff|z") == 0); free(e);
    DONE("nss_libc");
}
