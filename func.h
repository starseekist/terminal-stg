#ifndef FUNC_H
#define FUNC_H

#include <sys/time.h>

int min(int a, int b) {
    return a < b ? a : b;
}

int max(int a, int b) {
    return a > b ? a : b;
}

bool probability(int x, int y) {
    return (rand() % y) <= (x - 1);
}

uint64_t myclock() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

char* sformat(const char* format, ...) {
    static char text[1024];

    va_list ap;
    va_start(ap, format);
    int len = vsprintf(text, format, ap);
    va_end(ap);

    if (len >= (int)sizeof(text))
        eprintf("buffer overflow\n");

    return text;
}

#endif
