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

#define MYDEV_MAGIC 's'
#define CMD_ADDWAVE _IOW(MYDEV_MAGIC, 0, uint32_t)
#define CMD_REMOVEWAVE _IOW(MYDEV_MAGIC, 1, uint32_t)

// амплитуда 7 бит, фаза 9 бит
#define MAKEWAVE(a, p, f) (((a) & 0x7f) | (((p) & 0x1ff) << 7) | (((f) & 0xffff) << 16))

int main(int argc, char** argv)
{
	int fd;
	uint32_t wave;

	fd = open("/dev/ksound_device", O_RDWR);
	if (fd < 0)
	{
		printf("failed to open device file %d\n", fd);
		return -1;
	}

	wave = MAKEWAVE(100, 0, 520);
	ioctl(fd, CMD_ADDWAVE, &wave);

	wave = 520;
	ioctl(fd, CMD_REMOVEWAVE, &wave);

	close(fd);
	return 0;
}
