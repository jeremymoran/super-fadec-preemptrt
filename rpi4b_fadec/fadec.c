/*
fadec.c
Real-Time Data Port (RTDP) external master test - Raspberry Pi 4B

Monitors DATA_RDY from the ADS131M04 DAQ board.

2026-02-26 JM: Initial version
2026-05-17 JM: 

CORE AFFINITIES

Modified boot options:
# /boot/firmware/cmdline.txt
     isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2

core0, Linux kernel, IRQs, housekeeping (isolated from your app)
core1, DRDY polling + SPI acquisition (real-time, CPU-pinned)
core2, Main application / data processing (elevated priority)
core3, spare / Linux overflow / logging

HARDWARE PIN MAPPING
   DRDY1  >  GPIO 2   (ADC1 DATA READY - input, rising-edge event)
   MISO   >  GPIO 9   (SPI0 MISO       - input, data from ADC board via RS-485)
   CLK    >  GPIO 11  (SPI0 SCLK       - output, idles HIGH for SPI Mode 3)
   CS1    >  GPIO 13  (ADC1 CS         - output, active low)
   DE     >  GPIO 24  (RTDP_DE  - output, HIGH = RPi RS-485 driver enabled)
   REN    >  GPIO 25  (RTDP_REn - output, LOW  = RS-485 receiver enabled)

*/




void* main_application_thread(void* arg)
{
    spsc_ring_t* ring = (spsc_ring_t*)arg;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    // Lower RT priority than acquisition — can be preempted by core 1 work
    // but still isolated from Linux scheduler on core 2
    struct sched_param sp = { .sched_priority = 50 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    sample_frame_t frame;
    while (1) {
        if (ring_pop(ring, &frame) < 0) {
            asm volatile("yield");
            continue;
        }
        // Process frame — FFT, filter, log, network send, whatever
        process_sample(&frame);
    }
}