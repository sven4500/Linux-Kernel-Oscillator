#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// https://embetronicx.com/tutorials/linux/device-drivers/ioctl-tutorial-in-linux/

#define MYDEVMAGIC 's'
#define CMDADDWAVE _IOW(MYDEVMAGIC, 0, uint32_t)
#define CMDREMOVEWAVE _IOW(MYDEVMAGIC, 1, uint32_t)

// https://stackoverflow.com/questions/16758422/is-there-a-non-fatal-equivalent-to-assert-in-c
#define expect(value) \
    do \
    { \
        if (!(value)) \
        { \
            fprintf(stderr, "%s failed in %s:%d\n", #value, __FILE__, __LINE__); \
        } \
    } while(0)

// амплитуда 7 бит, фаза 9 бит, частота 16 бит
#define MAKEWAVE(amp, phase, freq) (((amp) & 0x7f) | (((phase) & 0x1ff) << 7) | (((freq) & 0xffff) << 16))
#define GETWAVEAMP(w) ((w) & 0x7f)
#define GETWAVEPHASE(w) (((w) >> 7) & 0x1ff)
#define GETWAVEFREQ(w) (((w) >> 16) & 0xffff)

int main(int argc, char** argv)
{
	int fd, loop = 1;
	uint32_t wave;

	fd = open("/dev/ksound_device", O_RDWR);
	if (fd < 0)
	{
		printf("failed to open device file %d\n", fd);
		return -1;
	}

	while (loop)
	{
		char cmd = 'q';

		// https://stackoverflow.com/questions/2507082/getc-vs-getchar-vs-scanf-for-reading-a-character-from-stdin
		printf("input command (a, r, q): ");
		scanf("%c", &cmd);
		//cmd = getchar();

		if (cmd == 'a')
		{
			int amp, phase, freq;
			uint32_t wave;

			scanf("%d %d %d", &amp, &phase, &freq);
			printf("cmd=\"%c\", amp=%d, phase=%d, freq=%d\n", cmd, amp, phase, freq);

			wave = MAKEWAVE(amp, phase, freq);
			ioctl(fd, CMDADDWAVE, &wave);

			expect(GETWAVEAMP(wave) == amp);
			expect(GETWAVEPHASE(wave) == phase);
			expect(GETWAVEFREQ(wave) == freq);
		}
		else if (cmd == 'r')
		{
			int freq;

			scanf("%d", &freq);
			printf("cmd=\"%c\", freq=%d\n", cmd, freq);

			ioctl(fd, CMDREMOVEWAVE, &freq);
		}
		else if (cmd == 'q')
		{
			loop = 0;
		}
	}

	close(fd);
	return 0;
}
