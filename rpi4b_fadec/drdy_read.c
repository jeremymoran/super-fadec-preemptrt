
#include <sys/mman.h>
#include <fcntl.h>

// BCM2711 (Pi 4) GPIO base
#define GPIO_BASE       0xFE200000UL
#define GPIO_GPLEV0     (0x34 / 4)   // register offset for pin levels 0-31
#define BLOCK_SIZE      4096

volatile uint32_t* gpio_regs = NULL;

void init_gpio_mmap() {
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    gpio_regs = (volatile uint32_t*)mmap(NULL, BLOCK_SIZE,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    close(fd);
}

// Inline register read — zero syscall overhead
static inline uint32_t read_drdy_register()
{
    // Assume DRDY lines are on GPIO 5-12 (8 devices)
    // Single 32-bit register read, shift down to bit 0
    return (gpio_regs[GPIO_GPLEV0] >> 5) & 0xFF;
}

// Similarly for CS assert/deassert:
#define GPIO_GPSET0  (0x1C / 4)
#define GPIO_GPCLR0  (0x28 / 4)

static inline void assert_cs(int dev)
{
    // CS pins on GPIO 13-20 for 8 devices
    gpio_regs[GPIO_GPCLR0] = (1u << (13 + dev));
}

static inline void deassert_cs(int dev)
{
    gpio_regs[GPIO_GPSET0] = (1u << (13 + dev));
}