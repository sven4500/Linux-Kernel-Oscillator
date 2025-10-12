#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// NOTE:
// https://embetronicx.com/tutorials/linux/device-drivers/ioctl-tutorial-in-linux/
#define MYDEVMAGIC 's'
#define CMDADDWAVE _IOW(MYDEVMAGIC, 0, uint32_t)
#define CMDREMOVEWAVE _IOW(MYDEVMAGIC, 1, uint32_t)

// NOTE: амплитуда 7 бит, фаза 9 бит, частота 16 бит
#define MAKEWAVE(amp, phase, freq) \
    (((amp)&0x7f) | (((phase)&0x1ff) << 7) | (((freq)&0xffff) << 16))

#define GETWAVEAMP(w) ((w)&0x7f)
#define GETWAVEPHASE(w) (((w) >> 7) & 0x1ff)
#define GETWAVEFREQ(w) (((w) >> 16) & 0xffff)

#define SETWAVEAMP(wave, amp) (((wave)&0xFFFFFF80) | ((amp)&0x7f))
#define SETWAVEPHASE(wave, phase) (((wave)&0xFFFF007F) | (((phase)&0x1ff) << 7))
#define SETWAVEFREQ(wave, freq) (((wave)&0x0000FFFF) | (((freq)&0xffff) << 16))

// NOTE:
// https://stackoverflow.com/questions/16758422/is-there-a-non-fatal-equivalent-to-assert-in-c
#define expect(value)                                                 \
    do {                                                              \
        if (!(value)) {                                               \
            fprintf(stderr, "%s failed in %s:%d\n", #value, __FILE__, \
                    __LINE__);                                        \
        }                                                             \
    } while (0)

int main(int argc, char **argv) {
    int fd, loop = 1;
    uint32_t wave;

    fd = open("/dev/ksound_device", O_RDWR);
    if (fd < 0) {
        printf("failed to open device file %d\n", fd);
        return -1;
    }

    // NOTE: проверка что макросы работают праильно
    wave = MAKEWAVE(87, 319, 41980);

    expect(GETWAVEAMP(wave) == 87);
    expect(GETWAVEPHASE(wave) == 319);
    expect(GETWAVEFREQ(wave) == 41980);

    wave = SETWAVEAMP(wave, 49);
    wave = SETWAVEPHASE(wave, 187);
    wave = SETWAVEFREQ(wave, 21953);

    expect(GETWAVEAMP(wave) == 49);
    expect(GETWAVEPHASE(wave) == 187);
    expect(GETWAVEFREQ(wave) == 21953);

    while (loop) {
        char cmd = 'q';

        // NOTE:
        // https://stackoverflow.com/questions/2507082/getc-vs-getchar-vs-scanf-for-reading-a-character-from-stdin
        // NOTE:
        // https://stackoverflow.com/questions/58294019/leading-whitespace-when-using-scanf-with-c
        printf("input command (a, r, q): ");
        scanf(" %c",
              &cmd);  // пробел - пропустить все не печатные символы в начале
        // cmd = getchar();

        if (cmd == 'a') {
            int amp, phase, freq;
            uint32_t wave;

            scanf("%d %d %d", &amp, &phase, &freq);
            printf("cmd=\"%c\", amp=%d, phase=%d, freq=%d\n", cmd, amp, phase,
                   freq);

            wave = MAKEWAVE(amp, phase, freq);
            ioctl(fd, CMDADDWAVE, &wave);

            expect(GETWAVEAMP(wave) == amp);
            expect(GETWAVEPHASE(wave) == phase);
            expect(GETWAVEFREQ(wave) == freq);
        } else if (cmd == 'r') {
            int freq;

            scanf("%d", &freq);
            printf("cmd=\"%c\", freq=%d\n", cmd, freq);

            ioctl(fd, CMDREMOVEWAVE, &freq);
        } else if (cmd == 'q') {
            loop = 0;
        }
    }

    close(fd);
    return 0;
}
