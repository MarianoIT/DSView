#include <log/xlog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int calls, failed;
static void receive(const char *data, int length)
{
    calls++;
    if (length <= 0 || length > 1000 || data[length - 1] != '\n' ||
        data[length] != '\0' || strncmp(data, "long-domain", 11)) failed = 1;
}
int main(void)
{
    char text[16384];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    xlog_context *context = xlog_new2(0);
    int receiver;
    if (!context || xlog_add_receiver(context, receive, &receiver)) return 1;
    xlog_writer *writer = xlog_create_writer(context, "long-domain-name-exceeding-storage");
    if (!writer) return 1;
    xlog_err(writer, "%s", text);
    xlog_err(writer, "%s", "short message");
    xlog_free_writer(writer);
    xlog_free(context);
    if (failed || calls != 2) return 1;
    puts("Long log messages and domains are bounded; short messages still delivered.");
    return 0;
}
