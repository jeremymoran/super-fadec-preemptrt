# RASPBERRY PI 4B FADEC

## Compatible Boards and Firmware
**ADS131M04 4-Channel 24-bit ADC DAQ Board**
**Version:** v0.5 (2026-05-18)  
**MCU:** Raspberry Pi Pico 2 (RP2350)  
**ADC:** Texas Instruments ADS131M04

## ADC Board Real-Time Data Port (RTDP) Implementation
The **Real-Time Data Port** is a dedicated low-latency path that lets an external master device poll the **latest ADC sample set** from the Pico while the main acquisition loop continues logging independently on `core0`.

In the current setup, the **Pico acts as the SPI slave** and the **Raspberry Pi 4B FADEC acts as the SPI master**. The Pi watches `DATA_RDY`, asserts `CSn`, waits briefly for the slave/transceiver path to settle, and then clocks out one RTDP frame.

### Key Features
- Runs RTDP service on `core1` so the main sampling/logging loop on `core0` is left alone.
- Uses **ping-pong DMA buffers** so the newest sample set can be handed to the SPI slave path with minimal CPU work.
- Exposes the **freshest 4-channel ADC sample set** rather than the full logging buffer.
- Data format is **13 bytes per frame**: `0xEB` sync header + `4 × 24-bit signed` channel values, big-endian.
- Uses timeout protection so the Pico can recover if the master does not respond.
- Drains stale SPI state between polls and de-initialises SPI if the master disappears.
- Enabled/disabled via virtual register 7 (`vregister[7]` = RTDP timeout / advertising window in µs).

### Hardware Pins & Required Circuitry
| RP2350 GPIO | Pin Name       | Direction | Function                              | Notes |
|-------------|----------------|-----------|---------------------------------------|-------|
| 17          | CSn            | Input     | Chip Select (GPIO-direct)             | Pulled up on Pico2 |
| 18          | SCK            | Input     | Serial Clock  (via RS485 bus)         | Pulled up on Pico2 |
| 19          | TX             | Output    | Data output (via RS485 bus)           | RTDP serial data |
| 27          | DRDY           | Output    | New data available (GPIO-direct)      | Goes high when fresh RTDP frame is ready |
| 20          | RTDP_DE        | Output    | RS-485 Driver Enable                  | High = transmit |
| 21          | RTDP_REn       | Output    | RS-485 Receiver Enable (active low)   | Low = receiver enabled |

### FADEC External Hardware
- Full-duplex RS-485/RS-422 transceiver on each slave return path
  - Pico GPIO19 -> DI (driver input)
  - Transceiver A/B differential pair -> FADEC master
  - DE -> GPIO20
  - /RE -> GPIO21

The Pi-side software does not depend on a specific transceiver part number.
It assumes only that the slave-side driver enable is active-high and the
receiver enable is active-low, matching the checked-in RP2350 firmware.

**SPI Mode:** Mode 3 (CPOL=1, CPHA=1)  
**Clock/Data transport:** `SCK` and returned RTDP data both pass through RS-485 line drivers  
**Checked-in RTDP default:** `RTDP_SPI_HZ = 2 MHz` in the shared `rtdp.h` config header

### Transport Timing Through The Line Drivers

The line-driver headline data rate does **not** translate directly into a safe
SPI clock frequency. In this RTDP design the line drivers are carrying two
timing-related paths:

- the Pi-generated `SCK` path to the Pico slave
- the Pico-returned RTDP data path back to the Pi

Those two paths do not have exactly the same delay. Even when both waveforms
look clean on a logic analyser, the difference in propagation delay, skew, and
driver-enable behaviour between the clock path and the data path reduces the
setup/hold margin seen by the SPI receiver.

That matters much more for SPI than for asynchronous serial links:

- a UART-style "10 Mbps" part only has to move bits quickly enough
- SPI must deliver data with the correct timing **relative to the sampling edge of the clock**

In practice, the usable RTDP SPI clock can be much lower than the nominal
line-rate number, especially when:

- `SCK` and returned data use separate transceiver channels
- the bus has stubs or shared-node loading
- cable length, termination, or biasing are not ideal
- the master samples close to the edge of the available timing window

The checked-in transport clock is `2 MHz` via `RTDP_SPI_HZ` in `rtdp.h`.
Higher rates must be revalidated on the actual transceiver, cable, and loading
combination in use.

### RTDP Frame Format

Each RTDP response is exactly **13 bytes**:

| Byte(s) | Meaning |
|---------|---------|
| 0 | Sync header = `0xEB` |
| 1..3 | Channel 0, signed 24-bit, big-endian |
| 4..6 | Channel 1, signed 24-bit, big-endian |
| 7..9 | Channel 2, signed 24-bit, big-endian |
| 10..12 | Channel 3, signed 24-bit, big-endian |

The Pi validates byte 0 against `0xEB`. Frames with any other first byte are treated as misaligned or invalid and are dropped.

### How the RTDP Works

**Core 0 (sampling loop):**
- Every new ADC sample set is stored in the main circular logging buffer.
- If RTDP is enabled and Core 1 is idle, Core 0 packs the newest sample set into the next RTDP ping-pong buffer.
- The packed buffer format is:
  - byte 0 = `0xEB`
  - bytes 1..12 = 4 signed 24-bit ADC values, big-endian
- Core 0 then pushes the ready buffer index to Core 1 via the inter-core FIFO.

**Core 1 (RTDP service):**
1. Waits for a buffer index from Core 0.
2. (Re)initialises SPI0 as a **Mode 3 slave** if needed.
3. Drains any stale SPI RX FIFO state from previous traffic.
4. Configures TX DMA for the full 13-byte frame.
5. Configures RX DMA to drain the receive side so the full-duplex SPI peripheral does not stall.
7. Asserts **DATA_RDY**.
8. Waits for the external master to pull **CSn** low.
9. Enables the slave transceiver driver only after **CSn** goes low, so the shared return bus is not driven early.
10. Waits for the full transfer and for **CSn** to return high.
11. Drains residual SPI state, disables the transceiver, clears **DATA_RDY**, and returns to idle.

If the external master does **not** respond within the timeout (`vregister[7]`), the Pico aborts the transfer, cleans up the SPI state, and waits for the next sample.

### Timing Notes

- The first byte is the most timing-sensitive part of the transfer.
- A clean-looking `SCK` waveform is not sufficient; what matters is the relative
  timing of `SCK` and returned data at the Pi input after both line-driver paths.
- The Pico stages the RTDP frame before clocking begins, but higher SPI clocks can
  still fail if the returned data arrives too late relative to the Pi sampling edge.
- The Pi FADEC code inserts a configurable CS-setup delay before starting `SCK`.
  The checked-in default is `20 us`, and it can be overridden at runtime with
  `FADEC_CS_SETUP_DELAY_NS`.
- That delay only covers slave select and driver turn-on. It does **not** remove
  propagation skew between the RS-485 clock path and the RS-485 data-return path.
- If header bytes begin to corrupt at higher speeds, treat that as a transport-margin
  problem first, not as an ADC or packet-format problem.

### What RTDP Is And Is Not

RTDP is the **live-view / latest-sample path**. It is used when the external Pi wants the most recent ADC values quickly and repeatedly.

RTDP is **not** the main bulk-recording path. The normal logging loop still writes to SRAM independently, and RTDP simply exposes the newest sample set in parallel.

### Example External Master Sequence

```c
// Pseudo-code for external device
while (true) {
    if (DATA_RDY == HIGH) {
        pull CSn LOW;
        delay_us(cs_setup_margin_us); // allow the selected transport path to settle
        uint8_t rx[13];
        spi_transfer(rx, 13);   // dummy TX bytes are ignored by the slave
        release CSn;

        if (rx[0] == 0xEB) {
            process_live_data(rx);  // decode 4 × signed 24-bit big-endian values
        }
    }
}
```
