# PYBL firmware

[`pybl_firmware.ino`](pybl_firmware.ino) is the current Arduino firmware snapshot for the PYBL Meditation Pebble prototype.

## Target hardware

- Seeed XIAO nRF52840
- DRV2605L haptic driver
- Linear resonant actuator
- TTP223 touch input
- Small LiPo battery

## Arduino dependencies

- `Wire`
- `Adafruit_DRV2605`
- nRF52 power support providing `nrfx_power.h`

## Development status

The source is being shared as work in progress. It documents the current breathing-envelope, touch, battery-monitoring and sleep approach, but it has not been presented as a stable release or production-ready safety implementation.
