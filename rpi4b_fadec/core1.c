#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <gpiod.h>   // libgpiod v2

#define SPI_DEVICE  "/dev/spidev0.0"
#define SPI_HZ      10000000   // 10MHz
#define N_DEVICES   1

// Per-device file descriptors and GPIO lines
typedef struct {
    int         spi_fd;
    struct gpiod_line_request* drdy_line;
    struct gpiod_line_request* cs_line;   // if not using SPI native CS
    uint8_t     id;
} device_ctx_t;

void* acquisition_thread(void* arg)
{
    spsc_ring_t* ring = (spsc_ring_t*)arg;

    // 1. Pin to core 1
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    // 2. Set SCHED_FIFO at max priority — preempts everything on this core
    struct sched_param sp = { .sched_priority = 99 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    // 3. Lock all memory — prevent page faults during acquisition
    mlockall(MCL_CURRENT | MCL_FUTURE);

    // 4. Pre-fault stack pages (forces pages into RAM now, not during loop)
    uint8_t stack_prefault[65536];
    memset(stack_prefault, 0, sizeof(stack_prefault));

    // 5. Open SPI device once
    int spi_fd = open(SPI_DEVICE, O_RDWR);
    uint32_t speed = SPI_HZ;
    uint8_t mode = SPI_MODE_1;    // CPOL=0, CPHA=1 — matches RP2350 config
    uint8_t bits = 8;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    // 6. Pre-allocate SPI transfer struct — never allocate in hot loop
    uint8_t rx_buf[N_DEVICES][12]; // 4ch × 3 bytes
    struct spi_ioc_transfer xfer[N_DEVICES];
    memset(xfer, 0, sizeof(xfer));
    for (int i = 0; i < N_DEVICES; i++) {
        xfer[i].rx_buf        = (uint64_t)rx_buf[i];
        xfer[i].len           = 12;
        xfer[i].speed_hz      = SPI_HZ;
        xfer[i].bits_per_word = 8;
        xfer[i].cs_change     = 0;
        // tx_buf = 0 → kernel sends zeros, fine for slave-read
    }

    // 7. Hot loop — no allocations, no syscalls except ioctl + clock_gettime
    uint32_t drdy_state_prev = 0;
    while (1) {
        // Read all DRDY GPIO lines in a single register read.
        // Requires DRDY lines on same GPIO chip, consecutive pins.
        // e.g. GPIO 5-12 for 8 devices.
        uint32_t drdy_state = read_drdy_register(); // see below

        // Edge detect: only act on DRDY going high (new data available)
        uint32_t new_data = drdy_state & ~drdy_state_prev;
        drdy_state_prev = drdy_state;

        if (!new_data) {
            // No new data — yield briefly to avoid starving Core 0 watchdogs
            // asm volatile("yield"); for ARM — lighter than sched_yield()
            asm volatile("yield");
            continue;
        }

        // Service each device that has new data
        for (int dev = 0; dev < N_DEVICES; dev++) {
            if (!(new_data & (1u << dev))) continue;

            // Assert CS for this device
            assert_cs(dev);   // gpiod line set, or direct register write

            // SPI transfer — 12 bytes in ~9.6µs at 10MHz
            ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer[dev]);

            // Deassert CS
            deassert_cs(dev);

            // Timestamp immediately after transfer
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);

            // Unpack 24-bit big-endian samples
            sample_frame_t frame;
            frame.device_id    = dev;
            frame.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            for (int ch = 0; ch < 4; ch++) {
                uint8_t* b = &rx_buf[dev][ch * 3];
                int32_t s = ((int32_t)b[0] << 16) |
                            ((int32_t)b[1] <<  8) |
                             (int32_t)b[2];
                if (s & 0x800000) s |= 0xFF000000; // sign extend
                frame.channels[ch] = s;
            }

            // Push to ring — if full, drop (main thread is too slow)
            if (ring_push(ring, &frame) < 0) {
                // increment dropped_frames counter (atomic, for diagnostics)
                atomic_fetch_add(&dropped_frames, 1);
            }
        }
    }
    return NULL;
}