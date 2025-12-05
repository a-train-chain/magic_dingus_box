#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <libdrm/drm.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <drm_device>\n", argv[0]);
        return 1;
    }

    const char *drm_device = argv[1];
    int fd = open(drm_device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open DRM device");
        return 1;
    }

    printf("Dropping master access for %s\n", drm_device);

    // Try to drop master access
    if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) != 0) {
        perror("Failed to drop DRM master");
        close(fd);
        return 1;
    }

    printf("Successfully dropped DRM master access\n");
    close(fd);
    return 0;
}
