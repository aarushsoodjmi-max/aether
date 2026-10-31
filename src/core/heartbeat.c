#include <stdio.h>
#include <unistd.h>

int main(void)
{
    puts("heartbeat: service started");
    fflush(stdout);

    for (;;)
        sleep(60);
}
